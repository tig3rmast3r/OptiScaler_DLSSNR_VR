#define NOMINMAX
#include "../OptiScaler/hooks/LukeNativeSceneCamera.h"
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "fixtures/kcd_camera_packets.hpp"
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void check(HRESULT value,const char* message){require(SUCCEEDED(value),message);}
int main(){try{
    std::array<unsigned char,kcd_camera::packet_bytes> first{},second{};kcd_camera::Camera expected{},other_camera{};
    first=kcd_fixture::packet(0);
    require(kcd_camera::decode(first.data(),first.size(),1280,720,expected),"fixture camera");
    bool changed=false;
    for(unsigned index=1;index<kcd_fixture::count;++index) {
        second=kcd_fixture::packet(index);
        if(kcd_camera::decode(second.data(),second.size(),1280,720,other_camera)&&!kcd_camera::equivalent(expected,other_camera)){changed=true;break;}
    }
    require(changed,"fixture has no changing camera");
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;ComPtr<ID3D12Device> device;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP");
    check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=2048;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> source;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&source)),"upload");
    void* cpu=nullptr;D3D12_RANGE empty{};check(source->Map(0,&empty,&cpu),"pre-hook persistent Map");
    std::memcpy(cpu,first.data(),first.size());std::memcpy(static_cast<unsigned char*>(cpu)+1024,second.data(),second.size());
    const auto unknown=source->GetGPUVirtualAddress();
    require(luke_scene_camera::install(device.Get())&&luke_scene_camera::install(device.Get()),"install");
    ComPtr<ID3D12CommandAllocator> allocator,other_allocator;ComPtr<ID3D12GraphicsCommandList> list,other;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&other_allocator)),"other allocator");
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,other_allocator.Get(),nullptr,IID_PPV_ARGS(&other)),"other list");
    D3D12_ROOT_PARAMETER params[3]{};for(unsigned i=0;i<3;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[i].Descriptor.ShaderRegister=i;}
    D3D12_ROOT_SIGNATURE_DESC sd{};sd.NumParameters=3;sd.pParameters=params;
    ComPtr<ID3DBlob> serialized,errors;ComPtr<ID3D12RootSignature> signature,different;
    check(D3D12SerializeRootSignature(&sd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"serialize");
    check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&signature)),"root signature");
    params[0].Descriptor.RegisterSpace=1;check(D3D12SerializeRootSignature(&sd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"other serialize");
    check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&different)),"other signature");
    list->SetGraphicsRootSignature(signature.Get());list->SetGraphicsRootConstantBufferView(1,unknown);
    require(!luke_scene_camera::take(list.Get(),1280,720).valid,"unknown range guessed");
    const auto gpu=source->GetGPUVirtualAddress();list->SetGraphicsRootConstantBufferView(1,gpu);
    require(!luke_scene_camera::take(other.Get(),1280,720).valid,"foreign list borrowed camera");
    // Modify mapped bytes after binding: read at production, not at old binding.
    std::memcpy(cpu,second.data(),second.size());
    auto saved=luke_scene_camera::take(list.Get(),1280,720);
    require(saved.valid&&saved.reads==1&&saved.candidates==1&&saved.graphics&&saved.slot==1,"current camera missing");
    require(kcd_camera::equivalent(saved.camera,other_camera)&&!kcd_camera::equivalent(saved.camera,expected),"binding-time camera reused");
    require(!luke_scene_camera::take(list.Get(),1280,720).valid,"preceding production reused");
    std::memcpy(cpu,first.data(),first.size());require(kcd_camera::equivalent(saved.camera,other_camera),"borrowed bytes escaped");
    list->SetGraphicsRootConstantBufferView(1,gpu);list->SetGraphicsRootSignature(different.Get());
    require(!luke_scene_camera::take(list.Get(),1280,720).valid,"signature change retained stale camera");
    list->SetGraphicsRootSignature(signature.Get());list->SetGraphicsRootConstantBufferView(1,gpu);list->SetGraphicsRootConstantBufferView(2,gpu+1024);
    const auto ambiguous=luke_scene_camera::take(list.Get(),1280,720);
    require(!ambiguous.valid&&ambiguous.candidates==2&&(ambiguous.failure&128),"ambiguous camera selected");
    list->SetGraphicsRootConstantBufferView(1,gpu+1792);
    require(!luke_scene_camera::take(list.Get(),1280,720).valid,"short range guessed");
    auto default_heap=heap;default_heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> resident;
    check(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&resident)),"DEFAULT");
    list->SetGraphicsRootConstantBufferView(1,resident->GetGPUVirtualAddress());const auto no_cpu=luke_scene_camera::take(list.Get(),1280,720);
    require(!no_cpu.valid&&(no_cpu.failure&16)&&FAILED(no_cpu.error),"non-CPU buffer synthesized camera");
    require(device->GetDeviceRemovedReason()==S_OK,"Map failure removed device");
    // A real dispatch still consumes the original root address/data after take.
    D3D12_ROOT_PARAMETER compute_params[2]{};compute_params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    compute_params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;sd.NumParameters=2;sd.pParameters=compute_params;
    check(D3D12SerializeRootSignature(&sd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"compute serialize");
    ComPtr<ID3D12RootSignature> compute_signature;check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&compute_signature)),"compute root");
    const char shader[]="cbuffer C:register(b0){uint4 c;} RWStructuredBuffer<uint> o:register(u0);[numthreads(1,1,1)]void main(){o[0]=c.x;}";
    ComPtr<ID3DBlob> compiled;check(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&compiled,&errors),"shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=compute_signature.Get();pd.CS={compiled->GetBufferPointer(),compiled->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"pipeline");
    desc.Width=256;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;ComPtr<ID3D12Resource> output,readback;
    check(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)),"output");
    auto readback_heap=heap;readback_heap.Type=D3D12_HEAP_TYPE_READBACK;desc.Flags=D3D12_RESOURCE_FLAG_NONE;
    check(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
    list->SetPipelineState(pipeline.Get());list->SetComputeRootSignature(compute_signature.Get());
    list->SetComputeRootUnorderedAccessView(0,output->GetGPUVirtualAddress());list->SetComputeRootConstantBufferView(1,gpu);
    require(luke_scene_camera::take(list.Get(),1280,720).valid,"compute scene camera");list->Dispatch(1,1,1);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=output.Get();
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1,&barrier);list->CopyBufferRegion(readback.Get(),0,output.Get(),0,4);check(list->Close(),"Close");
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    ID3D12CommandList* submitted[]{list.Get()};queue->ExecuteCommandLists(1,submitted);check(queue->Signal(fence.Get(),1),"signal");
    HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(done!=nullptr,"event");check(fence->SetEventOnCompletion(1,done),"notify");
    const auto waited=WaitForSingleObject(done,10000);CloseHandle(done);require(waited==WAIT_OBJECT_0,"GPU timeout");
    void* actual=nullptr;D3D12_RANGE read_range{0,4};check(readback->Map(0,&read_range,&actual),"readback Map");
    std::uint32_t wanted{};std::memcpy(&wanted,first.data(),4);require(*static_cast<std::uint32_t*>(actual)==wanted,"camera capture changed GPU constants");readback->Unmap(0,&empty);
    check(allocator->Reset(),"allocator Reset");check(list->Reset(allocator.Get(),nullptr),"Reset");
    require(!luke_scene_camera::take(list.Get(),1280,720).valid,"Reset retained camera");
    list->SetGraphicsRootSignature(signature.Get());list->SetGraphicsRootConstantBufferView(1,gpu);source->Unmap(0,&empty);source.Reset();
    require(!luke_scene_camera::take(list.Get(),1280,720).valid,"released resource retained camera");
    check(list->Close(),"final Close");check(other->Close(),"other Close");auto* list_id=list.Get();list.Reset();
    require(!luke_scene_camera::detail::roots.contains(list_id),"released list retained context");
    require(luke_scene_camera::uninstall(),"detach");require(!luke_scene_camera::take(nullptr,1280,720).valid,"inactive camera admitted");
    require(luke_scene_camera::install(device.Get())&&luke_scene_camera::uninstall(),"reinstall");
    std::cout<<"Current scene camera: real GPU consumer, production-time bytes, exact-list/once/signature/ambiguity/bounds/Map/Reset/Release/detach PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
