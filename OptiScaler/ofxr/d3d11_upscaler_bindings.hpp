#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>

namespace ofxr_bridge {
// Get* returns owned references. Keep them until Set* has rebound the objects,
// even if the model unbinds them and releases its last application reference.
class D3D11UpscalerBindings final {
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    std::array<ID3D11ShaderResourceView*,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources{};
    std::array<ID3D11SamplerState*,D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers{};
    std::array<ID3D11Buffer*,D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> buffers{};
    std::array<ID3D11UnorderedAccessView*,D3D11_1_UAV_SLOT_COUNT> unordered{};
    std::array<ID3D11RenderTargetView*,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    std::array<ID3D11ClassInstance*,D3D11_SHADER_MAX_INTERFACES> instances{};
    ID3D11DepthStencilView* depth{};
    ID3D11ComputeShader* shader{};
    UINT unordered_count{},instance_count=static_cast<UINT>(instances.size());
    template<class T,std::size_t N> static void release(std::array<T*,N>& objects) noexcept {
        for (auto* object:objects) if (object) object->Release();
    }
public:
    explicit D3D11UpscalerBindings(ID3D11DeviceContext* ctx):context(ctx) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(device.GetAddressOf());
        unordered_count=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?D3D11_1_UAV_SLOT_COUNT:D3D11_PS_CS_UAV_REGISTER_COUNT;
        context->CSGetShaderResources(0,static_cast<UINT>(resources.size()),resources.data());
        context->CSGetSamplers(0,static_cast<UINT>(samplers.size()),samplers.data());
        context->CSGetConstantBuffers(0,static_cast<UINT>(buffers.size()),buffers.data());
        context->CSGetUnorderedAccessViews(0,unordered_count,unordered.data());
        context->CSGetShader(&shader,instances.data(),&instance_count);
        context->OMGetRenderTargets(static_cast<UINT>(targets.size()),targets.data(),&depth);
        context->OMSetRenderTargets(0,nullptr,nullptr);
    }
    D3D11UpscalerBindings(const D3D11UpscalerBindings&)=delete;
    D3D11UpscalerBindings& operator=(const D3D11UpscalerBindings&)=delete;
    ~D3D11UpscalerBindings() noexcept {
        // Remove model bindings first, including formerly empty slots, so they
        // cannot conflict with the saved outputs/inputs. Preserve UAV counters.
        std::array<ID3D11ShaderResourceView*,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> no_resources{};
        std::array<ID3D11UnorderedAccessView*,D3D11_1_UAV_SLOT_COUNT> no_unordered{};
        context->CSSetShaderResources(0,static_cast<UINT>(no_resources.size()),no_resources.data());
        context->CSSetUnorderedAccessViews(0,unordered_count,no_unordered.data(),nullptr);
        context->OMSetRenderTargets(static_cast<UINT>(targets.size()),targets.data(),depth);
        context->CSSetUnorderedAccessViews(0,unordered_count,unordered.data(),nullptr);
        context->CSSetShaderResources(0,static_cast<UINT>(resources.size()),resources.data());
        context->CSSetSamplers(0,static_cast<UINT>(samplers.size()),samplers.data());
        context->CSSetConstantBuffers(0,static_cast<UINT>(buffers.size()),buffers.data());
        context->CSSetShader(shader,instances.data(),instance_count);
        release(resources);release(samplers);release(buffers);release(unordered);release(targets);release(instances);
        if (depth) depth->Release();
        if (shader) shader->Release();
    }
};
}
