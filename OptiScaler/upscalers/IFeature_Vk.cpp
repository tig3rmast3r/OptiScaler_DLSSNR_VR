#include <pch.h>

#include "IFeature_Vk.h"
#include "State.h"
#include "nvsdk_ngx_vk.h"
#include <upscalers/IFeature_VkwDx12.h>
#include <inputs/NVNGX_DLSS.h>
#include <ofxr/ofxr_bridge_client.hpp>
#include <hooks/VulkanCaptureFeatures.h>
#include <hooks/LukeSourceTrace.h>
#include "../../third_party/ofxr/include/xrfg/bridge_flight_logger.hpp"

// No second SR/NR/postprocessing pipeline: native Vulkan DLSS evaluates first.
// The bridge only transports that exact completed color and its same-call guides.
class LukeVulkanCaptureBridge final : public IFeature_VkwDx12
{
    IFeature* _native;
  protected:
    IFeature* InteropFeature() override { return _native; }
    bool CapturesCompletedOutput() const override { return true; }
    bool InitInteropFeature(NVSDK_NGX_Parameter*) override {
        ofxr_bridge::ensure_d3d12_submission_hook(InteropDeviceOwner());
        LOG_INFO("V30065 Luke native Vulkan guide/color capture ready, stream={} queue={:p}",
            Handle()->Id, static_cast<void*>(InteropQueue()));
        return _native->IsInited();
    }
    bool EvaluateInteropFeature(ID3D12GraphicsCommandList* list,
                                 NVSDK_NGX_Parameter* parameters) override {
        static_cast<void>(ofxr_bridge::take_luke_origin_frame());
        LukeRossDlssIngress::PublishD3D12Guide(Handle()->Id, list, parameters, _native, InteropQueue());
        const HRESULT result = LukeRossDlssIngress::CompleteD3D12Color(list, parameters, _native, true);
        static std::atomic_uint64_t receipts{0};
        const auto receipt = receipts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (receipt <= 2 || receipt % 240 == 0) {
            const auto control = ofxr_bridge::snapshot();
            LOG_INFO("V30067 Vulkan AFW completion receipt={} stream={} nativeFrame={} result=0x{:X} embedded={} tracking={} generation={} afwMode={}",
                receipt, Handle()->Id, _native->FrameCount(), static_cast<unsigned>(result),
                ofxr_bridge::embedded_layer_active(), control.desired.motion_vectors,
                control.desired.frame_generation, ofxr_bridge::afw_snapshot().desired.mode);
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            result, 130, Handle()->Id, _native->FrameCount());
        // Missing camera/guide rejects AFW, not the already-completed native SR.
        // Finish the copyback handshake even if no identity could be stamped.
        return true;
    }
  public:
    LukeVulkanCaptureBridge(IFeature* native, NVSDK_NGX_Parameter* parameters)
        : IFeature(native->Handle()->Id, parameters), IFeature_Vk(native->Handle()->Id, parameters),
          IFeature_VkwDx12(native->Handle()->Id, parameters), _native(native) {}
    Upscaler GetUpscalerType() const override { return _native->GetUpscalerType(); }
    ~LukeVulkanCaptureBridge() override {
        DrainCapture();
        ofxr_bridge::retire_dlss_guide_stream(Handle()->Id);
    }
};


bool IFeature_Vk::Init(VkInstance InInstance, VkPhysicalDevice InPD, VkDevice InDevice, VkCommandBuffer InCmdBuffer,
                       PFN_vkGetInstanceProcAddr InGIPA, PFN_vkGetDeviceProcAddr InGDPA,
                       NVSDK_NGX_Parameter* InParameters)
{
    Instance = InInstance;
    PhysicalDevice = InPD;
    Device = InDevice;
    GIPA = InGIPA;
    GDPA = InGDPA;

    auto result = InitInternal(InCmdBuffer, InParameters);

    if (result)
    {

        OutputScaler = std::make_unique<OS_Vk>("Output Scaling", InDevice, InPD, (TargetWidth() < DisplayWidth()));
        RCAS = std::make_unique<RCAS_Vk>("RCAS", InDevice, InPD);
        Magnifier = std::make_unique<Magnifier_Vk>("Magnifier", InDevice, InPD);

        // UpscalerTime = std::make_unique<GpuTime_Vk>(InDevice);
        if ((GetUpscalerType() == Upscaler::DLSS || GetUpscalerType() == Upscaler::DLSSD) &&
            !IsWithDx12() && LukeRossDlssIngress::Detected()) {
            if (!vulkan_capture::enabled(InDevice)) {
                LOG_ERROR("V30065 Luke Vulkan capture requires enabled shaderStorageImageExtendedFormats; native SR unchanged");
                return result;
            }
            _lukeCapture = std::make_unique<LukeVulkanCaptureBridge>(this, InParameters);
            if (!_lukeCapture->Init(InInstance, InPD, InDevice, InCmdBuffer, InGIPA, InGDPA, InParameters)) {
                LOG_ERROR("V30065 Luke Vulkan interop initialization failed; native SR remains Vulkan");
                _lukeCapture.reset();
            }
        }
    }

    return result;
}

bool IFeature_Vk::Evaluate(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
{
    if (!IsInited())
    {
        LOG_ERROR("Not inited!");
        return false;
    }

    if (Config::Instance()->OverrideSharpness.value_or_default())
        _sharpness = Config::Instance()->Sharpness.value_or_default();
    else
        _sharpness = GetSharpness(InParameters);

    if (_sharpness > 1.0f)
        _sharpness = 1.0f;

    auto upscaler = GetUpscalerType();
    bool useRcas = upscaler == Upscaler::XeSS ||
                   (upscaler == Upscaler::DLSS && Version() >= feature_version(2, 5, 1)) || upscaler == Upscaler::DLSSD;

    if (!useRcas)
        useRcas = Config::Instance()->RcasEnabled.value_or_default();

    if (_sharpness == 0.0f)
        useRcas = false;

    if (!useRcas && (Config::Instance()->MotionSharpnessEnabled.value_or_default() &&
                     Config::Instance()->MotionSharpness.value_or_default() > 0.0f))
    {
        useRcas = true;
    }

    if (!RCAS->IsInit())
        useRcas = false;

    bool useOutputScaling =
        Config::Instance()->OutputScalingEnabled.value_or_default() && (LowResMV() || RenderWidth() == DisplayWidth());

    if (!OutputScaler->IsInit())
        useOutputScaling = false;

    NVSDK_NGX_Resource_VK* paramOutput = nullptr;
    NVSDK_NGX_Resource_VK* paramMotion = nullptr;
    NVSDK_NGX_Resource_VK* paramDepth = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &paramOutput);
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &paramMotion);
    InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &paramDepth);

    // Save the original output so we can restore it later
    VkImageInfo originalOutput {};
    if (paramOutput)
    {
        originalOutput.Image = paramOutput->Resource.ImageViewInfo.Image;
        originalOutput.ImageView = paramOutput->Resource.ImageViewInfo.ImageView;
        originalOutput.SubresourceRange = paramOutput->Resource.ImageViewInfo.SubresourceRange;
        originalOutput.Format = paramOutput->Resource.ImageViewInfo.Format;
        originalOutput.Width = paramOutput->Resource.ImageViewInfo.Width;
        originalOutput.Height = paramOutput->Resource.ImageViewInfo.Height;
    }

    const VkImageUsageFlags intermediateUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Order is important as that's the order of shader dispatch
    std::vector<ShaderPass> pipeline;

    if (useOutputScaling)
    {
        pipeline.push_back(
            { // Setup
              [&](const VkImageInfo& nextOutput) -> VkImageInfo
              {
                  if (OutputScaler->CreateImageResource(Device, PhysicalDevice, TargetWidth(), TargetHeight(),
                                                        nextOutput.Format, intermediateUsage))
                  {
                      OutputScaler->SetImageLayout(InCmdBuffer, OutputScaler->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                                   VK_IMAGE_LAYOUT_GENERAL, nextOutput.SubresourceRange);

                      // TODO: improve query of the output info
                      VkImageInfo info = nextOutput;
                      info.Image = OutputScaler->GetImage();
                      info.ImageView = OutputScaler->GetImageView();
                      info.Width = TargetWidth();
                      info.Height = TargetHeight();
                      return info;
                  }
                  return VkImageInfo {}; // Returns null handle
              },

              // Dispatch
              [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
              {
                  LOG_DEBUG("Scaling output...");
                  OutputScaler->SetImageLayout(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_GENERAL,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, input.SubresourceRange);

                  if (!OutputScaler->Dispatch(InCmdBuffer, input, output))
                  {
                      Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
                      State::Instance().changeBackend[Handle()->Id] = true;
                      return false;
                  }
                  return true;
              } });
    }

    _actualSharpness = _sharpness;
    if (useRcas)
    {
        pipeline.push_back(
            { // Setup
              [&](const VkImageInfo& nextOutput) -> VkImageInfo
              {
                  InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                  _sharpness = 0.0f;

                  if (RCAS->CreateImageResource(Device, PhysicalDevice, nextOutput.Width, nextOutput.Height,
                                                nextOutput.Format, intermediateUsage))
                  {
                      RCAS->SetImageLayout(InCmdBuffer, RCAS->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_GENERAL, nextOutput.SubresourceRange);

                      VkImageInfo info = nextOutput;
                      info.Image = RCAS->GetImage();
                      info.ImageView = RCAS->GetImageView();
                      return info;
                  }
                  return VkImageInfo {};
              },

              // Dispatch
              [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
              {
                  if (!RCAS->CanRender() || !paramMotion || !paramOutput)
                      return true;

                  RCAS->SetImageLayout(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, input.SubresourceRange);

                  RcasConstants rcasConstants {};
                  rcasConstants.Sharpness = _actualSharpness.value_or(_sharpness);
                  rcasConstants.DepthIsLinear = DepthLinear();
                  rcasConstants.DepthIsReversed = DepthInverted();
                  rcasConstants.IsHdr = IsHdr();

                  _sharpness = _actualSharpness.value_or(_sharpness);
                  _actualSharpness.reset();

                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);

                  float nearPlane = 0.0f;
                  float farPlane = 0.0f;

                  if (InParameters->Get("DLSSG.CameraNear", &nearPlane) == NVSDK_NGX_Result_Success &&
                      InParameters->Get("DLSSG.CameraFar", &farPlane) == NVSDK_NGX_Result_Success)
                  {
                      rcasConstants.CameraNear = nearPlane;
                      rcasConstants.CameraFar = farPlane;
                  }
                  else
                  {
                      rcasConstants.CameraNear = Config::Instance()->FsrCameraNear.value_or_default();
                      rcasConstants.CameraFar = Config::Instance()->FsrCameraFar.value_or_default();
                  }

                  // In Vulkan we pass the Info structs instead of raw resources
                  VkImageInfo mvInfo = *(VkImageInfo*) &paramMotion->Resource.ImageViewInfo;
                  VkImageInfo depthInfo = *(VkImageInfo*) &paramDepth->Resource.ImageViewInfo;

                  if (!RCAS->Dispatch(Device, InCmdBuffer, rcasConstants, input, mvInfo, output, &depthInfo))
                  {
                      Config::Instance()->RcasEnabled.set_volatile_value(false);
                      return false;
                  }
                  return true;
              } });
    }

    if (Magnifier->ShouldRun())
    {
        pipeline.push_back(
            { // Setup
              [&](const VkImageInfo& nextOutput) -> VkImageInfo
              {
                  if (Magnifier->CreateImageResource(Device, PhysicalDevice, nextOutput.Width, nextOutput.Height,
                                                     nextOutput.Format, intermediateUsage))
                  {
                      Magnifier->SetImageLayout(InCmdBuffer, Magnifier->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                                VK_IMAGE_LAYOUT_GENERAL, nextOutput.SubresourceRange);

                      VkImageInfo info = nextOutput;
                      info.Image = Magnifier->GetImage();
                      info.ImageView = Magnifier->GetImageView();
                      return info;
                  }
                  return VkImageInfo {};
              },

              // Dispatch
              [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
              {
                  if (!Magnifier->CanRender())
                      return true;

                  Magnifier->SetImageLayout(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_GENERAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, input.SubresourceRange);

                  // In Vulkan we pass the Info structs instead of raw resources
                  VkImageInfo mvInfo = *(VkImageInfo*) &paramMotion->Resource.ImageViewInfo;
                  VkImageInfo depthInfo = *(VkImageInfo*) &paramDepth->Resource.ImageViewInfo;

                  return Magnifier->Dispatch(InCmdBuffer, input, output);
              } });
    }

    // Iterate BACKWARDS to establish where each shader needs to pull its input from
    VkImageInfo currentTarget = originalOutput;
    for (auto it = pipeline.rbegin(); it != pipeline.rend(); ++it)
    {
        VkImageInfo requiredInput = it->Setup(currentTarget);
        if (requiredInput.Image != VK_NULL_HANDLE)
        {
            it->outputBuffer = currentTarget;
            it->inputBuffer = requiredInput;
            currentTarget = requiredInput; // Shift the target back for the next previous stage
        }
    }

    // Write target back into the params
    // In DX11/DX12 we set ngx param but in Vulkan we can set just the resource info
    if (paramOutput)
    {
        paramOutput->Resource.ImageViewInfo.Image = currentTarget.Image;
        paramOutput->Resource.ImageViewInfo.ImageView = currentTarget.ImageView;
        paramOutput->Resource.ImageViewInfo.SubresourceRange = currentTarget.SubresourceRange;
        paramOutput->Resource.ImageViewInfo.Format = currentTarget.Format;
        paramOutput->Resource.ImageViewInfo.Width = currentTarget.Width;
        paramOutput->Resource.ImageViewInfo.Height = currentTarget.Height;
    }

    // UpscalerTime->Start(InCmdBuffer);

    auto evalResult = EvaluateInternal(InCmdBuffer, InParameters);

    // UpscalerTime->End(InCmdBuffer);

    if (!evalResult)
        return false;

    // Iterate FORWARDS to execute the shaders in the defined order
    for (auto& pass : pipeline)
    {
        if (pass.inputBuffer.Image != VK_NULL_HANDLE && pass.outputBuffer.Image != VK_NULL_HANDLE)
        {
            if (!pass.Dispatch(pass.inputBuffer, pass.outputBuffer))
            {
                return false;
            }
        }
    }

    // Restore original output pointer
    if (paramOutput)
    {
        paramOutput->Resource.ImageViewInfo.Image = originalOutput.Image;
        paramOutput->Resource.ImageViewInfo.ImageView = originalOutput.ImageView;
        paramOutput->Resource.ImageViewInfo.SubresourceRange = originalOutput.SubresourceRange;
        paramOutput->Resource.ImageViewInfo.Format = originalOutput.Format;
        paramOutput->Resource.ImageViewInfo.Width = originalOutput.Width;
        paramOutput->Resource.ImageViewInfo.Height = originalOutput.Height;
    }

    _frameCount++;

    if(_lukeCapture) luke_source::native(*InParameters,2,Handle()->Id,reinterpret_cast<std::uintptr_t>(InCmdBuffer));
    // ONE completion for native Vulkan SR and RR, after every native postpass.
    if (_lukeCapture && ofxr_bridge::afw_snapshot().desired.mode != 0 &&
        !_lukeCapture->Evaluate(InCmdBuffer, InParameters)) {
        static std::atomic_uint64_t failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 8)
            LOG_ERROR("V30065 Luke Vulkan completed-color capture failed");
    }

    return evalResult;
}
