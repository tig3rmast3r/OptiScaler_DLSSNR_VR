#include "pch.h"
#include "MenuCommandState.h"
#include <detours/detours.h>
#include <algorithm>

namespace MenuCommandState
{
namespace {
constexpr unsigned MaxImplementations=8;
struct Hook { PVOID* original; PVOID replacement; PVOID target; };
std::vector<Hook> installed;
struct ModulePin
{
    HMODULE module{};
    explicit ModulePin(HMODULE value):module(value) {}
    ModulePin(ModulePin&& other) noexcept:module(std::exchange(other.module,nullptr)) {}
    ModulePin(const ModulePin&)=delete;
    ~ModulePin() { if(module) FreeLibrary(module); }
};
std::vector<ModulePin> runtimeModules;
std::mutex installMutex;
std::mutex deviceCoverageMutex;
unsigned coverageGeneration=1;
constexpr GUID DeviceCoverageGuid{0xa1652f3e,0x6d23,0x40bb,{0x97,0x14,0x71,0x8d,0x3a,0xc6,0xc0,0x65}};
constexpr GUID NativeObjectGuid{0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
template<class T> ComPtr<T> Native(T* value)
{
    ComPtr<T> native;
    if(FAILED(value->QueryInterface(NativeObjectGuid,reinterpret_cast<void**>(native.GetAddressOf())))) native=value;
    return native;
}
unsigned nextImplementation{};
#define COMMA ,
#define HOOK(Name, Params, Args, Body) \
    using Fn##Name=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, Params); \
    std::array<Fn##Name,MaxImplementations> original##Name{}; \
    template<unsigned Slot> void STDMETHODCALLTYPE Capture##Name(ID3D12GraphicsCommandList* list, Params) { \
        Track(list,[&](State& state) { Body; }); original##Name[Slot](list, Args); }
HOOK(Pipeline,ID3D12PipelineState* value,value,state.pipeline=value)
HOOK(Heaps,UINT count COMMA ID3D12DescriptorHeap* const* value,count COMMA value,state.Heaps(count,value))
HOOK(GraphicsSignature,ID3D12RootSignature* value,value,state.graphics.Signature(value))
HOOK(ComputeSignature,ID3D12RootSignature* value,value,state.compute.Signature(value))
#define ROOT_HOOKS(Prefix, Root) \
    HOOK(Prefix##Table,UINT index COMMA D3D12_GPU_DESCRIPTOR_HANDLE value,index COMMA value,state.Root.Table(index,value)) \
    HOOK(Prefix##Constant,UINT index COMMA UINT value COMMA UINT offset,index COMMA value COMMA offset,state.Root.Constants(index,1,&value,offset)) \
    HOOK(Prefix##Constants,UINT index COMMA UINT count COMMA const void* value COMMA UINT offset,index COMMA count COMMA value COMMA offset,state.Root.Constants(index,count,value,offset)) \
    HOOK(Prefix##Cbv,UINT index COMMA D3D12_GPU_VIRTUAL_ADDRESS value,index COMMA value,state.Root.Address(index,value,Kind::Cbv)) \
    HOOK(Prefix##Srv,UINT index COMMA D3D12_GPU_VIRTUAL_ADDRESS value,index COMMA value,state.Root.Address(index,value,Kind::Srv)) \
    HOOK(Prefix##Uav,UINT index COMMA D3D12_GPU_VIRTUAL_ADDRESS value,index COMMA value,state.Root.Address(index,value,Kind::Uav))
ROOT_HOOKS(Graphics,graphics)
ROOT_HOOKS(Compute,compute)
HOOK(Viewports,UINT count COMMA const D3D12_VIEWPORT* value,count COMMA value,
    if(count<=16 && (value || !count)) { state.viewports.clear(); if(count) state.viewports.assign(value,value+count); state.viewportKnown=true; })
HOOK(Scissors,UINT count COMMA const D3D12_RECT* value,count COMMA value,
    if(count<=16 && (value || !count)) { state.scissors.clear(); if(count) state.scissors.assign(value,value+count); state.scissorKnown=true; })
HOOK(Topology,D3D12_PRIMITIVE_TOPOLOGY value,value,state.topology=value; state.topologyKnown=true)
HOOK(Index,const D3D12_INDEX_BUFFER_VIEW* value,value,state.index=value?*value:D3D12_INDEX_BUFFER_VIEW{}; state.indexKnown=true)
HOOK(Vertices,UINT start COMMA UINT count COMMA const D3D12_VERTEX_BUFFER_VIEW* value,start COMMA count COMMA value,
    if(start<32 && count<=32-start) for(UINT i=0;i<count;++i) { state.vertices[start+i]=value?value[i]:D3D12_VERTEX_BUFFER_VIEW{}; state.verticesKnown.set(start+i); })
HOOK(Blend,const float* value,value,for(UINT i=0;i<4;++i) state.blend[i]=value?value[i]:1.0f; state.blendKnown=true)
HOOK(Targets,UINT count COMMA const D3D12_CPU_DESCRIPTOR_HANDLE* value COMMA BOOL range COMMA const D3D12_CPU_DESCRIPTOR_HANDLE* depth,count COMMA value COMMA range COMMA depth,
    if(count<=8 && (value || !count)) {
        state.targets.clear(); UINT increment=0;
        if(count && range) { ComPtr<ID3D12Device> device; if(SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device)))) increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV); }
        for(UINT i=0;i<count;++i) state.targets.push_back(range?D3D12_CPU_DESCRIPTOR_HANDLE{value[0].ptr+(SIZE_T)i*increment}:value[i]);
        state.depth=depth?*depth:D3D12_CPU_DESCRIPTOR_HANDLE{}; state.targetsKnown=true;
    })
#undef ROOT_HOOKS
#undef HOOK
#undef COMMA
}
namespace {
using CreateFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_COMMAND_LIST_TYPE,ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**);
std::array<CreateFn,MaxImplementations> originalCreate{};
template<unsigned Slot> HRESULT STDMETHODCALLTYPE CaptureCreate(ID3D12Device* device,UINT node,D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline,REFIID iid,void** out)
{
    const auto result=originalCreate[Slot](device,node,type,allocator,pipeline,iid,out);
    if(SUCCEEDED(result) && out && *out) {
        ComPtr<ID3D12GraphicsCommandList> list;
        if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list)))) Reset(list.Get(),pipeline);
    }
    return result;
}
using ResetFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
std::array<ResetFn,MaxImplementations> originalReset{};
template<unsigned Slot> HRESULT STDMETHODCALLTYPE CaptureReset(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline)
{
    const auto result=originalReset[Slot](list,allocator,pipeline);
    if(SUCCEEDED(result)) { MenuGpuLifetime::ForgetRecording(list); Reset(list,pipeline); }
    return result;
}
using ExecuteFn=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
std::array<ExecuteFn,MaxImplementations> originalExecute{};
template<unsigned Slot> void STDMETHODCALLTYPE CaptureExecute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists)
{
    auto draws=MenuGpuLifetime::BeforeSubmit(count,lists);
    originalExecute[Slot](queue,count,lists);
    MenuGpuLifetime::AfterSubmit(queue,std::move(draws));
}
template<unsigned Slot> HRESULT Gather(ID3D12GraphicsCommandList* list,ID3D12CommandQueue* queue,std::vector<Hook>& hooks)
{
    auto** vtable=*reinterpret_cast<void***>(list);
#define ADD(Name, Index) original##Name[Slot]=reinterpret_cast<Fn##Name>(vtable[Index]); hooks.push_back({reinterpret_cast<PVOID*>(&original##Name[Slot]),reinterpret_cast<PVOID>(Capture##Name<Slot>),vtable[Index]})
    ADD(Topology,20); ADD(Viewports,21); ADD(Scissors,22); ADD(Blend,23); ADD(Pipeline,25);
    ADD(Heaps,28); ADD(ComputeSignature,29); ADD(GraphicsSignature,30);
    ADD(ComputeTable,31); ADD(GraphicsTable,32); ADD(ComputeConstant,33); ADD(GraphicsConstant,34);
    ADD(ComputeConstants,35); ADD(GraphicsConstants,36); ADD(ComputeCbv,37); ADD(GraphicsCbv,38);
    ADD(ComputeSrv,39); ADD(GraphicsSrv,40); ADD(ComputeUav,41); ADD(GraphicsUav,42);
    ADD(Index,43); ADD(Vertices,44); ADD(Targets,46);
#undef ADD
    ComPtr<ID3D12Device> device;
    if(FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return E_FAIL;
    constexpr GUID nativeGuid{0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
    ComPtr<ID3D12Device> native;
    if(SUCCEEDED(device->QueryInterface(nativeGuid,reinterpret_cast<void**>(native.GetAddressOf())))) device=native;
    auto* create=(*reinterpret_cast<void***>(device.Get()))[12];
    originalCreate[Slot]=reinterpret_cast<CreateFn>(create);
    hooks.push_back({reinterpret_cast<PVOID*>(&originalCreate[Slot]),reinterpret_cast<PVOID>(CaptureCreate<Slot>),create});
    originalReset[Slot]=reinterpret_cast<ResetFn>(vtable[10]);
    hooks.push_back({reinterpret_cast<PVOID*>(&originalReset[Slot]),reinterpret_cast<PVOID>(CaptureReset<Slot>),vtable[10]});
    if(queue) {
        auto* execute=(*reinterpret_cast<void***>(queue))[10];
        originalExecute[Slot]=reinterpret_cast<ExecuteFn>(execute);
        hooks.push_back({reinterpret_cast<PVOID*>(&originalExecute[Slot]),reinterpret_cast<PVOID>(CaptureExecute<Slot>),execute});
    }
    return S_OK;
}
}
HRESULT Install(ID3D12GraphicsCommandList* list,ID3D12CommandQueue* queue)
{
    std::lock_guard lock(installMutex);
    if(!list) return E_POINTER;
    if(nextImplementation>=MaxImplementations) return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    using GatherFn=HRESULT(*)(ID3D12GraphicsCommandList*,ID3D12CommandQueue*,std::vector<Hook>&);
    constexpr GatherFn gather[]={Gather<0>,Gather<1>,Gather<2>,Gather<3>,Gather<4>,Gather<5>,Gather<6>,Gather<7>};
    std::vector<Hook> hooks;
    const auto gathered=gather[nextImplementation](list,queue,hooks);
    if(FAILED(gathered)) return gathered;
    // A later device may share some entry points, or belong to another Agility
    // runtime. Reuse already-hooked targets; never mistake global readiness for
    // coverage of the new implementation or attach twice to the same address.
    std::erase_if(hooks,[&](const Hook& hook) {
        return std::any_of(installed.begin(),installed.end(),[&](const Hook& old) { return old.target==hook.target; });
    });
    if(hooks.empty()) return S_OK;
    // A short-lived probe device must not unload code whose entry points still
    // contain our detours. Keep runtime modules alive until successful detach.
    std::vector<ModulePin> pins;
    for(const auto& hook:hooks) {
        HMODULE module{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(hook.target),&module))
            return HRESULT_FROM_WIN32(GetLastError());
        const auto owns=[&](const ModulePin& pin) { return pin.module==module; };
        if(std::any_of(runtimeModules.begin(),runtimeModules.end(),owns) || std::any_of(pins.begin(),pins.end(),owns)) FreeLibrary(module);
        else pins.emplace_back(module);
    }
    LONG result=DetourTransactionBegin();
    if(result!=NO_ERROR) return HRESULT_FROM_WIN32(result);
    result=DetourUpdateThread(GetCurrentThread());
    for(auto& hook:hooks) if(result==NO_ERROR) result=DetourAttach(hook.original,hook.replacement);
    if(result!=NO_ERROR) { DetourTransactionAbort(); return HRESULT_FROM_WIN32(result); }
    result=DetourTransactionCommit();
    if(result!=NO_ERROR) return HRESULT_FROM_WIN32(result);
    installed.insert(installed.end(),hooks.begin(),hooks.end()); ++nextImplementation;
    for(auto& pin:pins) runtimeModules.push_back(std::move(pin));
    hooksReady=true; MenuGpuLifetime::resetHookReady=true;
    if(queue) MenuGpuLifetime::queueHookReady=true;
    return S_OK;
}
HRESULT InstallDevice(ID3D12Device* device,DeviceCoverage* coverage)
{
    if(coverage) *coverage={};
    if(!device) return E_POINTER;
    std::lock_guard lock(deviceCoverageMutex);
    auto nativeDevice=Native(device);
    unsigned receipt{}; UINT bytes=sizeof(receipt);
    if(hooksReady && SUCCEEDED(nativeDevice->GetPrivateData(DeviceCoverageGuid,&bytes,&receipt)) &&
        bytes==sizeof(receipt) && receipt==coverageGeneration) return S_OK;
    // Devices sharing CDevice's vtable can produce different Variadic_Thk
    // command lists. A COM-owned receipt covers only this device's probe.
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    auto result=nativeDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));
    if(FAILED(result)) return result;
    result=nativeDevice->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list));
    if(FAILED(result)) return result;
    result=nativeDevice->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue));
    if(FAILED(result)) { list->Close(); return result; }
    auto nativeList=Native(list.Get()); auto nativeQueue=Native(queue.Get());
    auto** table=*reinterpret_cast<void***>(nativeList.Get());
    DeviceCoverage evidence{true,*reinterpret_cast<void**>(nativeDevice.Get()),table,table[30],table[32]};
    result=Install(nativeList.Get(),nativeQueue.Get());
    const auto closed=list->Close();
    if(FAILED(result)) return result;
    if(FAILED(closed)) return closed;
    result=nativeDevice->SetPrivateData(DeviceCoverageGuid,sizeof(coverageGeneration),&coverageGeneration);
    if(SUCCEEDED(result) && coverage) *coverage=evidence;
    return result;
}
HRESULT Uninstall()
{
    std::lock_guard coverageLock(deviceCoverageMutex);
    std::lock_guard lock(installMutex);
    if(installed.empty()) return S_OK;
    LONG result=DetourTransactionBegin();
    if(result!=NO_ERROR) return HRESULT_FROM_WIN32(result);
    result=DetourUpdateThread(GetCurrentThread());
    for(auto& hook:installed) if(result==NO_ERROR) result=DetourDetach(hook.original,hook.replacement);
    if(result!=NO_ERROR) { DetourTransactionAbort(); return HRESULT_FROM_WIN32(result); }
    result=DetourTransactionCommit();
    if(result!=NO_ERROR) return HRESULT_FROM_WIN32(result);
    hooksReady=false; MenuGpuLifetime::resetHookReady=MenuGpuLifetime::queueHookReady=false;
    installed.clear(); nextImplementation=0; ++coverageGeneration; runtimeModules.clear(); return S_OK;
}
}
