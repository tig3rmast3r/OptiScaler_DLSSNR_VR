#include "pch.h"
#include "menu_dx12.h"
#include "menu_common.h"
namespace {
void MenuStateEvidence(ID3D12GraphicsCommandList* list,const MenuCommandState::State* state,bool restored)
{
    // Spend the bounded evidence budget on the opened menu, not earlier FPS-only draws.
    if(!MenuCommon::IsVisible()) return;
    static std::atomic_uint snapshots{}, completions{};
    auto& counter=restored?completions:snapshots;
    if(counter.fetch_add(1)>=16) return;
    if(!state) { LOG_WARN("V33023 menu skipped: no application state captured list:{:p}",(void*)list); return; }
    ID3D12DescriptorHeap* srv=nullptr; ID3D12DescriptorHeap* sampler=nullptr;
    for(const auto& heap:state->heaps) {
        const auto type=heap->GetDesc().Type;
        if(type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) sampler=heap.Get();
        if(type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) srv=heap.Get();
    }
    LOG_INFO("V33023 menu state phase:{} list:{:p} graphics:{:p} known:{} args:{} compute:{:p} known:{} args:{} heapsKnown:{} srv:{:p} sampler:{:p} pso:{:p}",
        restored?"restored":"captured",(void*)list,(void*)state->graphics.signature.Get(),state->graphics.known,state->graphics.arguments.size(),
        (void*)state->compute.signature.Get(),state->compute.known,state->compute.arguments.size(),state->heapsKnown,(void*)srv,(void*)sampler,(void*)state->pipeline.Get());
}
}

Menu_Dx12::Menu_Dx12(HWND handle, ID3D12Device* device) : MenuDxBase(handle)
{
    if (!Config::Instance()->UseUpscalerMenu() || !device) return;
    MenuCommandState::observer=MenuStateEvidence;
    renderer_ = std::make_unique<InUpscalerDx12Renderer>(device);
    if (renderer_->Init()) {
        Dx12Ready();
        LOG_INFO("V33023 in-upscaler menu tracks native command implementations on each actual D3D12 device");
    }
}

bool Menu_Dx12::Render(ID3D12GraphicsCommandList* list, ID3D12Resource* output)
{
    if (!Config::Instance()->UseUpscalerMenu() || !renderer_ || !list || !output) return false;
    std::lock_guard lock(renderMutex_);
    MenuGpuLifetime::Collect();
    if (!MenuGpuLifetime::Ready()) return false;
    const auto desc = output->GetDesc();
    MenuCommon::SetRenderExtent(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
    if (!renderer_->Init() || !MenuDxBase::RenderMenu()) return true;
    return renderer_->Render(ImGui::GetDrawData(), list, output, TranslateTypelessFormats(desc.Format));
}

Menu_Dx12::~Menu_Dx12()
{
    renderer_.reset();
    // GPU snapshots remain owned by recordings/submissions after the UI dies.
    MenuCommon::Shutdown();
}
