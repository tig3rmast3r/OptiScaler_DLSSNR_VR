#pragma once

template <typename FeatureType> struct ContextData
{
    std::unique_ptr<FeatureType> feature;
    NVSDK_NGX_Parameter* createParams = nullptr;
    int changeBackendCounter = 0;
};

class IFeature_Dx12;

namespace LukeRossDlssIngress
{
bool Detected() noexcept;
void PublishD3D12Guide(unsigned int handleId,
                       ID3D12GraphicsCommandList* commandList,
                       NVSDK_NGX_Parameter* parameters,
                       IFeature_Dx12* optiFeature);
}
