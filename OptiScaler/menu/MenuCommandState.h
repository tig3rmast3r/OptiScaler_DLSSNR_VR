#pragma once
#include "MenuGpuLifetime.h"
#include <array>
#include <bitset>

// The engine's graphics cache cannot see native bindings made by an injected menu.
// Track application state independently of the optional legacy SR restore flags.
namespace MenuCommandState
{
using Microsoft::WRL::ComPtr;
inline constexpr GUID StateGuid{0x5aa35147,0xb47b,0x452d,{0xac,0x0d,0x8d,0xc5,0x0f,0x94,0x39,0xe9}};
inline std::atomic_bool hooksReady{false};
inline thread_local bool applicationTracking=true;
inline thread_local unsigned suppression=0;
enum class Kind { Unset, Table, Constants, Cbv, Srv, Uav };
struct Argument
{
    Kind kind{};
    D3D12_GPU_DESCRIPTOR_HANDLE table{};
    D3D12_GPU_VIRTUAL_ADDRESS address{};
    std::vector<UINT> constants;
    std::bitset<64> written;
};
struct Root
{
    ComPtr<ID3D12RootSignature> signature;
    bool known{};
    std::vector<Argument> arguments;
    void Signature(ID3D12RootSignature* value)
    {
        if(signature.Get()!=value) arguments.clear();
        signature=value;
        known=true;
    }
    Argument* At(UINT index)
    {
        if(index>=64) return nullptr;
        if(arguments.size()<=index) arguments.resize(index+1);
        return &arguments[index];
    }
    void Table(UINT index,D3D12_GPU_DESCRIPTOR_HANDLE handle)
    { if(auto* a=At(index)) { *a={}; a->kind=Kind::Table; a->table=handle; } }
    void Address(UINT index,D3D12_GPU_VIRTUAL_ADDRESS address,Kind kind)
    { if(auto* a=At(index)) { *a={}; a->kind=kind; a->address=address; } }
    void Constants(UINT index,UINT count,const void* data,UINT offset)
    {
        if(!data || offset>=64 || count>64-offset) return;
        if(auto* a=At(index)) {
            if(a->kind!=Kind::Constants) { *a={}; a->kind=Kind::Constants; }
            if(a->constants.size()<offset+count) a->constants.resize(offset+count);
            for(UINT n=0;n<count;++n) { a->constants[offset+n]=static_cast<const UINT*>(data)[n]; a->written.set(offset+n); }
        }
    }
    void InvalidateTables() { for(auto& a:arguments) if(a.kind==Kind::Table) a={}; }
    void Restore(ID3D12GraphicsCommandList* list,bool graphics) const
    {
        if(graphics) list->SetGraphicsRootSignature(signature.Get());
        else list->SetComputeRootSignature(signature.Get());
        if(!signature) return;
        for(UINT i=0;i<arguments.size();++i)
        {
            const auto& a=arguments[i];
            switch(a.kind) {
            case Kind::Table:
                if(graphics) list->SetGraphicsRootDescriptorTable(i,a.table); else list->SetComputeRootDescriptorTable(i,a.table);
                break;
            case Kind::Cbv:
                if(graphics) list->SetGraphicsRootConstantBufferView(i,a.address); else list->SetComputeRootConstantBufferView(i,a.address);
                break;
            case Kind::Srv:
                if(graphics) list->SetGraphicsRootShaderResourceView(i,a.address); else list->SetComputeRootShaderResourceView(i,a.address);
                break;
            case Kind::Uav:
                if(graphics) list->SetGraphicsRootUnorderedAccessView(i,a.address); else list->SetComputeRootUnorderedAccessView(i,a.address);
                break;
            case Kind::Constants:
                for(UINT start=0;start<a.constants.size();) {
                    if(!a.written.test(start)) { ++start; continue; }
                    UINT end=start+1; while(end<a.constants.size() && a.written.test(end)) ++end;
                    if(graphics) list->SetGraphicsRoot32BitConstants(i,end-start,a.constants.data()+start,start);
                    else list->SetComputeRoot32BitConstants(i,end-start,a.constants.data()+start,start);
                    start=end;
                }
                break;
            default: break;
            }
        }
    }
};
struct State
{
    Root graphics, compute;
    ComPtr<ID3D12PipelineState> pipeline;
    std::vector<ComPtr<ID3D12DescriptorHeap>> heaps;
    bool heapsKnown{}, targetsKnown{}, viewportKnown{}, scissorKnown{}, topologyKnown{}, indexKnown{}, blendKnown{};
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> targets;
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    std::vector<D3D12_VIEWPORT> viewports;
    std::vector<D3D12_RECT> scissors;
    D3D12_PRIMITIVE_TOPOLOGY topology{};
    D3D12_INDEX_BUFFER_VIEW index{};
    std::array<D3D12_VERTEX_BUFFER_VIEW,32> vertices{};
    std::bitset<32> verticesKnown;
    std::array<float,4> blend{1,1,1,1};
    void Heaps(UINT count,ID3D12DescriptorHeap* const* value)
    {
        if(count>2 || (count && !value)) return;
        bool changed=heaps.size()!=count;
        for(UINT i=0;!changed && i<count;++i) changed=heaps[i].Get()!=value[i];
        if(changed) { graphics.InvalidateTables(); compute.InvalidateTables(); }
        heaps.clear(); for(UINT i=0;i<count;++i) heaps.emplace_back(value[i]); heapsKnown=true;
    }
    void Restore(ID3D12GraphicsCommandList* list) const
    {
        // Heap changes invalidate both graphics AND compute descriptor tables.
        if(heapsKnown) {
            ID3D12DescriptorHeap* raw[2]{}; for(size_t i=0;i<heaps.size();++i) raw[i]=heaps[i].Get();
            list->SetDescriptorHeaps(static_cast<UINT>(heaps.size()),raw);
        }
        graphics.Restore(list,true); compute.Restore(list,false);
        if(pipeline) list->SetPipelineState(pipeline.Get());
        if(targetsKnown) list->OMSetRenderTargets(static_cast<UINT>(targets.size()),targets.data(),FALSE,depth.ptr?&depth:nullptr);
        if(viewportKnown) list->RSSetViewports(static_cast<UINT>(viewports.size()),viewports.data());
        if(scissorKnown) list->RSSetScissorRects(static_cast<UINT>(scissors.size()),scissors.data());
        if(topologyKnown) list->IASetPrimitiveTopology(topology);
        if(indexKnown) list->IASetIndexBuffer(&index);
        for(UINT i=0;i<32;++i) if(verticesKnown.test(i)) list->IASetVertexBuffers(i,1,&vertices[i]);
        if(blendKnown) list->OMSetBlendFactor(blend.data());
    }
};
class Recording final : public IUnknown
{
    std::atomic_ulong references_{1};
public:
    std::mutex mutex;
    State state;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override
    { if(!out) return E_POINTER; *out=nullptr; if(iid!=__uuidof(IUnknown)) return E_NOINTERFACE; *out=this; AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n=--references_; if(!n) delete this; return n; }
};
inline ComPtr<Recording> Get(ID3D12GraphicsCommandList* list,bool create=false)
{
    ComPtr<Recording> owner; IUnknown* value{}; UINT bytes=sizeof(value);
    if(SUCCEEDED(list->GetPrivateData(StateGuid,&bytes,&value)) && value) owner.Attach(static_cast<Recording*>(value));
    if(!owner && create) {
        owner.Attach(new Recording());
        if(FAILED(list->SetPrivateDataInterface(StateGuid,owner.Get()))) owner.Reset();
    }
    return owner;
}
template<class F> inline void Track(ID3D12GraphicsCommandList* list,F&& update)
{
    if(!hooksReady || !applicationTracking || suppression || !list) return;
    auto owner=Get(list,true); if(!owner) return;
    std::lock_guard lock(owner->mutex); update(owner->state);
}
inline void Reset(ID3D12GraphicsCommandList* list,ID3D12PipelineState* initial)
{
    list->SetPrivateDataInterface(StateGuid,nullptr);
    auto owner=Get(list,true);
    if(owner) {
        owner->state.pipeline=initial;
        owner->state.graphics.known=owner->state.compute.known=owner->state.heapsKnown=true;
    }
}
using Observer=void(*)(ID3D12GraphicsCommandList*,const State*,bool);
inline Observer observer{};
class ProtectedDraw
{
    ID3D12GraphicsCommandList* list_;
    std::shared_ptr<State> before_;
    bool retained_{};
public:
    explicit ProtectedDraw(ID3D12GraphicsCommandList* list):list_(list)
    {
        auto owner=Get(list);
        if(owner) { std::lock_guard lock(owner->mutex); before_=std::make_shared<State>(owner->state); }
        if(observer) observer(list_,before_.get(),false);
        ++suppression;
    }
    bool Retain()
    {
        retained_=before_ && before_->graphics.known && before_->compute.known && before_->heapsKnown &&
            MenuGpuLifetime::Retain(list_,before_);
        return retained_;
    }
    ~ProtectedDraw()
    {
        if(retained_) { before_->Restore(list_); if(observer) observer(list_,before_.get(),true); }
        --suppression;
    }
};
HRESULT Install(ID3D12GraphicsCommandList* realList,ID3D12CommandQueue* realQueue=nullptr);
struct DeviceCoverage
{
    bool installed{};
    void* deviceVtable{};
    void* listVtable{};
    void* graphicsSignature{};
    void* graphicsTable{};
};
HRESULT InstallDevice(ID3D12Device* device,DeviceCoverage* coverage=nullptr);
HRESULT Uninstall();
}
