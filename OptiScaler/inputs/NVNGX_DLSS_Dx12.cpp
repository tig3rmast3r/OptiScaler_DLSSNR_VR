#include "pch.h"
#include "Util.h"
#include "Config.h"

#include "NVNGX_DLSS.h"
#include "NVNGX_Parameter.h"
#include "proxies/NVNGX_Proxy.h"
#include "dlssnr/DlssNr.h"
#include <upscalers/dlss/DLSSFeature_Dx12.h>
#include <shaders/output_scaling/OS_Dx12.h>

#include <upscalers/FeatureProvider_Dx12.h>
#include "upscalers/dlss/DLSSFeature_Dx12.h"

#include <framegen/nvngx/Nvngx_FG.h>
#include "FG/FSR3_Dx12_FG.h"
#include "FG/Upscaler_Inputs_Dx12.h"

#include <imgui/ImGuiNotify.hpp>

#include <hooks/D3D12_Hooks.h>
#include <hooks/Streamline_Hooks.h>
#include <hooks/D3D_NativeInterface.h>
#include <hooks/D3D_DeviceOwner.h>
#include <hooks/LukeNativeSceneCamera.h>
#include <diagnostics/DredMarkers.h>

#include <dxgi1_4.h>
#include <array>
#include <atomic>
#include <cmath>
#include <ankerl/unordered_dense.h>
#include <misc/IdentifyGpu.h>
#include "../ofxr/ofxr_bridge_client.hpp"
#include "../../third_party/ofxr/include/xrfg/dlss_motion_vectors.hpp"
#include "../../third_party/ofxr/include/xrfg/luke_color_origin.hpp"
#include "../../third_party/ofxr/include/xrfg/luke_ingress_devices.hpp"
#include "../../third_party/ofxr/include/xrfg/bridge_flight_logger.hpp"

static ankerl::unordered_dense::map<unsigned int, ContextData<IFeature_Dx12>> Dx12Contexts;
static std::unordered_map<unsigned int, NVSDK_NGX_Feature> HandleToFeature;

static ID3D12Device* D3D12Device = nullptr;
static int evalCounter = 0;
static bool shutdown = false;
static bool _skipInit = false;
static wchar_t const** paths;

namespace
{
HMODULE FindRealVrModule()
{
    const auto hasMarker = [](HMODULE module) {
        return module != nullptr && GetProcAddress(module, "RealVRVersion") != nullptr;
    };

    // Prefer the ordinary basename lookup, but do not rely on it: Luke Ross'
    // proxy and the system DXGI can both be present in this process.
    HMODULE candidate = GetModuleHandleW(L"dxgi.dll");
    if (hasMarker(candidate))
        return candidate;
    candidate = GetModuleHandleW(L"RealVR64.dll");
    if (hasMarker(candidate))
        return candidate;

    const auto& optiPath = Config::Instance()->MainDllPath.value();
    const std::filesystem::path localDxgi =
        std::filesystem::path(optiPath).parent_path() / L"dxgi.dll";
    candidate = GetModuleHandleW(localDxgi.c_str());
    return hasMarker(candidate) ? candidate : nullptr;
}
}

template <typename Interface>
static Interface* ResolveD3DNativeInterface(Interface* object, HRESULT* receipt = nullptr)
{
    IUnknown* native = nullptr;
    if (receipt) *receipt = E_POINTER;
    if (d3d_native::get_borrowed(object, &native, receipt))
        return static_cast<Interface*>(native);
    return nullptr;
}

static Microsoft::WRL::ComPtr<IUnknown> CanonicalIdentity(IUnknown* object)
{
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (object != nullptr)
        object->QueryInterface(IID_PPV_ARGS(identity.GetAddressOf()));
    return identity;
}

struct DlssNativeDeviceReceipt
{
    HRESULT get_device = E_POINTER;
    HRESULT native_query = E_POINTER;
    HRESULT retain = E_POINTER;
    std::uintptr_t reported = 0;
};

template <typename DeviceChild>
static Microsoft::WRL::ComPtr<ID3D12Device> ResolveD3DNativeDevice(
    DeviceChild* object, DlssNativeDeviceReceipt* receipt = nullptr)
{
    Microsoft::WRL::ComPtr<ID3D12Device> reportedDevice;
    if (receipt) *receipt = {};
    if (object == nullptr)
        return {};
    const HRESULT getResult = object->GetDevice(IID_PPV_ARGS(reportedDevice.GetAddressOf()));
    if (receipt) {
        receipt->get_device = getResult;
        receipt->reported = reinterpret_cast<std::uintptr_t>(reportedDevice.Get());
    }
    if (FAILED(getResult)) return {};

    HRESULT nativeResult = E_POINTER;
    ID3D12Device* nativeDevice = ResolveD3DNativeInterface(
        reportedDevice.Get(), &nativeResult);
    if (receipt) receipt->native_query = nativeResult;
    Microsoft::WRL::ComPtr<ID3D12Device> retainedDevice;
    if (nativeDevice == nullptr)
        return {};
    const HRESULT retainResult = nativeDevice->QueryInterface(IID_PPV_ARGS(retainedDevice.GetAddressOf()));
    if (receipt) receipt->retain = retainResult;
    if (FAILED(retainResult)) return {};
    return retainedDevice;
}

static bool SameCanonicalIdentity(IUnknown* left, IUnknown* right)
{
    const auto leftIdentity = CanonicalIdentity(left);
    const auto rightIdentity = CanonicalIdentity(right);
    return leftIdentity && rightIdentity &&
        leftIdentity.Get() == rightIdentity.Get();
}

static bool SameAdapter(ID3D12Device* left, ID3D12Device* right)
{
    if (left == nullptr || right == nullptr)
        return false;
    const LUID leftLuid = left->GetAdapterLuid();
    const LUID rightLuid = right->GetAdapterLuid();
    return leftLuid.HighPart == rightLuid.HighPart &&
        leftLuid.LowPart == rightLuid.LowPart;
}

static void MaybeLogOfxrGuideStatistics();

static const char* OfxrGuideStatusName(std::uint32_t status)
{
    switch (status)
    {
    case 0: return "disabled";
    case 1: return "waiting_for_dlss";
    case 2: return "output_not_direct";
    case 3: return "queue_mismatch";
    case 4: return "invalid_input";
    case 5: return "temporal_mismatch";
    case 6: return "used";
    default: return "unknown";
    }
}

static void MaybeLogOfxrGuideStatistics()
{
    const auto s = ofxr_bridge::dlss_guide_statistics();
    if (s.published == 0) return;
    static std::atomic_uint64_t lastLoggedPublication { 0 };
    std::uint64_t previous = lastLoggedPublication.load(std::memory_order_relaxed);
    if ((previous != 0 && s.published < previous + 240) ||
        !lastLoggedPublication.compare_exchange_strong(
            previous, s.published, std::memory_order_relaxed))
        return;
    LOG_INFO(
        "OFXR DLSS guides: status={} published={} depth={} snapshots={} failures={} "
        "resolve={} missing={} stale={} queue_mismatch={} invalid={} matched={} used={} "
        "fsr_attempts={} fsr_used={} fsr_failures={} fsr_status={}",
        OfxrGuideStatusName(s.status), s.published, s.depth_publications,
        s.snapshot_copies, s.snapshot_failures, s.resolve_calls,
        s.resolve_missing_streams, s.resolve_stale_pairs,
        s.resolve_queue_mismatches, s.invalid_rejections, s.matched, s.used,
        s.fsr_attempts, s.fsr_used, s.fsr_failures, s.fsr_status);
}

static NVSDK_NGX_Feature ResolveEvaluatedFeature(
    unsigned int handleId, NVSDK_NGX_Parameter* parameters)
{
    if (const auto known = HandleToFeature.find(handleId);
        known != HandleToFeature.end())
        return known->second;

    int interpolationCount = 0;
    if (parameters->Get("DLSSG.MultiFrameCount", &interpolationCount) ==
        NVSDK_NGX_Result_Success)
    {
        HandleToFeature.emplace(handleId, NVSDK_NGX_Feature_FrameGeneration);
        LOG_INFO("Late-attached native NGX handle {} classified as FrameGeneration", handleId);
        return NVSDK_NGX_Feature_FrameGeneration;
    }

    ID3D12Resource* rayReconstructionGuide = nullptr;
    if ((parameters->Get(NVSDK_NGX_Parameter_GBuffer_DiffuseAlbedo,
                         &rayReconstructionGuide) == NVSDK_NGX_Result_Success ||
         parameters->Get(NVSDK_NGX_Parameter_GBuffer_Albedo,
                         &rayReconstructionGuide) == NVSDK_NGX_Result_Success) &&
        rayReconstructionGuide != nullptr)
    {
        HandleToFeature.emplace(handleId, NVSDK_NGX_Feature_RayReconstruction);
        LOG_INFO("Late-attached native NGX handle {} classified as RayReconstruction", handleId);
        return NVSDK_NGX_Feature_RayReconstruction;
    }

    ID3D12Resource* output = nullptr;
    ID3D12Resource* motion = nullptr;
    if (parameters->Get(NVSDK_NGX_Parameter_Output, &output) ==
            NVSDK_NGX_Result_Success &&
        parameters->Get(NVSDK_NGX_Parameter_MotionVectors, &motion) ==
            NVSDK_NGX_Result_Success &&
        output != nullptr && motion != nullptr)
    {
        HandleToFeature.emplace(handleId, NVSDK_NGX_Feature_SuperSampling);
        LOG_INFO("Late-attached native NGX handle {} classified as SuperSampling", handleId);
        return NVSDK_NGX_Feature_SuperSampling;
    }
    return NVSDK_NGX_Feature_Reserved0;
}

static void PublishOfxrDlssGuides(
    unsigned int handleId,
    ID3D12GraphicsCommandList* commandList,
    NVSDK_NGX_Parameter* parameters,
    IFeature* optiFeature,
    ID3D12CommandQueue* producerQueue = nullptr)
{
    // This is called at the actual native NGX EvaluateFeature ingress, after
    // OptiScaler's DLSS parameter processing and before the DLSS DLL runs.
    // Motion/depth are NGX inputs, never post-DLSS corrected outputs.
    const auto control = ofxr_bridge::snapshot();
    const bool enabled = control.desired.motion_vectors == 1 ||
        control.desired.frame_generation == 1;
    ofxr_bridge::configure_dlss_guides(enabled);
    if (!enabled) {
        if (producerQueue) {
            static std::atomic_uint64_t rejects{0};
            const auto count = rejects.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 2 || count % 240 == 0)
                LOG_WARN("V30067 Vulkan guide tracking disabled embedded={} motion={} generation={}",
                    ofxr_bridge::embedded_layer_active(), control.desired.motion_vectors,
                    control.desired.frame_generation);
        }
        return;
    }

    ID3D12Resource* output = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* inputColor = nullptr;
    auto* queue = producerQueue ? producerQueue : State::Instance().currentCommandQueue;
    const auto outputGet = parameters->Get(NVSDK_NGX_Parameter_Output, &output);
    const auto motionGet = parameters->Get(NVSDK_NGX_Parameter_MotionVectors, &motion);
    if (outputGet != NVSDK_NGX_Result_Success || motionGet != NVSDK_NGX_Result_Success ||
        output == nullptr || motion == nullptr || queue == nullptr ||
        commandList == nullptr) {
        if (producerQueue) {
            static std::atomic_uint64_t rejects{0};
            const auto count = rejects.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 2 || count % 240 == 0)
                LOG_WARN("V30067 Vulkan guide missing resources outputGet=0x{:X} motionGet=0x{:X} output={:p} motion={:p} queue={:p} list={:p}",
                    static_cast<unsigned>(outputGet), static_cast<unsigned>(motionGet),
                    static_cast<void*>(output), static_cast<void*>(motion), static_cast<void*>(queue), static_cast<void*>(commandList));
        }
        return;
    }

    parameters->Get(NVSDK_NGX_Parameter_Depth, &depth);
    parameters->Get(NVSDK_NGX_Parameter_Color, &inputColor);

    // Normalize interfaces, then prove one authoritative queue-device owner.
    const std::array<IUnknown*, 6> originalObjects{output, motion, depth, inputColor, commandList, queue};
    std::array<HRESULT, 6> objectQueries{};
    output = ResolveD3DNativeInterface(output, &objectQueries[0]);
    motion = ResolveD3DNativeInterface(motion, &objectQueries[1]);
    depth = ResolveD3DNativeInterface(depth, &objectQueries[2]);
    inputColor = ResolveD3DNativeInterface(inputColor, &objectQueries[3]);
    commandList = ResolveD3DNativeInterface(commandList, &objectQueries[4]);
    queue = ResolveD3DNativeInterface(queue, &objectQueries[5]);
    const std::array<IUnknown*, 6> nativeObjects{output, motion, depth, inputColor, commandList, queue};
    std::array<DlssNativeDeviceReceipt, 6> receipts{};
    const auto outputDevice = ResolveD3DNativeDevice(output, &receipts[0]);
    const auto motionDevice = ResolveD3DNativeDevice(motion, &receipts[1]);
    const auto depthDevice = ResolveD3DNativeDevice(depth, &receipts[2]);
    const auto inputColorDevice = ResolveD3DNativeDevice(inputColor, &receipts[3]);
    const auto commandDevice = ResolveD3DNativeDevice(commandList, &receipts[4]);
    const auto queueDevice = ResolveD3DNativeDevice(queue, &receipts[5]);
    d3d_owner::Receipt ownerReceipt;
    const bool queueListOwner = d3d_owner::same(queueDevice.Get(), commandDevice.Get(), &ownerReceipt);
    if (ownerReceipt.fresh) {
        auto& logger = xrfg::bridge_flight_logger();
        logger.event(xrfg::BridgeFlightOperation::luke_afw_stage,
            ownerReceipt.verified ? S_OK : E_INVALIDARG, 128,
            reinterpret_cast<std::uintptr_t>(queueDevice.Get()),
            reinterpret_cast<std::uintptr_t>(commandDevice.Get()));
        const auto pack = [](HRESULT lo, HRESULT hi) {
            return std::uint64_t(static_cast<std::uint32_t>(lo)) |
                (std::uint64_t(static_cast<std::uint32_t>(hi)) << 32);
        };
        for (unsigned i = 0; i < ownerReceipt.steps.size(); i += 2)
            logger.event(xrfg::BridgeFlightOperation::luke_afw_stage, i / 2, 129,
                ownerReceipt.mask, pack(ownerReceipt.steps[i], ownerReceipt.steps[i + 1]));
        if (ofxr_bridge::logging_setting())
            LOG_WARN("V30063 device backing proof verified={} mask=0x{:X} queue={:p} list={:p} pre=0x{:X}/0x{:X} forward=0x{:X}/0x{:X} reverse=0x{:X}/0x{:X} clear=0x{:X}/0x{:X} empty=0x{:X}/0x{:X}",
                ownerReceipt.verified, ownerReceipt.mask, static_cast<void*>(queueDevice.Get()),
                static_cast<void*>(commandDevice.Get()), static_cast<unsigned>(ownerReceipt.steps[0]),
                static_cast<unsigned>(ownerReceipt.steps[1]), static_cast<unsigned>(ownerReceipt.steps[2]),
                static_cast<unsigned>(ownerReceipt.steps[3]), static_cast<unsigned>(ownerReceipt.steps[4]),
                static_cast<unsigned>(ownerReceipt.steps[5]), static_cast<unsigned>(ownerReceipt.steps[6]),
                static_cast<unsigned>(ownerReceipt.steps[7]), static_cast<unsigned>(ownerReceipt.steps[8]),
                static_cast<unsigned>(ownerReceipt.steps[9]));
    }
    // bits 0..4: missing output/MV/list/queue/depth device;
    // bits 5..7: output/MV/depth adapter mismatch; bit 8: queue/list identity.
    const auto deviceFailures = xrfg::luke_ingress_device_failures({
        bool(outputDevice), bool(motionDevice), bool(commandDevice), bool(queueDevice),
        depth != nullptr, bool(depthDevice),
        SameAdapter(outputDevice.Get(), commandDevice.Get()),
        SameAdapter(motionDevice.Get(), commandDevice.Get()),
        !depth || SameAdapter(depthDevice.Get(), commandDevice.Get()),
        queueListOwner});
    if (deviceFailures)
    {
        const std::array<ID3D12Device*, 6> devices{outputDevice.Get(), motionDevice.Get(),
            depthDevice.Get(), inputColorDevice.Get(), commandDevice.Get(), queueDevice.Get()};
        std::uint64_t fingerprint = deviceFailures;
        for (auto* device : devices)
            fingerprint = (fingerprint * 1099511628211ULL) ^ reinterpret_cast<std::uintptr_t>(device);
        static std::atomic<std::uint64_t> lastFingerprint{0};
        static std::atomic<unsigned> packets{0};
        // At most eight changed-signature packets per process, independent of
        // frame rate. No GPU query/readback, hook, retry or continuous file I/O.
        if (lastFingerprint.exchange(fingerprint, std::memory_order_relaxed) != fingerprint &&
            packets.fetch_add(1, std::memory_order_relaxed) < 8) {
            auto& logger = xrfg::bridge_flight_logger();
            logger.event(xrfg::BridgeFlightOperation::luke_afw_stage, deviceFailures, 123, handleId, fingerprint);
            if (ofxr_bridge::logging_setting())
                LOG_WARN("V30061 OFXR ingress device rejection mask=0x{:X} handle={} fingerprint={}",
                    deviceFailures, handleId, fingerprint);
            const auto pack = [](HRESULT lo, HRESULT hi) {
                return std::uint64_t(static_cast<std::uint32_t>(lo)) |
                    (std::uint64_t(static_cast<std::uint32_t>(hi)) << 32);
            };
            static constexpr const char* roles[]{"output", "motion", "depth", "inputColor", "list", "queue"};
            for (std::size_t i = 0; i < devices.size(); ++i) {
                const auto identity = CanonicalIdentity(devices[i]);
                const auto luid = devices[i] ? devices[i]->GetAdapterLuid() : LUID{};
                logger.event(xrfg::BridgeFlightOperation::luke_afw_stage, i, 124,
                    reinterpret_cast<std::uintptr_t>(originalObjects[i]), reinterpret_cast<std::uintptr_t>(nativeObjects[i]));
                logger.event(xrfg::BridgeFlightOperation::luke_afw_stage, i, 125,
                    receipts[i].reported, reinterpret_cast<std::uintptr_t>(devices[i]));
                logger.event(xrfg::BridgeFlightOperation::luke_afw_stage, i, 126,
                    reinterpret_cast<std::uintptr_t>(identity.Get()),
                    std::uint64_t(luid.LowPart) | (std::uint64_t(static_cast<std::uint32_t>(luid.HighPart)) << 32));
                logger.event(xrfg::BridgeFlightOperation::luke_afw_stage, i, 127,
                    pack(objectQueries[i], receipts[i].get_device), pack(receipts[i].native_query, receipts[i].retain));
                if (ofxr_bridge::logging_setting())
                    LOG_WARN("V30061 OFXR ingress {} object={:p}->{:p} objectQI=0x{:X} getDevice=0x{:X} device={:p}->{:p} deviceQI=0x{:X} retain=0x{:X} canonical={:p} luid={}:{}",
                        roles[i], static_cast<void*>(originalObjects[i]), static_cast<void*>(nativeObjects[i]),
                        static_cast<std::uint32_t>(objectQueries[i]), static_cast<std::uint32_t>(receipts[i].get_device),
                        reinterpret_cast<void*>(receipts[i].reported), static_cast<void*>(devices[i]),
                        static_cast<std::uint32_t>(receipts[i].native_query), static_cast<std::uint32_t>(receipts[i].retain),
                        static_cast<void*>(identity.Get()), luid.HighPart, luid.LowPart);
            }
        }
        return;
    }

    ofxr_bridge::ensure_d3d12_submission_hook(queueDevice.Get());

    const auto outputDesc = output->GetDesc();
    const auto motionDesc = motion->GetDesc();
    unsigned int outputX = 0;
    unsigned int outputY = 0;
    unsigned int depthX = 0;
    unsigned int depthY = 0;
    unsigned int inputColorX = 0;
    unsigned int inputColorY = 0;
    unsigned int displayWidth = optiFeature ? optiFeature->DisplayWidth() : 0;
    unsigned int displayHeight = optiFeature ? optiFeature->DisplayHeight() : 0;
    unsigned int renderWidth = optiFeature ? optiFeature->RenderWidth() : 0;
    unsigned int renderHeight = optiFeature ? optiFeature->RenderHeight() : 0;
    float motionX = 0.0f;
    float motionY = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float frameTimeDelta = 16.6667f;
    int reset = 0;
    unsigned int flags = 0;

    parameters->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, &outputX);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, &outputY);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &depthX);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &depthY);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
        &inputColorX);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
        &inputColorY);
    if (displayWidth == 0) parameters->Get(NVSDK_NGX_Parameter_OutWidth, &displayWidth);
    if (displayHeight == 0) parameters->Get(NVSDK_NGX_Parameter_OutHeight, &displayHeight);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &renderWidth);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &renderHeight);
    if (renderWidth == 0) parameters->Get(NVSDK_NGX_Parameter_Width, &renderWidth);
    if (renderHeight == 0) parameters->Get(NVSDK_NGX_Parameter_Height, &renderHeight);
    parameters->Get(NVSDK_NGX_Parameter_MV_Offset_X, &motionX);
    parameters->Get(NVSDK_NGX_Parameter_MV_Offset_Y, &motionY);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &scaleX);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &scaleY);
    parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);
    parameters->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &frameTimeDelta);
    parameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
    const unsigned int availableOutputWidth = static_cast<unsigned int>(
        outputDesc.Width - std::min<UINT64>(outputX, outputDesc.Width));
    const unsigned int availableOutputHeight = outputDesc.Height -
        std::min<unsigned int>(outputY, outputDesc.Height);
    if (renderWidth == 0) renderWidth = static_cast<unsigned int>(motionDesc.Width);
    if (renderHeight == 0) renderHeight = motionDesc.Height;
    if (displayWidth == 0) displayWidth = availableOutputWidth;
    if (displayHeight == 0) displayHeight = availableOutputHeight;

    const bool lowResolutionMotion = optiFeature
        ? optiFeature->LowResMV()
        : (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    const unsigned int motionWidth = lowResolutionMotion ? renderWidth : displayWidth;
    const unsigned int motionHeight = lowResolutionMotion ? renderHeight : displayHeight;
    const unsigned int motionBaseX =
        static_cast<unsigned int>(std::max(0.0f, motionX));
    const unsigned int motionBaseY =
        static_cast<unsigned int>(std::max(0.0f, motionY));
    const auto motionState = !producerQueue && Config::Instance()->MVResourceBarrier.has_value()
        ? static_cast<D3D12_RESOURCE_STATES>(Config::Instance()->MVResourceBarrier.value())
        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto depthState = !producerQueue && Config::Instance()->DepthResourceBarrier.has_value()
        ? static_cast<D3D12_RESOURCE_STATES>(Config::Instance()->DepthResourceBarrier.value())
        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto inputColorState = !producerQueue && Config::Instance()->ColorResourceBarrier.has_value()
        ? static_cast<D3D12_RESOURCE_STATES>(Config::Instance()->ColorResourceBarrier.value())
        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    unsigned int depthWidth = 0;
    unsigned int depthHeight = 0;
    if (depth != nullptr)
    {
        const auto depthDesc = depth->GetDesc();
        depthWidth = std::min<unsigned int>(renderWidth, static_cast<unsigned int>(
            depthDesc.Width - std::min<UINT64>(depthX, depthDesc.Width)));
        depthHeight = std::min<unsigned int>(renderHeight,
            depthDesc.Height - std::min<unsigned int>(depthY, depthDesc.Height));
    }
    if (!std::isfinite(frameTimeDelta) || frameTimeDelta <= 0.0f)
        frameTimeDelta = 16.6667f;
    const float cameraNear = Config::Instance()->FsrCameraNear.value_or_default();
    const float configuredCameraFar = Config::Instance()->FsrCameraFar.value_or_default();
    const bool depthInfinite = configuredCameraFar == 0.0f;
    const float cameraFar = depthInfinite
        ? std::max(cameraNear + 1.0f, 1.0f)
        : configuredCameraFar;
    const bool depthInverted = optiFeature
        ? optiFeature->DepthInverted()
        : (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;


    if (!enabled) return;

    OFXR_DlssGuidePublicationV2 publication{};
    publication.stream = handleId;
    publication.output = output;
    publication.motion_vectors = motion;
    publication.producer_queue = queue;
    publication.producer_command_list = commandList;
    publication.output_x = outputX;
    publication.output_y = outputY;
    publication.output_width = std::min(displayWidth, availableOutputWidth);
    publication.output_height = std::min(displayHeight, availableOutputHeight);
    publication.motion_x = motionBaseX;
    publication.motion_y = motionBaseY;
    publication.motion_width = std::min<unsigned int>(motionWidth,
        static_cast<unsigned int>(motionDesc.Width -
            std::min<UINT64>(motionBaseX, motionDesc.Width)));
    publication.motion_height = std::min<unsigned int>(motionHeight,
        motionDesc.Height - std::min<unsigned int>(motionBaseY, motionDesc.Height));
    publication.scale_x = scaleX;
    publication.scale_y = scaleY;
    publication.jitter_x = jitterX;
    publication.jitter_y = jitterY;
    publication.resource_state = static_cast<std::uint32_t>(motionState);
    publication.jittered = optiFeature
        ? (optiFeature->JitteredMV() ? 1 : 0)
        : ((flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0 ? 1 : 0);
    publication.reset = reset != 0 ? 1 : 0;
    publication.depth = depth;
    publication.depth_x = depthX;
    publication.depth_y = depthY;
    publication.depth_width = depthWidth;
    publication.depth_height = depthHeight;
    publication.depth_resource_state = static_cast<std::uint32_t>(depthState);
    publication.frame_time_delta_ms = frameTimeDelta;
    publication.camera_near = cameraNear;
    publication.camera_far = cameraFar;
    publication.depth_inverted = depthInverted ? 1 : 0;
    publication.depth_infinite = depthInfinite ? 1 : 0;
    publication.output_resource_state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON);
    publication.snapshot_output = 0;
    if (inputColor != nullptr && inputColorDevice &&
        SameAdapter(inputColorDevice.Get(), commandDevice.Get()))
    {
        const auto inputColorDesc = inputColor->GetDesc();
        if (inputColorX < inputColorDesc.Width &&
            inputColorY < inputColorDesc.Height)
        {
            publication.input_color = inputColor;
            publication.input_color_x = inputColorX;
            publication.input_color_y = inputColorY;
            publication.input_color_width = std::min<unsigned int>(renderWidth,
                static_cast<unsigned int>(inputColorDesc.Width - inputColorX));
            publication.input_color_height = std::min<unsigned int>(renderHeight,
                inputColorDesc.Height - inputColorY);
            publication.input_color_resource_state =
                static_cast<std::uint32_t>(inputColorState);
        }
    }
    publication.verified_native_device = queueDevice.Get();
    // KCD native NGX has no Streamline camera parameters. Its current scene
    // packet is consumed on this exact list immediately before this SR call.
    static const bool kcdNative = _wcsicmp(Util::ExePath().filename().c_str(), L"KingdomCome.exe") == 0;
    luke_scene_camera::Receipt scene{};
    bool exactCamera{};
    if(kcdNative && !producerQueue && LukeRossDlssIngress::Detected()) {
        scene=luke_scene_camera::take(commandList,renderWidth,renderHeight);
        if(scene.valid) {
            const auto& camera=scene.camera;
            std::copy(camera.projection.begin(),camera.projection.end(),publication.camera_view_to_clip);
            std::copy(camera.inverse_projection.begin(),camera.inverse_projection.end(),publication.camera_clip_to_view);
            std::copy(camera.position.begin(),camera.position.end(),publication.camera_position);
            std::copy(camera.right.begin(),camera.right.end(),publication.camera_right);
            std::copy(camera.up.begin(),camera.up.end(),publication.camera_up);
            std::copy(camera.forward.begin(),camera.forward.end(),publication.camera_forward);
            publication.camera_near=camera.near_plane;publication.camera_far=camera.far_plane;
            publication.camera_fov_vertical=camera.fov;publication.depth_inverted=1;
            publication.depth_infinite=camera.far_plane==0?1:0;
            publication.camera_data_valid=1;publication.camera_matrices_valid=1;exactCamera=true;
        }
        static std::atomic_uint samples{0};static std::atomic_uint64_t nextSampleMs{0};
        const auto now=GetTickCount64();auto deadline=nextSampleMs.load(std::memory_order_relaxed);
        if(samples.load(std::memory_order_relaxed)<32&&now>=deadline&&
            nextSampleMs.compare_exchange_strong(deadline,now+1000,std::memory_order_relaxed)) {
            const auto sample=samples.fetch_add(1,std::memory_order_relaxed)+1;
            auto& logger=xrfg::bridge_flight_logger();
            logger.event(xrfg::BridgeFlightOperation::luke_afw_stage,scene.valid?1:0,944,handleId,scene.token);
            logger.event(xrfg::BridgeFlightOperation::luke_afw_stage,sample,945,
                (std::uint64_t(scene.attempts)<<32)|scene.reads,(std::uint64_t(scene.candidates)<<32)|scene.failure);
            logger.event(xrfg::BridgeFlightOperation::luke_afw_stage,scene.owner_mask,946,scene.location,scene.resource);
            logger.event(xrfg::BridgeFlightOperation::luke_afw_stage,
                (scene.graphics?0x100:0)|scene.slot,947,reinterpret_cast<std::uint64_t>(commandList),scene.signature);
            LOG_WARN("V33012 current scene camera valid={} token={} attempts={} reads={} candidates={} failures=0x{:X} map=0x{:X} slot={} graphics={}",
                scene.valid,scene.token,scene.attempts,scene.reads,scene.candidates,scene.failure,
                static_cast<std::uint32_t>(scene.error),scene.slot,scene.graphics);
        }
    } else exactCamera=ofxr_bridge::apply_streamline_camera(publication);
    const bool published = ofxr_bridge::publish_dlss_guides(
        publication, queueDevice.Get(), scene.valid ? scene.token : 0);
    if (LukeRossDlssIngress::Detected()) {
        const auto afw = ofxr_bridge::afw_snapshot();
        if (afw.desired.mode != 0)
            xrfg::embedded::report_afw_ingress(published, exactCamera);
        const unsigned admission = (ofxr_bridge::embedded_layer_active() ? 1u : 0u) |
            (exactCamera ? 2u : 0u) | (published ? 4u : 0u) |
            (publication.camera_matrices_valid ? 8u : 0u) |
            (afw.desired.mode != 0 ? 16u : 0u);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            admission, 130, handleId,
            (std::uint64_t(publication.output_width) << 32) | publication.output_height);
        // Bounded changed-state evidence also works for native-NGX games
        // that never call Streamline. No camera fallback or new GPU work.
        static std::atomic_uint lastAdmission{~0u}, receiptCount{0};
        if (lastAdmission.exchange(admission, std::memory_order_relaxed) != admission &&
            receiptCount.fetch_add(1, std::memory_order_relaxed) < 8)
            LOG_WARN("V33012 Luke ingress embedded={} camera={} matrices={} published={} afw_mode={} stream={} output={}x{}",
                bool(admission & 1u), exactCamera, publication.camera_matrices_valid,
                published, afw.desired.mode, handleId,
                publication.output_width, publication.output_height);
    }
    if (producerQueue) {
        static std::atomic_uint64_t receipts{0};
        const auto count = receipts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 2 || count % 240 == 0) {
            const auto stats = ofxr_bridge::dlss_guide_statistics();
            LOG_INFO("V30067 Vulkan guide publication receipt={} accepted={} embedded={} camera={} color={}x{} motion={}x{} depth={}x{} published={} invalid={} failures={}",
                count, published, ofxr_bridge::embedded_layer_active(), exactCamera,
                publication.output_width, publication.output_height, publication.motion_width, publication.motion_height,
                publication.depth_width, publication.depth_height, stats.published, stats.invalid_publications, stats.snapshot_failures);
        }
    }
    MaybeLogOfxrGuideStatistics();
}

void LukeRossDlssIngress::PublishD3D12Guide(
    unsigned int handleId, ID3D12GraphicsCommandList* commandList,
    NVSDK_NGX_Parameter* parameters, IFeature* optiFeature,
    ID3D12CommandQueue* producerQueue)
{
    PublishOfxrDlssGuides(handleId, commandList, parameters, optiFeature, producerQueue);
}


bool LukeRossDlssIngress::Detected() noexcept
{
    return FindRealVrModule() != nullptr;
}

class ScopedInitDx12
{
  private:
    bool previousState;

  public:
    ScopedInitDx12()
    {
        previousState = _skipInit;
        _skipInit = true;
    }

    ~ScopedInitDx12() { _skipInit = previousState; }
};

static void UpdateInitPaths(NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    State::Instance().NVNGX_FeatureInfo_Paths.clear();

    if (InFeatureInfo != nullptr)
    {
        auto exePath = Util::ExePath().remove_filename();

        std::optional<std::filesystem::path> nvngxDlssPath = std::nullopt;
        std::optional<std::filesystem::path> nvngxDlssDPath = std::nullopt;
        std::optional<std::filesystem::path> nvngxDlssGPath = std::nullopt;

        // Check DLSS path
        if (State::Instance().NVNGX_DLSS_Path.has_value())
        {
            nvngxDlssPath = std::filesystem::path(State::Instance().NVNGX_DLSS_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlss.dll");

            if (path.has_value())
                nvngxDlssPath = path.value();
        }

        // Check DLSS-D path
        if (State::Instance().NVNGX_DLSSD_Path.has_value())
        {
            nvngxDlssDPath = std::filesystem::path(State::Instance().NVNGX_DLSSD_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlssd.dll");

            if (path.has_value())
                nvngxDlssDPath = path.value();
        }

        // Check DLSS-G path
        if (State::Instance().NVNGX_DLSSG_Path.has_value())
        {
            nvngxDlssGPath = std::filesystem::path(State::Instance().NVNGX_DLSSG_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlssg.dll");

            if (path.has_value())
                nvngxDlssGPath = path.value();
        }

        // Override locations
        if (Config::Instance()->DLSSFeaturePath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(Config::Instance()->DLSSFeaturePath.value());

        // If DLSS path is overriden
        if (Config::Instance()->NVNGX_DLSS_Library.has_value() && nvngxDlssPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssPath.value().parent_path().wstring());

        // OptiDll Path
        State::Instance().NVNGX_FeatureInfo_Paths.push_back(Config::Instance()->MainDllPath.value());

        // Original paths from NVNGX
        for (size_t i = 0; i < InFeatureInfo->PathListInfo.Length; i++)
        {
            const wchar_t* path = InFeatureInfo->PathListInfo.Path[i];
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(std::wstring(path));
        }

        // Exe path
        State::Instance().NVNGX_FeatureInfo_Paths.push_back(exePath.wstring());

        // If DLSS path is not overriden
        if (!Config::Instance()->NVNGX_DLSS_Library.has_value() && nvngxDlssPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssPath.value().parent_path().wstring());

        // Add found locations
        if (nvngxDlssDPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssDPath.value().parent_path().wstring());

        if (nvngxDlssGPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssGPath.value().parent_path().wstring());

        // Build pointer array
        paths = new const wchar_t*[State::Instance().NVNGX_FeatureInfo_Paths.size()];
        for (size_t i = 0; i < State::Instance().NVNGX_FeatureInfo_Paths.size(); ++i)
        {
            paths[i] = State::Instance().NVNGX_FeatureInfo_Paths[i].c_str();
            LOG_DEBUG("Feature Path [{}]: {}", i, wstring_to_string(State::Instance().NVNGX_FeatureInfo_Paths[i]));
        }

        InFeatureInfo->PathListInfo.Path = paths;
        InFeatureInfo->PathListInfo.Length = (int) State::Instance().NVNGX_FeatureInfo_Paths.size();
    }
}

#pragma region DLSS Init Calls

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init_Ext(unsigned long long InApplicationId,
                                                        const wchar_t* InApplicationDataPath, ID3D12Device* InDevice,
                                                        NVSDK_NGX_Version InSDKVersion,
                                                        const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    LOG_FUNC();

    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    State::Instance().NVNGX_ApplicationId = InApplicationId;
    State::Instance().NVNGX_ApplicationDataPath = std::wstring(InApplicationDataPath);
    State::Instance().NVNGX_Version = InSDKVersion;
    State::Instance().NVNGX_FeatureInfo = &localFeatureInfo;
    State::Instance().NVNGX_Version = InSDKVersion;

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D12_Init_Ext() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D12_Init_Ext");

            auto result = NVNGXProxy::D3D12_Init_Ext()(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion,
                                                       &localFeatureInfo);
            LOG_INFO("calling NVNGXProxy::D3D12_Init_Ext result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx12Inited(true);
        }
        else
        {
            LOG_WARN("NVNGXProxy::NVNGXModule or NVNGXProxy::D3D12_Init_Ext is nullptr!");
        }
    }

    if (InFeatureInfo != nullptr && InSDKVersion > 0x0000013)
        State::Instance().NVNGX_Logger = InFeatureInfo->LoggingInfo;

    if (State::Instance().nvngxDx12Inited && InDevice == D3D12Device)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        Nvngx_FG::D3D12_Init_Ext(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion, &localFeatureInfo);
    }

    LOG_INFO("AppId: {0}", InApplicationId);
    LOG_INFO("SDK: {0:x}", (unsigned int) InSDKVersion);
    LOG_INFO(L"InApplicationDataPath {0}", std::wstring(InApplicationDataPath));

    D3D12Device = InDevice;
    State::Instance().currentD3D12Device = InDevice;
    D3D12Hooks::HookDevice(InDevice);

    State::Instance().nvngxDx12Inited = true;

    UpscalerInputsDx12::Init(InDevice);

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init(unsigned long long InApplicationId,
                                                    const wchar_t* InApplicationDataPath, ID3D12Device* InDevice,
                                                    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
                                                    NVSDK_NGX_Version InSDKVersion)
{
    LOG_FUNC();

    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D12_Init() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D12_Init");

            auto result = NVNGXProxy::D3D12_Init()(InApplicationId, InApplicationDataPath, InDevice, &localFeatureInfo,
                                                   InSDKVersion);

            LOG_INFO("calling NVNGXProxy::D3D12_Init result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx12Inited(true);
        }
    }

    if (State::Instance().nvngxDx12Inited && InDevice == D3D12Device)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    // if (State::Instance().activeFgInput == FGInput::NvngxFG)
    //{
    //     Nvngx_FG::D3D12_Init(InApplicationId, InApplicationDataPath, InDevice, InFeatureInfo, InSDKVersion);
    // }

    ScopedInitDx12 scopedInit {};
    auto result =
        NVSDK_NGX_D3D12_Init_Ext(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion, &localFeatureInfo);

    LOG_DEBUG("was called NVSDK_NGX_D3D12_Init_Ext");
    return result;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init_ProjectID(const char* InProjectId,
                                                              NVSDK_NGX_EngineType InEngineType,
                                                              const char* InEngineVersion,
                                                              const wchar_t* InApplicationDataPath,
                                                              ID3D12Device* InDevice, NVSDK_NGX_Version InSDKVersion,
                                                              const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    LOG_FUNC();

    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InProjectId = project_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D12_Init_ProjectID() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D12_Init_ProjectID");

            auto result =
                NVNGXProxy::D3D12_Init_ProjectID()(InProjectId, InEngineType, InEngineVersion, InApplicationDataPath,
                                                   InDevice, InSDKVersion, &localFeatureInfo);

            LOG_INFO("calling NVNGXProxy::D3D12_Init_ProjectID result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx12Inited(true);
        }
    }

    LOG_INFO("InProjectId: {0}", InProjectId);
    LOG_INFO("InEngineType: {0}", (int) InEngineType);
    LOG_INFO("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(InProjectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    if (State::Instance().nvngxDx12Inited && InDevice == D3D12Device)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    ScopedInitDx12 scopedInit {};
    auto result = NVSDK_NGX_D3D12_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, &localFeatureInfo);
    return result;
}

// Not sure about this one, original nvngx does not export this method
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init_with_ProjectID(
    const char* InProjectId, NVSDK_NGX_EngineType InEngineType, const char* InEngineVersion,
    const wchar_t* InApplicationDataPath, ID3D12Device* InDevice, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion)
{
    LOG_FUNC();

    LOG_INFO("InProjectId: {0}", InProjectId);
    LOG_INFO("InEngineType: {0}", (int) InEngineType);
    LOG_INFO("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(InProjectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    if (State::Instance().nvngxDx12Inited)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    auto result = NVSDK_NGX_D3D12_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, InFeatureInfo);

    return result;
}

#pragma endregion

#pragma region DLSS Shutdown Calls

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Shutdown(void)
{
    shutdown = true;
    State::Instance().nvngxDx12Inited = false;

    D3D12Device = nullptr;

    State::Instance().currentFeature = nullptr;

    // Unhooking and cleaning stuff causing issues during shutdown.
    // Disabled for now to check if it cause any issues
    // UnhookAll();
    DLSSFeatureDx12::Shutdown(D3D12Device);

    // Added `&& !State::Instance().isShuttingDown` hack for crash on exit
    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx12Inited() &&
        NVNGXProxy::D3D12_Shutdown() != nullptr && !State::Instance().isShuttingDown)
    {
        auto result = NVNGXProxy::D3D12_Shutdown()();
        NVNGXProxy::SetDx12Inited(false);
    }

    // Unhooking and cleaning stuff causing issues during shutdown.
    // Disabled for now to check if it cause any issues
    // HooksDx::UnHook();

    // Disabled to prevent crash
    if (State::Instance().currentFG != nullptr && State::Instance().activeFgInput == FGInput::Upscaler)
    {
        if (State::Instance().isShuttingDown)
            State::Instance().currentFG->Shutdown();
        else
            State::Instance().currentFG->DestroyFGContext();

        State::Instance().clearCapturedHudlesses = true;
    }

    shutdown = false;

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        Nvngx_FG::D3D12_Shutdown();
    }

    State::Instance().nvngxDx12Inited = false;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Shutdown1(ID3D12Device* InDevice)
{
    shutdown = true;
    State::Instance().nvngxDx12Inited = false;

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        Nvngx_FG::D3D12_Shutdown1(InDevice);
    }

    // Added `&& !State::Instance().isShuttingDown` hack for crash on exit
    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx12Inited() &&
        NVNGXProxy::D3D12_Shutdown1() != nullptr && !State::Instance().isShuttingDown)
    {
        auto result = NVNGXProxy::D3D12_Shutdown1()(InDevice);
        NVNGXProxy::SetDx12Inited(false);
    }

    return NVSDK_NGX_D3D12_Shutdown();
}

#pragma endregion

#pragma region DLSS Parameter Calls

/**
 * @brief [Deprecated NGX API] Superceeded by NVSDK_NGX_AllocateParameters and NVSDK_NGX_GetCapabilityParameters.
 *
 * Retrieves a common NVSDK parameter map for providing params to the SDK. The lifetime of this
 * map is NOT managed by the application. It is expected to be managed internally by the SDK.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    // If DLSS is enabled and the real DLSS module is loaded, get native NGX table
    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::D3D12_GetParameters() != nullptr)
    {
        LOG_INFO("Calling NVNGXProxy::D3D12_GetParameters");
        auto result = NVNGXProxy::D3D12_GetParameters()(OutParameters);
        LOG_INFO("Calling NVNGXProxy::D3D12_GetParameters result: {0:X}, ptr: {1:X}", (UINT) result,
                 (UINT64) *OutParameters);

        // Copy OptiScaler config to real NGX param table
        if (result == NVSDK_NGX_Result_Success)
        {
            InitNGXParameters(*OutParameters, API::DX12);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVPersistent);
            return NVSDK_NGX_Result_Success;
        }
    }

    // Get custom parameters if using custom backend
    static NVNGX_Parameters oldParams = NVNGX_Parameters(API::DX12, true);
    *OutParameters = &oldParams;
    InitNGXParameters(*OutParameters, API::DX12);

    LOG_DEBUG("Returning custom Opti parameters");

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new NVSDK parameter map pre-populated with NGX capabilities and information about available
 * features. The output parameter map may also be used in the same ways as a parameter map allocated with
 * AllocateParameters(). The lifetime of this map is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetCapabilityParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    // Get native DLSS params if DLSS is enabled and the module is loaded
    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::IsDx12Inited() && NVNGXProxy::D3D12_GetCapabilityParameters() != nullptr)
    {
        LOG_INFO("Calling NVNGXProxy::D3D12_GetCapabilityParameters");
        auto result = NVNGXProxy::D3D12_GetCapabilityParameters()(OutParameters);
        LOG_INFO("Calling NVNGXProxy::D3D12_GetCapabilityParameters result: {0:X}, ptr: {1:X}", (UINT) result,
                 (UINT64) *OutParameters);

        if (result == NVSDK_NGX_Result_Success)
        {
            // Init external NGX table with current configuration and mark as dynamic+external
            InitNGXParameters(*OutParameters, API::DX12);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return NVSDK_NGX_Result_Success;
        }
    }

    // Get custom parameters if using custom backend
    auto& params = *(new NVNGX_Parameters(API::DX12, false));
    InitNGXParameters(&params, API::DX12);
    *OutParameters = &params;

    LOG_DEBUG("Returning custom Opti parameters");

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new parameter map used to provide parameters needed by the DLSS API. The lifetime of this map
 * is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_AllocateParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::D3D12_AllocateParameters() != nullptr)
    {
        LOG_INFO("Calling NVNGXProxy::D3D12_AllocateParameters");
        auto result = NVNGXProxy::D3D12_AllocateParameters()(OutParameters);
        LOG_INFO("Calling NVNGXProxy::D3D12_AllocateParameters result: {0:X}, ptr: {1:X}", (UINT) result,
                 (UINT64) *OutParameters);

        if (result == NVSDK_NGX_Result_Success)
        {
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return result;
        }
    }

    auto* params = new NVNGX_Parameters(API::DX12, false);
    *OutParameters = params;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_PopulateParameters_Impl(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;

    InitNGXParameters(InParameters, API::DX12);

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        Nvngx_FG::D3D12_PopulateParameters_Impl(InParameters);
    }

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Destroys a given input parameter map created with AllocateParameters or GetCapabilityParameters.
 Must not be called on maps returned by GetParameters(). Unsupported tables will not be freed.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;

    const bool isUsingDlss = Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule();
    const bool success = TryDestroyNGXParameters(InParameters, NVNGXProxy::D3D12_DestroyParameters());

    if (isUsingDlss)
        UpscalerInputsDx12::Reset();

    return success ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

#pragma endregion

#pragma region DLSS Feature Calls

static Upscaler GetUpscalerBackend()
{
    Upscaler upscaler = Upscaler::XeSS; // Default

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    if (NVNGXProxy::IsDx12Inited() && primaryGpu.dlssCapable)
        upscaler = Upscaler::DLSS;

    if (primaryGpu.fsr4Support != FSR4Support::None)
        upscaler = Upscaler::FFX;

    if (Config::Instance()->Dx12Upscaler.has_value())
        upscaler = Config::Instance()->Dx12Upscaler.value();

    return upscaler;
}

static bool EnsureD3D12Device(ID3D12GraphicsCommandList* cmdList)
{
    if (D3D12Device)
        return true;

    LOG_DEBUG("Get D3D12 device from InCmdList!");

    if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&D3D12Device))) || !D3D12Device)
    {
        LOG_ERROR("Can't get Dx12Device from InCmdList!");
        return false;
    }

    return true;
}

static NVSDK_NGX_Result TryEvaluateOptiFeature(ID3D12GraphicsCommandList* InCmdList,
                                               const NVSDK_NGX_Handle* InFeatureHandle,
                                               NVSDK_NGX_Parameter* InParameters,
                                               PFN_NVSDK_NGX_ProgressCallback InCallback);

static NVSDK_NGX_Result TryCreateOptiFeature(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID,
                                             NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle)
{
    State& state = State::Instance();
    const Config& cfg = *Config::Instance();

    state.api = DX12;

    const uint32_t handleId = IFeature::GetNextHandleId();
    LOG_INFO("Creating OptiScaler feature, HandleId: {}", handleId);

    // Determine backend name
    Upscaler upscalerBackend;
    if (InFeatureID == NVSDK_NGX_Feature_SuperSampling)
    {
        upscalerBackend = GetUpscalerBackend();
        LOG_INFO("Creating {} upscaler feature", UpscalerDisplayName(upscalerBackend));
    }
    else
    {
        upscalerBackend = Upscaler::DLSSD;
        LOG_INFO("Creating DLSSD (Ray Reconstruction) feature");
    }

    // Root signature restoration setup
    const bool restoreCompute = cfg.RestoreComputeSignature.value_or_default();
    const bool restoreGraphics = cfg.RestoreGraphicSignature.value_or_default();
    const bool shouldRestoreSigs = restoreCompute || restoreGraphics;

    // To avoid capturing the upscaler creation
    D3D12Hooks::SetRootSignatureTracking(false);

    if (shouldRestoreSigs)
        D3D12Hooks::HookToCommandListLate(InCmdList);

    // Create context entry
    Dx12Contexts[handleId] = {};

    // Retrieve feature implementation
    if (!FeatureProvider_Dx12::GetFeature(upscalerBackend, handleId, InParameters, &Dx12Contexts[handleId].feature))
    {
        LOG_ERROR("Failed to retrieve feature implementation for '{}'", UpscalerDisplayName(upscalerBackend));

        D3D12Hooks::SetRootSignatureTracking(true);

        Dx12Contexts.erase(handleId);
        return NVSDK_NGX_Result_Fail;
    }

    // Ensure D3D12 device
    if (!EnsureD3D12Device(InCmdList))
    {
        LOG_ERROR("Failed to acquire D3D12 device");

        D3D12Hooks::SetRootSignatureTracking(true);

        // Partial cleanup � handle is allocated but context is incomplete
        Dx12Contexts.erase(handleId);
        return NVSDK_NGX_Result_Fail;
    }

    // Assign handle
    if (*OutHandle == nullptr)
        *OutHandle = new NVSDK_NGX_Handle { handleId };
    else
        (*OutHandle)->Id = handleId;

    state.autoExposure.reset();

    IFeature_Dx12* feature = Dx12Contexts[handleId].feature.get();

    // Initialize feature
    if (feature->Init(D3D12Device, InCmdList, InParameters))
    {
        state.currentFeature = feature;
        evalCounter = 0;
        UpscalerInputsDx12::Reset();
    }
    else
    {
        LOG_ERROR("Feature '{}' initialization failed falling back to FSR 2.1.2", UpscalerDisplayName(upscalerBackend));
        state.newBackend = Upscaler::FSR21;
        state.changeBackend[handleId] = true;
    }

    // Restore root signatures
    if (shouldRestoreSigs)
        D3D12Hooks::RestoreRoot(InCmdList);

    D3D12Hooks::SetRootSignatureTracking(true);

    if (state.activeFgInput == FGInput::Upscaler)
        state.fgChanged = true;

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Instantiates a new feature based on the given unique feature ID and param table and
 * provides a handle used to reference the feature elsewhere in the API. Currently supports
 * various TSR and Frame Generation algorithms, including a special case for DLSS-RR passthrough.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* InCmdList,
                                                             NVSDK_NGX_Feature InFeatureID,
                                                             NVSDK_NGX_Parameter* InParameters,
                                                             NVSDK_NGX_Handle** OutHandle)
{
    LOG_FUNC();

    if (!InCmdList)
    {
        LOG_ERROR("InCmdList is null");
        return NVSDK_NGX_Result_Fail;
    }

    if (!OutHandle)
    {
        LOG_ERROR("OutHandle is null");
        return NVSDK_NGX_Result_Fail;
    }

    const State& state = State::Instance();
    const Config& cfg = *Config::Instance();

    // DLSSG replacements passthrough
    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None && Nvngx_FG::isDx12Available() &&
        InFeatureID == NVSDK_NGX_Feature_FrameGeneration)
    {
        LOG_INFO("Passthrough to DLSSG Replacement's CreateFeature for FrameGeneration");

        NVSDK_NGX_Result res = Nvngx_FG::D3D12_CreateFeature(InCmdList, InFeatureID, InParameters, OutHandle);

        if (*OutHandle)
        {
            LOG_INFO("Created modded DLSSG feature with HandleId: {}", (*OutHandle)->Id);
            HandleToFeature[(*OutHandle)->Id] = InFeatureID;
        }

        return res;
    }

    // Native DLSS passthrough (exclude SuperSampling and RayReconstruction)
    if (InFeatureID != NVSDK_NGX_Feature_SuperSampling && InFeatureID != NVSDK_NGX_Feature_RayReconstruction)
    {
        if (cfg.DLSSEnabled.value_or_default() && NVNGXProxy::InitDx12(D3D12Device) &&
            NVNGXProxy::D3D12_CreateFeature() != nullptr)
        {
            LOG_INFO("Passthrough to native NGX CreateFeature for feature {}", (int) InFeatureID);

            NVSDK_NGX_Result res = NVNGXProxy::D3D12_CreateFeature()(InCmdList, InFeatureID, InParameters, OutHandle);

            if (*OutHandle)
            {
                LOG_INFO("Native CreateFeature success, HandleId: {}", (*OutHandle)->Id);
                HandleToFeature[(*OutHandle)->Id] = InFeatureID;
            }
            else
            {
                LOG_INFO("Native CreateFeature failed: 0x{:X}", (uint32_t) res);
            }

            return res;
        }

        LOG_WARN("Native DLSS passthrough not available for feature {}", (int) InFeatureID);
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }

    // OptiScaler internal handling (SuperSampling or RayReconstruction)
    auto tryResult = TryCreateOptiFeature(InCmdList, InFeatureID, InParameters, OutHandle);

    if (tryResult == NVSDK_NGX_Result_Success)
        HandleToFeature[(*OutHandle)->Id] = InFeatureID;

    return tryResult;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* InHandle)
{
    LOG_FUNC();

    if (!InHandle)
        return NVSDK_NGX_Result_Success;

    auto handleId = InHandle->Id;
    ofxr_bridge::retire_dlss_guide_stream(handleId);

    // Neural Rendering owns one temporal feature per source handle. Retire that history with the
    // upscaler context instead of leaving it available for a later handle that happens to reuse the
    // same dimensions.
    DlssNr::ReleaseForHandle(handleId);

    // Clean up framegen
    if (State::Instance().currentFG != nullptr && State::Instance().activeFgInput == FGInput::Upscaler)
    {
        State::Instance().fgChanged = true;
        State::Instance().currentFG->DestroyFGContext();
        State::Instance().clearCapturedHudlesses = true;
        UpscalerInputsDx12::Reset();
    }

    if (!shutdown)
        LOG_INFO("releasing feature with id {0}", handleId);

    // OptiScaler handles start after this offset. If it's outside this range, it doesn't belong to OptiScaler.
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::D3D12_ReleaseFeature() != nullptr)
        {
            if (!shutdown)
                LOG_INFO("calling D3D12_ReleaseFeature for ({0})", handleId);

            // Clean up real DLSS feature
            auto result = NVNGXProxy::D3D12_ReleaseFeature()(InHandle);

            if (!shutdown)
                LOG_INFO("D3D12_ReleaseFeature result for ({0}): {1:X}", handleId, (UINT) result);

            return result;
        }
        else
        {
            if (!shutdown)
                LOG_INFO("D3D12_ReleaseFeature not available for ({0})", handleId);

            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        }
    }
    // Clean up OptiScaler feature with framegen
    else if (State::Instance().activeFgNvngx != FGNvngxReplacement::None && handleId >= NVNGX_PROVIDER_ID_OFFSET)
    {
        LOG_INFO("D3D12_ReleaseFeature modded DLSSG with HandleId: {0}", handleId);
        return Nvngx_FG::D3D12_ReleaseFeature(InHandle);
    }

    // Remove feature from context map
    if (auto it = Dx12Contexts.find(handleId); it != Dx12Contexts.end())
    {
        auto& entry = it->second;

        if (auto* deviceContext = entry.feature.get())
        {
            // Clear global reference if it matches
            if (deviceContext == State::Instance().currentFeature)
                State::Instance().currentFeature = nullptr;

            // Erase from map (smart pointer reset is implicit on erase)
            Dx12Contexts.erase(it);
        }
    }
    else
    {
        // Fallback Error Handling
        if (!shutdown)
            LOG_ERROR("can't release feature with id {0}!", handleId);
    }

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Used by the client application to check for feature support.
 * @param Adapter Device the feature is for.
 * @param FeatureDiscoveryInfo Specifies the feature being queried.
 * @param OutSupported Used to indicate whether a feature is supported and its requirements.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetFeatureRequirements(
    IDXGIAdapter* Adapter, const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo,
    NVSDK_NGX_FeatureRequirement* OutSupported)
{
    LOG_DEBUG("for ({0})", (int) FeatureDiscoveryInfo->FeatureID);

    const bool isUpscaling = FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_SuperSampling;
    const bool isFG = FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_FrameGeneration;
    const bool dlssgAdjacent = Nvngx_FG::isDx12Available() || State::Instance().activeFgInput == FGInput::DLSSG;

    if (isUpscaling || (isFG && dlssgAdjacent))
    {
        if (OutSupported == nullptr)
        {
            static auto tmp = NVSDK_NGX_FeatureRequirement();
            OutSupported = &tmp;
        }

        OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        OutSupported->MinHWArchitecture = 0;

        // Some old windows 10 os version
        strcpy_s(OutSupported->MinOSVersion, "10.0.10240.16384");
        return NVSDK_NGX_Result_Success;
    }

    if (Config::Instance()->DLSSEnabled.value_or_default() && IdentifyGpu::getPrimaryGpu().dlssCapable &&
        NVNGXProxy::NVNGXModule() == nullptr)
    {
        NVNGXProxy::InitNVNGX();
    }

    if (Config::Instance()->DLSSEnabled.value_or_default() && IdentifyGpu::getPrimaryGpu().dlssCapable &&
        NVNGXProxy::D3D12_GetFeatureRequirements() != nullptr)
    {
        LOG_DEBUG("D3D12_GetFeatureRequirements for ({0})", (int) FeatureDiscoveryInfo->FeatureID);
        auto result = NVNGXProxy::D3D12_GetFeatureRequirements()(Adapter, FeatureDiscoveryInfo, OutSupported);
        LOG_DEBUG("D3D12_GetFeatureRequirements result for ({0}): {1:X}", (int) FeatureDiscoveryInfo->FeatureID,
                  (UINT) result);

        return result;
    }
    else
    {
        LOG_DEBUG("D3D12_GetFeatureRequirements not available for ({0})", (int) FeatureDiscoveryInfo->FeatureID);
    }

    OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_AdapterUnsupported;
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

static NVSDK_NGX_Result TryEvaluateOptiFeature(ID3D12GraphicsCommandList* InCmdList,
                                               const NVSDK_NGX_Handle* InFeatureHandle,
                                               NVSDK_NGX_Parameter* InParameters,
                                               PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    State& state = State::Instance();
    const Config& cfg = *Config::Instance();
    const uint32_t handleId = InFeatureHandle->Id;

    auto ctxIt = Dx12Contexts.find(handleId);

    if (ctxIt == Dx12Contexts.end())
    {
        LOG_WARN("No context found for handle {}", handleId);
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    ContextData<IFeature_Dx12>& ctxData = ctxIt->second;
    IFeature_Dx12* feature = ctxData.feature.get();

    if (feature == nullptr) // Prevent source api name flicker when dlssg is active
        state.setInputApiName = state.currentInputApiName;

    const auto targetApiName =
        !state.setInputApiName.has_value() ? ApiUpscalerInput::DLSS_DX12 : state.setInputApiName.value();

    if (state.currentInputApiName != targetApiName)
        state.currentInputApiName = targetApiName;

    state.setInputApiName.reset();
    evalCounter++;

    // Skip evaluation for the first N frames if configured
    if (cfg.SkipFirstFrames.has_value() && evalCounter < cfg.SkipFirstFrames.value())
        return NVSDK_NGX_Result_Success;

    // Root signature restoration setup
    const bool restoreCompute = cfg.RestoreComputeSignature.value_or_default();
    const bool restoreGraphics = cfg.RestoreGraphicSignature.value_or_default();
    const bool shouldRestoreSigs = restoreCompute || restoreGraphics;

    if (shouldRestoreSigs)
    {
        D3D12Hooks::HookToCommandListLate(InCmdList);

        if (!D3D12Hooks::CanRestoreRootSignature(InCmdList))
        {
            LOG_DEBUG("Skipping upscaling because can't restore root signature");
            return NVSDK_NGX_Result_Success;
        }
    }

    if (InCallback)
        LOG_INFO("Progress callback provided but unused in synchronous OptiScaler path");

    // Resolution change detection (only for upscalers that may require recreation)
    if (feature != nullptr)
    {
        const bool isFFX =
            feature->GetUpscalerType() == Upscaler::FFX || feature->GetUpscalerType() == Upscaler::FFX_on12;
        const bool isFSR31OrLater = isFFX && feature->Version() >= feature_version { 3, 1, 0 };

        // FSR 3.1 supports upscaleSize that doesn't need reinit to change output resolution
        if (!isFSR31OrLater && feature->UpdateOutputResolution(InParameters))
            state.changeBackend[handleId] = true;
    }

    // To avoid capturing potential upscaler change (creation) and then upscaling itself
    D3D12Hooks::SetRootSignatureTracking(false);

    // Backend change or recreation requested
    if (state.changeBackend[handleId])
    {
        UpscalerInputsDx12::Reset();

        auto successfulPhase = FeatureProvider_Dx12::ChangeFeature(state.newBackend, D3D12Device, InCmdList, handleId,
                                                                   InParameters, &ctxData);
        feature = ctxData.feature.get();

        evalCounter = 0;

        if (ctxData.changeBackendCounter != 0 || !successfulPhase)
        {
            D3D12Hooks::SetRootSignatureTracking(true);
            return NVSDK_NGX_Result_Success;
        }
    }

    // Fallback to FSR 2.1.2 if feature failed to initialize and user didn't explicitly request it
    if (!feature->IsInited() && cfg.Dx12Upscaler.value_or_default() != Upscaler::FSR21)
    {
        LOG_WARN("Feature '{}' failed to initialize. Falling back to FSR 2.1.2", feature->Name());
        ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to FSR 2.1.2" });

        state.newBackend = Upscaler::FSR21;
        state.changeBackend[handleId] = true;

        D3D12Hooks::SetRootSignatureTracking(true);

        return NVSDK_NGX_Result_Success;
    }

    state.currentFeature = feature;

    // Prepare upscaling inputs
    UpscalerInputsDx12::UpscaleStart(InCmdList, InParameters, feature);
    FSR3FG::SetUpscalerInputs(InCmdList, InParameters, feature);

    // Evaluate the feature
    bool evalSuccess = false;
    {
        // Resource tracking
        UpscalerInputsDx12::UpscaleEnd(InCmdList, InParameters, feature);

        ScopedSkipHeapCapture skip {};
        // Run inside OptiScaler's protected command-list scope, after input tracking and before SR.
        // Running outside TryEvaluate would let NR overwrite the root signature that gets restored.
        if (HandleToFeature[handleId] == NVSDK_NGX_Feature_SuperSampling &&
            Config::Instance()->DlssNrRunBeforeSr.value_or_default())
            DlssNr::EvaluateBeforeUpscale(handleId, InCmdList, InParameters);
        static constexpr wchar_t kSrBegin[] = L"OptiScaler V23216 DLSS SR begin";
        static constexpr wchar_t kSrComplete[] = L"OptiScaler V23216 DLSS SR complete";
        DredMarkers::Set(InCmdList, kSrBegin);
        evalSuccess = feature->Evaluate(InCmdList, InParameters);
        NrFlightTrace::Result(InCmdList, L"OptiScaler V23216 DLSS SR result", evalSuccess ? 1 : 0);
        DredMarkers::Set(InCmdList, kSrComplete);
        if (evalSuccess && HandleToFeature[handleId] == NVSDK_NGX_Feature_SuperSampling &&
            !Config::Instance()->DlssNrRunBeforeSr.value_or_default())
            DlssNr::EvaluateAfterUpscale(handleId, InCmdList, InParameters);
    }

    if (!evalSuccess)
    {
        LOG_ERROR("Feature evaluation failed for '{}'", feature->Name());
        ImGui::InsertNotification({ ImGuiToastType::Error, 10000, "Upscaler failed to run!" });
    }

    // Restore root signatures
    if (shouldRestoreSigs)
        D3D12Hooks::RestoreRoot(InCmdList);

    D3D12Hooks::SetRootSignatureTracking(true);

    return evalSuccess ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

/**
 * @brief Per-frame feature execution. Runs a feature (upscaler, framegen, etc.) on a given command list using a
 * preexisting feature instance referenced by a unique handle.
 */
static NVSDK_NGX_Result EvaluateFeatureImplementationDx12(ID3D12GraphicsCommandList* InCmdList,
                                                               const NVSDK_NGX_Handle* InFeatureHandle,
                                                               NVSDK_NGX_Parameter* InParameters,
                                                               PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    if (!InFeatureHandle)
    {
        LOG_DEBUG("InFeatureHandle is null");
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    if (!InCmdList)
    {
        LOG_ERROR("InCmdList is null");
        return NVSDK_NGX_Result_Fail;
    }

    const uint32_t handleId = InFeatureHandle->Id;
    LOG_DEBUG("EvaluateFeature - Handle: {}, CmdList: {:p}", handleId, (void*) InCmdList);

    const State& state = State::Instance();
    const Config& cfg = *Config::Instance();

    const auto feature = ResolveEvaluatedFeature(handleId, InParameters);
    static size_t evalWithoutFG = 0;
    bool fgCreated = std::any_of(HandleToFeature.begin(), HandleToFeature.end(),
                                 [](const auto& pair) { return pair.second == NVSDK_NGX_Feature_FrameGeneration; });

    static std::optional<float> lastDlssgCameraNear {};
    static std::optional<float> lastDlssgCameraFar {};

    if (feature == NVSDK_NGX_Feature_FrameGeneration)
    {
        evalWithoutFG = 0;

        int frameCount = 0;
        InParameters->Get("DLSSG.MultiFrameCount", &frameCount);
        State::Instance().dlssgDetectedInterpolationCount = frameCount;
        ReflexHooks::setDlssgFrameCount(frameCount);

        float dlssgCameraNear = 0.0f;
        float dlssgCameraFar = 0.0f;

        if (InParameters->Get("DLSSG.CameraNear", &dlssgCameraNear) == NVSDK_NGX_Result_Success)
            lastDlssgCameraNear = dlssgCameraNear;

        if (InParameters->Get("DLSSG.CameraFar", &dlssgCameraFar) == NVSDK_NGX_Result_Success)
            lastDlssgCameraFar = dlssgCameraFar;
    }
    else if (fgCreated)
    {
        evalWithoutFG++;

        if (evalWithoutFG == 6)
        {
            // Report FG as disabled
            State::Instance().dlssgDetectedInterpolationCount = 0;
            ReflexHooks::setDlssgFrameCount(0);
        }
    }

    // Native DLSS passthrough
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (cfg.DLSSEnabled.value_or_default() && NVNGXProxy::D3D12_EvaluateFeature() != nullptr)
        {
            LOG_DEBUG("Passthrough to native DLSS EvaluateFeature for handle {}", handleId);

            // Offer NR only to ordinary DLSS SR; RR and frame generation remain outside this trial.
            if (feature == NVSDK_NGX_Feature_SuperSampling &&
                cfg.DlssNrRunBeforeSr.value_or_default())
                DlssNr::EvaluateBeforeUpscale(handleId, InCmdList, InParameters);

            if (feature == NVSDK_NGX_Feature_SuperSampling) {
                LukeRossDlssIngress::PublishD3D12Guide(
                    handleId, InCmdList, InParameters, nullptr);
            }

            static constexpr wchar_t kNativeSrBegin[] = L"OptiScaler V23216 native DLSS SR begin";
            static constexpr wchar_t kNativeSrComplete[] = L"OptiScaler V23216 native DLSS SR complete";
            DredMarkers::Set(InCmdList, kNativeSrBegin);
            NVSDK_NGX_Result result =
                NVNGXProxy::D3D12_EvaluateFeature()(InCmdList, InFeatureHandle, InParameters, InCallback);
            NrFlightTrace::Result(InCmdList, L"OptiScaler V23216 native DLSS SR result", result);
            DredMarkers::Set(InCmdList, kNativeSrComplete);
            LOG_DEBUG("Native DLSS EvaluateFeature result: 0x{:X}", (uint32_t) result);

            if (result == NVSDK_NGX_Result_Success && feature == NVSDK_NGX_Feature_SuperSampling &&
                !cfg.DlssNrRunBeforeSr.value_or_default())
                DlssNr::EvaluateAfterUpscale(handleId, InCmdList, InParameters);

            return result;
        }

        LOG_DEBUG("Native DLSS EvaluateFeature not available for handle {}", handleId);
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    // DLSSG replacements passthrough
    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None && handleId >= NVNGX_PROVIDER_ID_OFFSET)
    {
        LOG_DEBUG("Passthrough to DLSSG Replacement's EvaluateFeature for handle {}", handleId);
        return Nvngx_FG::D3D12_EvaluateFeature(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    if (lastDlssgCameraNear.has_value())
        InParameters->Set("DLSSG.CameraNear", lastDlssgCameraNear.value());

    if (lastDlssgCameraFar.has_value())
        InParameters->Set("DLSSG.CameraFar", lastDlssgCameraFar.value());

    // OptiScaler internal handling
    const NVSDK_NGX_Result optiResult = TryEvaluateOptiFeature(InCmdList, InFeatureHandle, InParameters, InCallback);

    return optiResult;
}

HRESULT LukeRossDlssIngress::CompleteD3D12Color(
    ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters,
    IFeature* feature, bool ownedOutput, bool nestedEvaluation)
{
    const auto color_origin = ofxr_bridge::take_luke_origin_frame();
    if (LukeRossDlssIngress::Detected()) {
        const auto mode = ofxr_bridge::afw_snapshot().desired.mode;
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            (color_origin ? 1 : 0) | (mode != 0 ? 2 : 0) | (nestedEvaluation ? 4 : 0),
            131, color_origin ? color_origin->publication : 0,
            color_origin ? color_origin->color_origin_token : 0);
    }
    if (color_origin &&
        ofxr_bridge::afw_snapshot().desired.mode != 0) {
        HRESULT marker_result = E_UNEXPECTED;
        constexpr unsigned kOriginNested = 1, kOriginMissingOutput = 2, kOriginMissingContext = 4,
            kOriginDimensions = 8, kOriginResourceAdapter = 16, kOriginListIdentity = 32,
            kOriginUnsupported = 64;
        unsigned marker_failures = nestedEvaluation ? kOriginNested : 0;
        xrfg::LukeColorOriginRect rect{};
        D3D12_RESOURCE_DESC final_desc{};
        ID3D12Resource* final_output = nullptr;
        auto* native_list = ResolveD3DNativeInterface(commandList);
        if (parameters && parameters->Get(NVSDK_NGX_Parameter_Output, &final_output) == NVSDK_NGX_Result_Success && final_output) {
            final_output = ResolveD3DNativeInterface(final_output);
        }
        if (!final_output) marker_failures |= kOriginMissingOutput;
        const auto final_device = ResolveD3DNativeDevice(final_output);
        const auto list_device = ResolveD3DNativeDevice(native_list);
        const bool owned_sr = ownedOutput;
        if (SameCanonicalIdentity(final_output, color_origin->output.Get())) {
            rect = {color_origin->output_x, color_origin->output_y,
                    color_origin->output_width, color_origin->output_height};
        } else {
            // Evaluate params need not repeat creation-only OutWidth/OutHeight.
            // Owned postprocessing restores the feature's DISPLAY target, not
            // the SR/OutputScaling intermediate's TARGET allocation dimensions.
            if (owned_sr && feature) {
                rect.x = color_origin->output_x; rect.y = color_origin->output_y;
                rect.width = feature->DisplayWidth();
                rect.height = feature->DisplayHeight();
            } else marker_failures |= kOriginMissingContext;
        }
        if (final_output) final_desc = final_output->GetDesc();
        if (rect.width < xrfg::kLukeOriginWidth || !rect.height || rect.height > 8192 ||
            static_cast<UINT64>(rect.x) + rect.width > final_desc.Width ||
            static_cast<UINT64>(rect.y) + rect.height > final_desc.Height)
            marker_failures |= kOriginDimensions;
        // Match the validated V23090 ingress contract: resources may expose
        // distinct device interfaces on the same adapter; list/queue ownership
        // uses the same proven backing owner and authoritative allocation device.
        if (!SameAdapter(final_device.Get(), color_origin->verified_producer_device.Get()))
            marker_failures |= kOriginResourceAdapter;
        if (!d3d_owner::same(list_device.Get(), color_origin->verified_producer_device.Get()))
            marker_failures |= kOriginListIdentity;
        if (final_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || final_desc.SampleDesc.Count != 1 ||
            final_desc.DepthOrArraySize != 1 || !xrfg::luke_origin_pixel_bytes(final_desc.Format))
            marker_failures |= kOriginUnsupported;
        if (marker_failures) {
            marker_result = (marker_failures & kOriginNested) ? E_UNEXPECTED : E_INVALIDARG;
        } else {
            // Owned DLSS final shaders leave UAV; its pipeline does not
            // honor the native passthrough OutputResourceBarrier setting.
            const auto state = owned_sr ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                : static_cast<D3D12_RESOURCE_STATES>(Config::Instance()->OutputResourceBarrier.value_or(
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            marker_result = xrfg::mark_luke_color_origin(native_list, final_output,
                color_origin->color_origin_token, state, color_origin->verified_producer_device.Get(), rect);
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            marker_result, 91, color_origin->publication, color_origin->camera_frame_token);
        return marker_result;
    }
    return S_FALSE;
}
// ONE SR completion point, after native SR or owned SR+postprocessing+NR.
// The immutable guide's color token comes from that evaluation's input, never latest.
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* InCmdList,
                                                               const NVSDK_NGX_Handle* InFeatureHandle,
                                                               NVSDK_NGX_Parameter* InParameters,
                                                               PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    static thread_local unsigned completion_depth = 0;
    static thread_local bool nested_evaluation = false;
    struct CompletionScope {
        unsigned& depth;
        bool outermost;
        explicit CompletionScope(unsigned& value) : depth(value), outermost(value++ == 0) {}
        ~CompletionScope() { --depth; }
    } scope(completion_depth);
    if (!scope.outermost) {
        // Ordinary NR calls the raw proxy, not this export. If a provider
        // reenters, do not clear or deliver the enclosing production's TLS ID.
        // The enclosing completion rejects the ambiguous origin explicitly.
        nested_evaluation = true;
        return EvaluateFeatureImplementationDx12(InCmdList, InFeatureHandle, InParameters, InCallback);
    }
    nested_evaluation = false;
    static_cast<void>(ofxr_bridge::take_luke_origin_frame());
    const auto result = EvaluateFeatureImplementationDx12(InCmdList, InFeatureHandle, InParameters, InCallback);
    if (result == NVSDK_NGX_Result_Success) {
        const auto context = InFeatureHandle ? Dx12Contexts.find(InFeatureHandle->Id) : Dx12Contexts.end();
        IFeature* feature = context != Dx12Contexts.end() ? context->second.feature.get() : nullptr;
        LukeRossDlssIngress::CompleteD3D12Color(InCmdList, InParameters, feature,
            InFeatureHandle && InFeatureHandle->Id >= DLSS_MOD_ID_OFFSET, nested_evaluation);
    } else static_cast<void>(ofxr_bridge::take_luke_origin_frame());
    return result;
}

#pragma endregion

#pragma region DLSS Buffer Size Call

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature InFeatureId,
                                                                    const NVSDK_NGX_Parameter* InParameters,
                                                                    size_t* OutSizeInBytes)
{
    if (OutSizeInBytes == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None && InFeatureId == NVSDK_NGX_Feature_FrameGeneration)
    {
        return Nvngx_FG::D3D12_GetScratchBufferSize(InFeatureId, InParameters, OutSizeInBytes);
    }

    LOG_WARN("-> 52428800");
    *OutSizeInBytes = 52428800;
    return NVSDK_NGX_Result_Success;
}

#pragma endregion
