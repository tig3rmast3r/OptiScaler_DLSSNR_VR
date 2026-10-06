#pragma once

template <typename FeatureType> struct ContextData
{
    std::unique_ptr<FeatureType> feature;
    NVSDK_NGX_Parameter* createParams = nullptr;
    int changeBackendCounter = 0;
};

class IFeature_Dx12;
class IFeature;

namespace LukeRossDlssIngress
{
bool Detected() noexcept;
void PublishD3D12Guide(unsigned int handleId,
                       ID3D12GraphicsCommandList* commandList,
                       NVSDK_NGX_Parameter* parameters,
                       IFeature* optiFeature,
                       ID3D12CommandQueue* producerQueue = nullptr);
// Shared final-output writer. Vulkan interop calls it only AFTER native Vulkan
// SR/postprocessing and GPU-ordered transfer, never instead of native DLSS.
HRESULT CompleteD3D12Color(ID3D12GraphicsCommandList* commandList,
                         NVSDK_NGX_Parameter* parameters, IFeature* feature,
                         bool ownedOutput, bool nestedEvaluation = false);
}
