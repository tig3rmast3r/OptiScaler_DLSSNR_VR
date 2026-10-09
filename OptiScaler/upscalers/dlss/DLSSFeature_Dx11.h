#pragma once
#include <upscalers/IFeature_Dx11.h>
#include "DLSSFeature.h"
#include <string>
namespace ofxr_bridge { class CspD3D11SuperResolution; }

class DLSSFeatureDx11 : public DLSSFeature, public IFeature_Dx11
{
  private:
    std::unique_ptr<ofxr_bridge::CspD3D11SuperResolution> CspSuperResolution;
    unsigned nativeWidth{},nativeHeight{},nativeOutWidth{},nativeOutHeight{};
    bool RenderCspSuperResolution(ID3D11DeviceContext*,NVSDK_NGX_Parameter*,unsigned,unsigned);
  protected:
  public:
    bool InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters) override;
    bool Evaluate(ID3D11DeviceContext*,NVSDK_NGX_Parameter*) override;
    void CompleteNativeGuides(ID3D11DeviceContext*,NVSDK_NGX_Parameter*,bool) override;

    feature_version Version() override { return DLSSFeature::Version(); }
    Upscaler GetUpscalerType() const final { return DLSSFeature::GetUpscalerType(); }
    API Api() const override { return IFeature_Dx11::Api(); }
    bool CallsUpscalerEndByItself() override { return IFeature_Dx11::CallsUpscalerEndByItself(); }

    bool IsWithDx12() override { return false; }

    DLSSFeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~DLSSFeatureDx11();
};
