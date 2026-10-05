#include "xrfg/luke_depth_format.hpp"
#include "xrfg/luke_recording_scope.hpp"
#include "luke_depth_convert_shader.hpp"
#include "luke_compose_motion_shader.hpp"
#include "xrfg/luke_motion_format.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
void check(HRESULT h) { if (FAILED(h)) throw std::runtime_error("HRESULT " + std::to_string(h)); }
void expect(bool b) { if (!b) throw std::runtime_error("depth contract failed"); }
D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; return x;
}
int main() { try {
    ComPtr<ID3D12Debug> debug; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device;check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; device.As(&info);
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));check(list->Close());
    int aborted=0;
    for(int i=0;i<2;++i) { check(allocator->Reset()); check(list->Reset(allocator.Get(),nullptr));
        xrfg::LukeRecordingScope scope(list.Get(),[&](HRESULT h) noexcept { if(SUCCEEDED(h)) ++aborted; });
    }
    expect(aborted==2); // Repeated pre-submit failure cannot leave the list open.
    ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);expect(event!=nullptr);UINT64 value=0;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    auto texture=[&](UINT w,UINT h,DXGI_FORMAT f,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=d.MipLevels=1;d.Format=f;d.SampleDesc.Count=1;d.Flags=flags;
        ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    std::array<D3D12_DESCRIPTOR_RANGE,2> ranges{};
    for(UINT i=0;i<2;++i) { ranges[i].RangeType=i?D3D12_DESCRIPTOR_RANGE_TYPE_UAV:D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[i].NumDescriptors=1; }
    std::array<D3D12_ROOT_PARAMETER,3> params{};
    for(UINT i=0;i<2;++i) { params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]}; }
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants.Num32BitValues=4;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=params.data();
    ComPtr<ID3DBlob> blob,errors;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));
    ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={g_xrfg_luke_depth_convert_shader,sizeof(g_xrfg_luke_depth_convert_shader)};
    ComPtr<ID3D12PipelineState> pipeline;check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
    for(auto format:{DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_D32_FLOAT_S8X24_UINT,DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_D16_UNORM}) {
      const auto typed=xrfg::luke_depth_format(format);expect(typed.resource!=DXGI_FORMAT_UNKNOWN);
      for(UINT dilate=0;dilate<3;++dilate) {
        auto source=texture(8,4,format,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto staging=texture(8,4,typed.resource,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        auto output=texture(3,2,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
        ComPtr<ID3D12DescriptorHeap> dsv;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv)));
        device->CreateDepthStencilView(source.Get(),nullptr,dsv->GetCPUDescriptorHandleForHeapStart());
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> views;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)));
        auto cpu=views->GetCPUDescriptorHandleForHeapStart();const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=typed.srv;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(staging.Get(),&srv,cpu);cpu.ptr+=stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);
        const auto od=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;device->GetCopyableFootprints(&od,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto readheap=heap;readheap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;check(device->CreateCommittedResource(&readheap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        check(allocator->Reset());check(list->Reset(allocator.Get(),nullptr));
        list->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,0.25F,0,0,nullptr);
        D3D12_RECT rect{3,1,6,3};list->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,0.75F,0,1,&rect);
        auto b=barrier(source.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=source.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=staging.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        b=barrier(staging.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
        ID3D12DescriptorHeap* heaps[]{views.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        auto gpu=views->GetGPUDescriptorHandleForHeapStart();list->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=stride;list->SetComputeRootDescriptorTable(1,gpu);
        UINT constants[]{1,dilate,3,1};list->SetComputeRoot32BitConstants(2,4,constants,0);list->Dispatch(1,1,1);
        b=barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
        from.pResource=output.Get();to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        check(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),++value));check(fence->SetEventOnCompletion(value,event));expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
        void* mapped=nullptr;check(readback->Map(0,nullptr,&mapped));
        for(UINT y=0;y<2;++y)for(UINT x=0;x<3;++x) { float v;std::memcpy(&v,static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*4,4);expect(std::abs(v-0.75F)<0.0001F); }
        readback->Unmap(0,nullptr);
      }
    }
    // Execute the unchanged production composition shader with BOTH float2
    // input precisions, cropped coordinates and a nonzero temporal camera step.
    const auto half = [](float v) -> std::uint16_t {
        std::uint32_t bits{}; std::memcpy(&bits,&v,4);
        if((bits & 0x7fffffff)==0) return static_cast<std::uint16_t>(bits>>16);
        return static_cast<std::uint16_t>(((bits>>16)&0x8000) |
            (((bits>>23)-127+15)<<10) | ((bits>>13)&0x3ff));
    };
    const auto unhalf = [](std::uint16_t v) {
        const unsigned e=(v>>10)&31, m=v&1023;
        const float result=e?std::ldexp(1.0F+m/1024.0F,static_cast<int>(e)-15):std::ldexp(m/1024.0F,-14);
        return (v&0x8000)?-result:result;
    };
    std::array<D3D12_DESCRIPTOR_RANGE,3> motion_ranges{};
    std::array<D3D12_ROOT_PARAMETER,4> motion_params{};
    for(UINT i=0;i<3;++i) {
        motion_ranges[i].RangeType=i==2?D3D12_DESCRIPTOR_RANGE_TYPE_UAV:D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        motion_ranges[i].NumDescriptors=1;motion_ranges[i].BaseShaderRegister=i==1?1:0;
        motion_params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        motion_params[i].DescriptorTable={1,&motion_ranges[i]};
    }
    motion_params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    motion_params[3].Constants.Num32BitValues=56;
    D3D12_ROOT_SIGNATURE_DESC motion_rd{};motion_rd.NumParameters=4;motion_rd.pParameters=motion_params.data();
    ComPtr<ID3DBlob> motion_blob;check(D3D12SerializeRootSignature(&motion_rd,D3D_ROOT_SIGNATURE_VERSION_1,&motion_blob,&errors));
    ComPtr<ID3D12RootSignature> motion_root;check(device->CreateRootSignature(0,motion_blob->GetBufferPointer(),motion_blob->GetBufferSize(),IID_PPV_ARGS(&motion_root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC motion_pd{};motion_pd.pRootSignature=motion_root.Get();
    motion_pd.CS={g_xrfg_luke_compose_motion_shader,sizeof(g_xrfg_luke_compose_motion_shader)};
    ComPtr<ID3D12PipelineState> motion_pipeline;check(device->CreateComputePipelineState(&motion_pd,IID_PPV_ARGS(&motion_pipeline)));
    for(auto format:{DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R32G32_FLOAT}) {
      expect(xrfg::luke_motion_srv_format(format)==format);
      for(float camera_step:{0.0F,0.25F}) {
        auto input=texture(6,4,format,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        auto motion_depth=texture(2,2,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        auto composed=texture(2,2,xrfg::kLukeAfwMotionFormat,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto afw_motion=texture(2,2,xrfg::kLukeAfwMotionFormat,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        check(allocator->Reset());check(list->Reset(allocator.Get(),nullptr));
        std::array<ComPtr<ID3D12Resource>,2> uploads;
        for(UINT slot=0;slot<2;++slot) {
          ID3D12Resource* target=slot?motion_depth.Get():input.Get();const auto desc=target->GetDesc();
          D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
          D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;
          bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
          auto uh=heap;uh.Type=D3D12_HEAP_TYPE_UPLOAD;
          check(device->CreateCommittedResource(&uh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&uploads[slot])));
          void* data{};check(uploads[slot]->Map(0,nullptr,&data));std::memset(data,0,static_cast<size_t>(bytes));
          for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
            auto* row=static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch;
            if(slot) { const float depth_value=0.5F;std::memcpy(row+x*4,&depth_value,4); }
            else if(format==DXGI_FORMAT_R32G32_FLOAT) { const float values[]{x*0.25F,-static_cast<float>(y)*0.5F};std::memcpy(row+x*8,values,8); }
            else { const std::uint16_t values[]{half(x*0.25F),half(-static_cast<float>(y)*0.5F)};std::memcpy(row+x*4,values,4); }
          }
          uploads[slot]->Unmap(0,nullptr);
          D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=uploads[slot].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
          to.pResource=target;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
          auto b=barrier(target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> views;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)));
        const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);auto cpu=views->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=xrfg::luke_motion_srv_format(format);srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(input.Get(),&srv,cpu);cpu.ptr+=stride;srv.Format=DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(motion_depth.Get(),&srv,cpu);cpu.ptr+=stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=xrfg::kLukeAfwMotionFormat;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(composed.Get(),nullptr,&uav,cpu);
        ID3D12DescriptorHeap* heaps[]{views.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(motion_root.Get());list->SetPipelineState(motion_pipeline.Get());
        auto gpu=views->GetGPUDescriptorHandleForHeapStart();for(UINT i=0;i<3;++i){list->SetComputeRootDescriptorTable(i,gpu);gpu.ptr+=stride;}
        std::array<std::uint32_t,56> constants{};constants[0]=2;constants[1]=1;constants[2]=1;
        const auto setfloat=[&](UINT index,float v){std::memcpy(&constants[index],&v,4);};
        setfloat(4,-1);setfloat(5,-1);setfloat(6,2);setfloat(7,2);
        for(UINT base:{8U,24U,40U})for(UINT i=0;i<4;++i)setfloat(base+i*5,1);
        setfloat(52,camera_step);list->SetComputeRoot32BitConstants(3,56,constants.data(),0);list->Dispatch(1,1,1);
        auto b=barrier(composed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
        list->CopyResource(afw_motion.Get(),composed.Get());b=barrier(afw_motion.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
        const auto desc=afw_motion->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto rh=heap;rh.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;check(device->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=afw_motion.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        check(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),++value));check(fence->SetEventOnCompletion(value,event));expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
        void* data{};check(readback->Map(0,nullptr,&data));
        for(UINT y=0;y<2;++y)for(UINT x=0;x<2;++x) {
          std::uint16_t values[2];std::memcpy(values,static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch+x*4,4);
          expect(std::abs(unhalf(values[0])-((x+3)*0.25F+camera_step))<0.001F);
          expect(std::abs(unhalf(values[1])+((y+1)*0.5F))<0.001F);
        }
        readback->Unmap(0,nullptr);
      }
    }
    for(auto format:{DXGI_FORMAT_R16G16_TYPELESS,DXGI_FORMAT_R32G32_TYPELESS,DXGI_FORMAT_R16G16_UNORM,DXGI_FORMAT_R16G16_SINT,DXGI_FORMAT_R32_FLOAT})
      expect(xrfg::luke_motion_srv_format(format)==DXGI_FORMAT_UNKNOWN);
    expect(xrfg::luke_depth_format(DXGI_FORMAT_R8G8B8A8_UNORM).resource==DXGI_FORMAT_UNKNOWN);
    if(info) for(UINT64 i=0;i<info->GetNumStoredMessages();++i) { SIZE_T n=0;check(info->GetMessage(i,nullptr,&n));auto data=new char[n];auto msg=reinterpret_cast<D3D12_MESSAGE*>(data);check(info->GetMessage(i,msg,&n));bool bad=msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR; if(bad) std::cerr<<msg->pDescription<<'\n';delete[] data;expect(!bad); }
    CloseHandle(event);std::cout<<"Depth families and typed float16/float32 motion composition/crop/camera/copy passed on WARP\n";return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
