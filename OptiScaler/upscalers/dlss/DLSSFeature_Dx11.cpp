#include <pch.h>
#include "DLSSFeature_Dx11.h"
#include <Config.h>

#include <dxgi.h>
#include <Util.h>
#include <ofxr/csp_super_resolution.hpp>
#include "../../../third_party/ofxr/include/xrfg/d3d11_dlss_guides.hpp"

bool DLSSFeatureDx11::InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");

        SetInit(false);
        return false;
    }

    NVSDK_NGX_Result nvResult;
    bool initResult = false;

    do
    {
        if (!_dlssInited)
        {
            _dlssInited = NVNGXProxy::InitDx11(Device);

            if (!_dlssInited)
                return false;

            _moduleLoaded =
                (NVNGXProxy::D3D11_Init_ProjectID() != nullptr || NVNGXProxy::D3D11_Init_Ext() != nullptr) &&
                (NVNGXProxy::D3D11_Shutdown() != nullptr || NVNGXProxy::D3D11_Shutdown1() != nullptr) &&
                (NVNGXProxy::D3D11_GetParameters() != nullptr || NVNGXProxy::D3D11_AllocateParameters() != nullptr) &&
                NVNGXProxy::D3D11_DestroyParameters() != nullptr && NVNGXProxy::D3D11_CreateFeature() != nullptr &&
                NVNGXProxy::D3D11_ReleaseFeature() != nullptr && NVNGXProxy::D3D11_EvaluateFeature() != nullptr;

            // delay between init and create feature
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        const bool csp_xr=_wcsicmp(Util::ExePath().filename().c_str(),L"acs.exe")==0 &&
            xrfg::has_d3d11_sr_destinations(Device);
        if (csp_xr) {
            if (!NVNGXProxy::D3D11_CreateFeature() || !NVNGXProxy::D3D11_EvaluateFeature() ||
                !NVNGXProxy::D3D11_ReleaseFeature() || !NVNGXProxy::D3D11_AllocateParameters() || !NVNGXProxy::D3D11_DestroyParameters()) {
                LOG_ERROR("CSP owned SR: required native NGX entry points unavailable");break;
            }
            _moduleLoaded=true;
            ProcessInitParams(InParameters);
            CspSuperResolution=std::make_unique<ofxr_bridge::CspD3D11SuperResolution>(
                InContext,InParameters,ofxr_bridge::CspD3D11SuperResolution::ParametersApi{
                    [](NVSDK_NGX_Parameter** params) {return NVNGXProxy::D3D11_AllocateParameters()?NVNGXProxy::D3D11_AllocateParameters()(params):NVSDK_NGX_Result_Fail;},
                    [](NVSDK_NGX_Parameter* params) {if (!State::Instance().isShuttingDown && NVNGXProxy::D3D11_DestroyParameters()) NVNGXProxy::D3D11_DestroyParameters()(params);}
                },[this](auto* context,auto* params,unsigned width,unsigned height) {
                    return RenderCspSuperResolution(context,params,width,height);
                });
            if (!CspSuperResolution->ready()) {
                LOG_ERROR("CSP owned SR: native DX11 context/fence/parameter initialization failed");
                break;
            }
            ReadVersion();initResult=true;
            LOG_INFO("CSP owned SR: deferring native DLSS to the enumerated OpenXR eye image, handle {}",Handle()->Id);
            break;
        }
        LOG_INFO("Creating DLSS feature");

        if (NVNGXProxy::D3D11_CreateFeature() != nullptr)
        {
            ProcessInitParams(InParameters);

            _p_dlssHandle = &_dlssHandle;
            nvResult = NVNGXProxy::D3D11_CreateFeature()(InContext, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                         &_p_dlssHandle);

            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("NVNGXProxy::D3D11_CreateFeature result: {0:X}", (unsigned int) nvResult);
                break;
            }
        }
        else
        {
            LOG_ERROR("NVNGXProxy::D3D11_CreateFeature is nullptr");
            break;
        }

        ReadVersion();

        initResult = true;

    } while (false);

    SetInit(initResult);

    return initResult;
}

bool DLSSFeatureDx11::Evaluate(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* parameters) {
    if (CspSuperResolution) return CspSuperResolution->prepare(context,parameters);
    return IFeature_Dx11::Evaluate(context,parameters);
}
void DLSSFeatureDx11::CompleteNativeGuides(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* parameters,bool success) {
    // The successful early evaluate only produced a carrier. Its full-size
    // completion belongs to the actual native SR dispatch below.
    if (!CspSuperResolution || !success) IFeature_Dx11::CompleteNativeGuides(context,parameters,success);
}
bool DLSSFeatureDx11::RenderCspSuperResolution(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* parameters,unsigned outWidth,unsigned outHeight) {
    unsigned width{},height{};parameters->Get(NVSDK_NGX_Parameter_Width,&width);parameters->Get(NVSDK_NGX_Parameter_Height,&height);
    const bool changed=!_p_dlssHandle || nativeWidth!=width || nativeHeight!=height || nativeOutWidth!=outWidth || nativeOutHeight!=outHeight;
    if (changed) {
        // The owner has already drained the old work before any extent change.
        if (_p_dlssHandle) {NVNGXProxy::D3D11_ReleaseFeature()(_p_dlssHandle);_p_dlssHandle=nullptr;}
        _renderWidth=width;_renderHeight=height;_displayWidth=outWidth;_displayHeight=outHeight;
        ProcessInitParams(parameters);
        _targetWidth=outWidth;_targetHeight=outHeight;
        parameters->Set(NVSDK_NGX_Parameter_OutWidth,outWidth);parameters->Set(NVSDK_NGX_Parameter_OutHeight,outHeight);
        NVSDK_NGX_Handle* created{};
        const auto result=NVNGXProxy::D3D11_CreateFeature()(context,NVSDK_NGX_Feature_SuperSampling,parameters,&created);
        if (result!=NVSDK_NGX_Result_Success || !created) {
            LOG_ERROR("CSP owned SR: full-size DLSS create failed {:X}, {}x{} -> {}x{}",static_cast<unsigned>(result),width,height,outWidth,outHeight);
            IFeature_Dx11::CompleteNativeGuides(context,parameters,false);return false;
        }
        _p_dlssHandle=created;nativeWidth=width;nativeHeight=height;nativeOutWidth=outWidth;nativeOutHeight=outHeight;
        parameters->Set(NVSDK_NGX_Parameter_Reset,1u);
    }
    const auto success=IFeature_Dx11::Evaluate(context,parameters);
    IFeature_Dx11::CompleteNativeGuides(context,parameters,success);
    return success;
}

bool DLSSFeatureDx11::EvaluateInternal(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D11_EvaluateFeature() != nullptr)
    {
        ProcessEvaluateParams(InParameters);

        nvResult = NVNGXProxy::D3D11_EvaluateFeature()(InDeviceContext, _p_dlssHandle, InParameters, NULL);

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }

        LOG_TRACE("_EvaluateFeature ok!");
    }
    else
    {
        LOG_ERROR("_EvaluateFeature is nullptr");
        return false;
    }

    return true;
}

DLSSFeatureDx11::DLSSFeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx11(InHandleId, InParameters), DLSSFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSFeatureDx11::~DLSSFeatureDx11()
{
    if (CspSuperResolution && !CspSuperResolution->retire()) return;
    CspSuperResolution.reset();
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D11_ReleaseFeature() != nullptr && _p_dlssHandle != nullptr)
        NVNGXProxy::D3D11_ReleaseFeature()(_p_dlssHandle);
}
