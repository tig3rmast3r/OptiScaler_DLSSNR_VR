#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#include <menu/InUpscalerDx12Renderer.h>
#include <menu/MenuCommandState.h>

using Microsoft::WRL::ComPtr;
static unsigned checks;
static void Check(bool ok, const char* what)
{
    ++checks;
    if (!ok) throw std::runtime_error(what);
}
static void Hr(HRESULT result) { Check(SUCCEEDED(result), "D3D12 operation failed"); }
static void Wait(ID3D12Fence* fence, UINT64 value)
{
    const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "fence event");
    Hr(fence->SetEventOnCompletion(value, event));
    const auto result = WaitForSingleObject(event, 30000);
    CloseHandle(event);
    Check(result == WAIT_OBJECT_0, "GPU completion timed out");
}
static ComPtr<ID3D12Resource> Output(ID3D12Device* device, unsigned size, bool direct, bool typeless)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = size; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (direct) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> result;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&result)));
    return result;
}
static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                       D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &barrier);
}
struct Commands
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    explicit Commands(ID3D12Device* device)
    {
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)));
    }
};
static void Execute(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list)
{
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
}
static void Frame(unsigned size, unsigned index)
{
    ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(size), static_cast<float>(size));
    ImGui::GetIO().DeltaTime = 1.0f / 90.0f;
    ImGui::NewFrame();
    const auto color = index % 2 ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 0, 0, 255);
    auto* draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(ImVec2(20, 20), ImVec2(100, 100), color);
    draw->AddText(ImVec2(10, 120), IM_COL32_WHITE, index < 4 ? "Menu" : "Font growth 0123456789 ABCDEFG abcdefg");
    // Force mesh growth while earlier eye submissions are still queued.
    for (unsigned i = 0; i < (index + 1) * 500; ++i)
        draw->AddRectFilled(ImVec2(150, 150), ImVec2(155, 155), color);
    ImGui::Render();
}
struct GameBindings
{
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline, srPipeline;
    ComPtr<ID3D12RootSignature> srRoot;
    ComPtr<ID3D12DescriptorHeap> srHeap;
    ComPtr<ID3D12DescriptorHeap> srv, sampler, rtv;
    ComPtr<ID3D12Resource> texture, target;
    explicit GameBindings(ID3D12Device* device)
    {
        target=Output(device,256,true,false); texture=Output(device,8,true,false);
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        D3D12_DESCRIPTOR_RANGE samplerRange{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,1,0,0,0};
        D3D12_ROOT_PARAMETER params[6]{};
        for(UINT i=0;i<5;++i) { params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[i].Constants={i==3?0:i+1,0,i==3?4u:1u}; params[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL; }
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5].DescriptorTable={1,&range}; params[5].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable={1,&samplerRange};
        D3D12_ROOT_SIGNATURE_DESC desc{6,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> blob,vs,ps;
        Hr(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr));
        Hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
        constexpr char shader[]=R"(
cbuffer Color:register(b0) { float4 color; };
Texture2D<float4> image:register(t0);
SamplerState imageSampler:register(s0);
float4 VS(uint id:SV_VertexID):SV_POSITION { float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)}; return float4(p[id],0,1); }
float4 PS():SV_Target { return color*image.SampleLevel(imageSampler,float2(0.5,0.5),0); }
)";
        Hr(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,nullptr));
        Hr(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps,nullptr));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature=root.Get();
        p.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        p.SampleMask=UINT_MAX; p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets=1; p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; p.SampleDesc.Count=1;
        auto& b=p.BlendState.RenderTarget[0]; b.BlendEnable=TRUE;
        b.SrcBlend=D3D12_BLEND_BLEND_FACTOR; b.DestBlend=D3D12_BLEND_ZERO; b.BlendOp=D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha=D3D12_BLEND_ONE; b.DestBlendAlpha=D3D12_BLEND_ZERO; b.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        b.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable=TRUE; p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        p.DepthStencilState.FrontFace.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        p.DepthStencilState.FrontFace.StencilFailOp=p.DepthStencilState.FrontFace.StencilDepthFailOp=
            p.DepthStencilState.FrontFace.StencilPassOp=D3D12_STENCIL_OP_KEEP;
        p.DepthStencilState.BackFace=p.DepthStencilState.FrontFace;
        Hr(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline)));
        D3D12_ROOT_SIGNATURE_DESC srDesc{};
        ComPtr<ID3DBlob> srBlob,cs;
        Hr(D3D12SerializeRootSignature(&srDesc,D3D_ROOT_SIGNATURE_VERSION_1,&srBlob,nullptr));
        Hr(device->CreateRootSignature(0,srBlob->GetBufferPointer(),srBlob->GetBufferSize(),IID_PPV_ARGS(&srRoot)));
        constexpr char compute[]="[numthreads(1,1,1)] void CS() {}";
        Hr(D3DCompile(compute,sizeof(compute),nullptr,nullptr,nullptr,"CS","cs_5_0",0,0,&cs,nullptr));
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{}; c.pRootSignature=srRoot.Get(); c.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
        Hr(device->CreateComputePipelineState(&c,IID_PPV_ARGS(&srPipeline)));
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.NumDescriptors=1;
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Hr(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&srv)));
        Hr(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&srHeap)));
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        Hr(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&sampler)));
        D3D12_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplerDesc.MaxAnisotropy=1; samplerDesc.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        samplerDesc.MaxLOD=D3D12_FLOAT32_MAX;
        device->CreateSampler(&samplerDesc,sampler->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels=1; device->CreateShaderResourceView(texture.Get(),&view,srv->GetCPUDescriptorHandleForHeapStart());
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors=2; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Hr(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtv)));
        auto cpu=rtv->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(target.Get(),nullptr,cpu);
        cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(texture.Get(),nullptr,cpu);
    }
    void Bind(ID3D12Device* device,ID3D12GraphicsCommandList* list)
    {
        auto cpu=rtv->GetCPUDescriptorHandleForHeapStart(); auto textureView=cpu;
        textureView.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        Transition(list,texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float white[4]={1,1,1,1}; list->ClearRenderTargetView(textureView,white,0,nullptr);
        Transition(list,texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(list,target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float black[4]={0,0,0,0}; list->ClearRenderTargetView(cpu,black,0,nullptr);
        list->SetPipelineState(pipeline.Get()); list->SetGraphicsRootSignature(root.Get());
        // Distinct compute bindings exercise heap invalidation in both pipelines.
        list->SetComputeRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]={srv.Get(),sampler.Get()}; list->SetDescriptorHeaps(2,heaps);
        const float rgb[3]={0,0,1},alpha=1;
        list->SetGraphicsRoot32BitConstants(3,3,rgb,0); list->SetGraphicsRoot32BitConstant(3,*reinterpret_cast<const UINT*>(&alpha),3);
        list->SetGraphicsRootDescriptorTable(5,srv->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootDescriptorTable(5,srv->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRootDescriptorTable(4,sampler->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootDescriptorTable(4,sampler->GetGPUDescriptorHandleForHeapStart());
        list->OMSetRenderTargets(1,&cpu,FALSE,nullptr);
        D3D12_VIEWPORT viewport{0,0,64,64,0,1}; D3D12_RECT scissor{16,16,64,64};
        list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const float blend[4]={0.25f,0.5f,0.75f,1}; list->OMSetBlendFactor(blend);
    }
    void FakeSr(ID3D12GraphicsCommandList* list)
    {
        MenuCommandState::applicationTracking=false;
        list->SetComputeRootSignature(srRoot.Get()); list->SetPipelineState(srPipeline.Get());
        ID3D12DescriptorHeap* heaps[]={srHeap.Get()}; list->SetDescriptorHeaps(1,heaps);
    }
    void DrawAfterMenu(ID3D12GraphicsCommandList* list,bool validate=true)
    {
        MenuCommandState::applicationTracking=true;
        list->SetComputeRootDescriptorTable(5,srv->GetGPUDescriptorHandleForHeapStart());
        // Like Unreal's SetSRVs: its cache assumes the original signature/heap is still bound.
        list->SetGraphicsRootDescriptorTable(5,srv->GetGPUDescriptorHandleForHeapStart());
        // Unreal ApplySamplers also updates tables without rebinding its cached root/heap.
        list->SetComputeRootDescriptorTable(4,sampler->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRootDescriptorTable(4,sampler->GetGPUDescriptorHandleForHeapStart());
        if(validate) {
        ComPtr<ID3D12Device> device; ComPtr<ID3D12InfoQueue> info;
        Hr(list->GetDevice(IID_PPV_ARGS(&device))); Hr(device.As(&info));
        unsigned errors=0;
        for(UINT64 n=0;n<info->GetNumStoredMessages();++n) {
            SIZE_T bytes{}; info->GetMessage(n,nullptr,&bytes); std::vector<unsigned char> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); info->GetMessage(n,message,&bytes);
            if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { ++errors; std::fprintf(stderr,"%s\n",message->pDescription); }
        }
        Check(errors==0,"cached game SetSRVs/ApplySamplers after menu uses wrong root/heap");
        }
        list->DrawInstanced(3,1,0,0);
        Transition(list,target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list,texture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
};
static void CheckPixel(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12Resource* texture,bool game)
{
    Commands copy(device); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes{};
    const auto desc=texture->GetDesc(); device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=bytes;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    Transition(copy.list.Get(),texture,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=texture; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint;
    copy.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Transition(copy.list.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Hr(copy.list->Close()); Execute(queue,copy.list.Get());
    ComPtr<ID3D12Fence> done; Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
    Hr(queue->Signal(done.Get(),1)); Wait(done.Get(),1);
    void* mapped{}; Hr(readback->Map(0,nullptr,&mapped));
    const auto* pixel=static_cast<unsigned char*>(mapped)+footprint.Offset+40*footprint.Footprint.RowPitch+40*4;
    Check(game?pixel[2]>180 && pixel[2]<200 && pixel[0]<10 && pixel[1]<10:
        pixel[0]>240 && pixel[1]<10,"actual Agility SDK cached sampler/game or menu pixel");
    readback->Unmap(0,nullptr);
}
static void AgilityAfterProbe(IDXGIFactory4* dxgi,const char* path,UINT version)
{
    ComPtr<ID3D12SDKConfiguration1> sdk;
    Hr(D3D12GetInterface(CLSID_D3D12SDKConfiguration,IID_PPV_ARGS(&sdk)));
    ComPtr<ID3D12DeviceFactory> factory;
    Hr(sdk->CreateDeviceFactory(version,path,IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter; Hr(dxgi->EnumAdapters1(0,&adapter));
    ComPtr<ID3D12DeviceFactory> probeFactory;
    Hr(sdk->CreateDeviceFactory(version,path,IID_PPV_ARGS(&probeFactory)));
    ComPtr<ID3D12Device> probeDevice;
    Hr(probeFactory->CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&probeDevice)));
    MenuCommandState::DeviceCoverage probeCoverage;
    Hr(MenuCommandState::InstallDevice(probeDevice.Get(),&probeCoverage));
    Check(probeCoverage.installed,"low-feature probe obtains its own device coverage");
    ComPtr<ID3D12Device> device;
    auto created=factory->CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_2,IID_PPV_ARGS(&device));
    if(FAILED(created)) created=factory->CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
    Hr(created);
    Check(device.Get()!=probeDevice.Get(),"independent factory produces a distinct rendering device");
    MenuCommandState::DeviceCoverage actualCoverage;
    Hr(MenuCommandState::InstallDevice(device.Get(),&actualCoverage));
    Check(actualCoverage.installed,"render device gets native coverage even after same-SDK probe");
    Check(actualCoverage.deviceVtable==probeCoverage.deviceVtable,
        "regression exercises two distinct devices sharing the same native device vtable");
    std::printf("SAME DEVICE VTABLE probe=%p actual=%p ROOT=%p/%p TABLE=%p/%p\n",
        probeCoverage.deviceVtable,actualCoverage.deviceVtable,probeCoverage.graphicsSignature,
        actualCoverage.graphicsSignature,probeCoverage.graphicsTable,actualCoverage.graphicsTable);
    MenuCommandState::DeviceCoverage repeatedCoverage;
    Hr(MenuCommandState::InstallDevice(device.Get(),&repeatedCoverage));
    Check(!repeatedCoverage.installed,"repeated coverage on the same live device allocates no new probe");
    probeDevice.Reset(); probeFactory.Reset();
    Commands commands(device.Get());
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; ComPtr<ID3D12CommandQueue> queue;
    Hr(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)));
    GameBindings game(device.Get()); game.Bind(device.Get(),commands.list.Get());
    auto captured=MenuCommandState::Get(commands.list.Get());
    Check(captured && captured->state.graphics.signature.Get()==game.root.Get() && captured->state.heaps.size()==2,
        "actual Agility graphics root and sampler heap captured after probe-runtime transition");
    auto* previous=ImGui::GetCurrentContext(); auto* actualContext=ImGui::CreateContext();
    ImGui::SetCurrentContext(actualContext); ImGui::GetIO().IniFilename=nullptr;
    auto renderer=std::make_unique<InUpscalerDx12Renderer>(device.Get()); Check(renderer->Init(),"Agility renderer init");
    Frame(256,0); auto output=Output(device.Get(),256,true,false);
    game.FakeSr(commands.list.Get());
    Check(renderer->Render(ImGui::GetDrawData(),commands.list.Get(),output.Get(),DXGI_FORMAT_R8G8B8A8_UNORM),"actual Agility menu draw");
    game.DrawAfterMenu(commands.list.Get(),false); // Game SDK folder need not contain optional SDKLayers.
    auto token=std::make_shared<int>(1); std::weak_ptr<int> weak=token;
    Check(MenuGpuLifetime::Retain(commands.list.Get(),token),"actual Agility recording lease"); token.reset();
    ComPtr<ID3D12Fence> gate,done;
    Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
    Hr(queue->Wait(gate.Get(),1)); Hr(commands.list->Close()); Execute(queue.Get(),commands.list.Get());
    Hr(queue->Signal(done.Get(),1));
    Check(MenuGpuLifetime::PendingCount()==1,"actual Agility Execute hook owns queued menu work");
    ComPtr<ID3D12CommandAllocator> replacement;
    Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&replacement)));
    Hr(commands.list->Reset(replacement.Get(),nullptr)); Hr(commands.list->Close());
    Check(!weak.expired(),"actual Agility Reset preserves pending submission");
    renderer.reset(); ImGui::DestroyContext(); ImGui::SetCurrentContext(previous);
    Check(!weak.expired(),"actual Agility UI teardown preserves pending submission");
    Hr(gate->Signal(1)); Wait(done.Get(),1); MenuGpuLifetime::Collect();
    Check(weak.expired() && MenuGpuLifetime::PendingCount()==0,"actual Agility Reset and completion retire recording");
    CheckPixel(device.Get(),queue.Get(),output.Get(),false); CheckPixel(device.Get(),queue.Get(),game.target.Get(),true);
    std::printf("PASS: Agility SDK %u after initial OS probe; physical GPU cached sampler pixels and real submission/Reset hooks\n",version);
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==1 || argc==3,"expected either no SDK arguments or folder plus version");
        ComPtr<ID3D12Debug> debug;
        Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        ComPtr<ID3D12Debug1> debug1;
        if (SUCCEEDED(debug.As(&debug1))) debug1->SetEnableGPUBasedValidation(TRUE);
        ComPtr<IDXGIFactory4> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp;
        Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device;
        Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info;
        Hr(device.As(&info));
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        ComPtr<ID3D12CommandQueue> queue;
        Hr(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12Fence> gate, done;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        auto renderer = std::make_unique<InUpscalerDx12Renderer>(device.Get());
        Check(renderer->Init(), "renderer init");
        Frame(256, 0);
        Commands discarded(device.Get());
        Hr(MenuCommandState::Install(discarded.list.Get()));
        auto first = Output(device.Get(), 256, true, false);
        Check(!renderer->Render(ImGui::GetDrawData(), discarded.list.Get(), first.Get(), DXGI_FORMAT_R8G8B8A8_UNORM),
            "menu must not record without submission/reset hooks");
        Hr(MenuCommandState::Install(discarded.list.Get(),queue.Get()));
        Hr(discarded.list->Close());
        Hr(discarded.list->Reset(discarded.allocator.Get(),nullptr));
        if(argc>=3) AgilityAfterProbe(factory.Get(),argv[1],static_cast<UINT>(std::strtoul(argv[2],nullptr,10)));
        ComPtr<ID3D12Device> foreignDevice;
        ComPtr<IDXGIAdapter1> hardware;
        if (SUCCEEDED(factory->EnumAdapters1(0, &hardware)) &&
            SUCCEEDED(D3D12CreateDevice(hardware.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&foreignDevice))) &&
            foreignDevice.Get()!=device.Get())
        {
            Commands foreignCommands(foreignDevice.Get());
            GameBindings foreignBindings(foreignDevice.Get());
            foreignBindings.Bind(foreignDevice.Get(),foreignCommands.list.Get());
            auto captured=MenuCommandState::Get(foreignCommands.list.Get());
            Check(captured && captured->state.graphics.signature.Get()==foreignBindings.root.Get() &&
                captured->state.heaps.size()==2,"second-device root/sampler capture after initial WARP probe");
            auto** firstVtable=*reinterpret_cast<void***>(discarded.list.Get());
            auto** secondVtable=*reinterpret_cast<void***>(foreignCommands.list.Get());
            for(UINT slot=20;slot<=46;++slot) if(firstVtable[slot]!=secondVtable[slot])
                std::printf("DEVICE METHOD DIFFERENCE slot=%u initial=%p actual=%p\n",slot,firstVtable[slot],secondVtable[slot]);
            Hr(foreignCommands.list->Close());
            auto foreignOutput = Output(foreignDevice.Get(), 256, true, false);
            Check(!renderer->Render(ImGui::GetDrawData(), discarded.list.Get(), foreignOutput.Get(), DXGI_FORMAT_R8G8B8A8_UNORM),
                "foreign output rejected before recording");
        }
        else std::puts("SKIP: second-adapter ownership check unavailable; WARP lifetime/pixel regression still required");
        const auto drawResult = renderer->Render(ImGui::GetDrawData(), discarded.list.Get(), first.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!drawResult) for (UINT64 n=0; n<info->GetNumStoredMessages(); ++n) {
            SIZE_T bytes{}; info->GetMessage(n,nullptr,&bytes); std::vector<unsigned char> storage(bytes);
            auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); info->GetMessage(n,m,&bytes);
            std::fprintf(stderr,"%s\n",m->pDescription);
        }
        Check(drawResult, "discarded draw records");
        auto discardedToken = std::make_shared<int>(1);
        std::weak_ptr<int> discardedWeak = discardedToken;
        Check(MenuGpuLifetime::Retain(discarded.list.Get(), discardedToken), "retain discarded recording");
        discardedToken.reset();
        Hr(discarded.list->Close());
        Hr(discarded.allocator->Reset());
        Hr(discarded.list->Reset(discarded.allocator.Get(), nullptr));
        Check(discardedWeak.expired(), "discarded recording releases without queue submission");
        Hr(discarded.list->Close());
        Hr(queue->Wait(gate.Get(), 1));
        std::vector<Commands> oldCommands;
        std::vector<ComPtr<ID3D12CommandAllocator>> resetAllocators;
        std::vector<ComPtr<ID3D12Resource>> outputs;
        std::vector<std::weak_ptr<int>> tokens;
        std::vector<std::unique_ptr<GameBindings>> gameBindings;
        for (unsigned i = 0; i < 8; ++i)
        {
            const auto size = 256 + i * 8;
            Frame(size, i);
            oldCommands.emplace_back(device.Get());
            auto& commands = oldCommands.back();
            outputs.push_back(Output(device.Get(), size, i % 2 == 0, i % 3 == 0));
            gameBindings.push_back(std::make_unique<GameBindings>(device.Get()));
            gameBindings.back()->Bind(device.Get(),commands.list.Get());
            gameBindings.back()->FakeSr(commands.list.Get());
            Check(renderer->Render(ImGui::GetDrawData(), commands.list.Get(), outputs.back().Get(), DXGI_FORMAT_R8G8B8A8_UNORM),
                "growing per-eye menu draw");
            gameBindings.back()->DrawAfterMenu(commands.list.Get());
            auto token = std::make_shared<int>(static_cast<int>(i)); tokens.push_back(token);
            Check(MenuGpuLifetime::Retain(commands.list.Get(), token), "retain queued recording");
            token.reset();
            Hr(commands.list->Close());
            Execute(queue.Get(), commands.list.Get());
            if (i == 0) continue; // Preserve this recording for legal re-execution after completion.
            ComPtr<ID3D12CommandAllocator> replacement;
            Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement)));
            Hr(commands.list->Reset(replacement.Get(), nullptr));
            Hr(commands.list->Close());
            resetAllocators.push_back(replacement);
            Check(!tokens.back().expired(), "Reset cannot release outstanding GPU submission");
        }
        Check(MenuGpuLifetime::PendingCount() == 8, "all eye submissions remain owned while GPU is blocked");
        renderer.reset();
        ImGui::DestroyContext();
        for (const auto& token : tokens) Check(!token.expired(), "renderer/context destruction preserves GPU work");
        Hr(gate->Signal(1));
        Hr(queue->Signal(done.Get(), 1)); Wait(done.Get(), 1);
        MenuGpuLifetime::Collect();
        Check(MenuGpuLifetime::PendingCount() == 0, "all completed submissions retire");
        Check(!tokens[0].expired(), "closed recording still owns reusable draw after execution retires");
        Execute(queue.Get(), oldCommands[0].list.Get());
        Hr(queue->Signal(done.Get(), 2)); Wait(done.Get(), 2);
        MenuGpuLifetime::Collect();
        Check(MenuGpuLifetime::PendingCount()==0, "legal re-execution retires independently");
        Hr(oldCommands[0].allocator->Reset());
        Hr(oldCommands[0].list->Reset(oldCommands[0].allocator.Get(), nullptr));
        Hr(oldCommands[0].list->Close());
        for (const auto& token : tokens) Check(token.expired(), "completed draw ownership released");
        // Read back each distinct eye: delayed submissions must retain their own mesh/font/target.
        const auto eyeCount=outputs.size();
        for(auto& game:gameBindings) outputs.push_back(game->target);
        for (unsigned i = 0; i < outputs.size(); ++i)
        {
            Commands copy(device.Get());
            const auto desc = outputs[i]->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes{};
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = bytes; buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            ComPtr<ID3D12Resource> readback;
            Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&readback)));
            Transition(copy.list.Get(), outputs[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = outputs[i].Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
            copy.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            Transition(copy.list.Get(), outputs[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Hr(copy.list->Close()); Execute(queue.Get(), copy.list.Get());
            Hr(queue->Signal(done.Get(), i + 3)); Wait(done.Get(), i + 3);
            void* pixels{}; Hr(readback->Map(0, nullptr, &pixels));
            const auto* pixel = static_cast<unsigned char*>(pixels) + footprint.Offset + 40 * footprint.Footprint.RowPitch + 40 * 4;
            if(i<eyeCount) Check(pixel[i % 2 ? 1 : 0] > 240 && pixel[i % 2 ? 0 : 1] < 10, "per-eye delayed menu color preserved");
            else Check(pixel[2]>180 && pixel[2]<200 && pixel[0]<10 && pixel[1]<10,
                "game cached graphics signature/heap/constants/PSO/RT/viewport/scissor/blend restored after menu");
            readback->Unmap(0, nullptr);
        }
        unsigned errors{};
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes{}; Hr(info->GetMessage(i, nullptr, &bytes));
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Hr(info->GetMessage(i, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            { ++errors; std::fprintf(stderr, "%s\n", message->pDescription); }
        }
        Check(errors == 0, "D3D12 debug/GPU validation reported errors");
        Hr(MenuCommandState::Uninstall());
        std::printf("PASS: %u menu lifetime checks; delayed dual-eye/growth/typeless/copy/re-execution/Reset/context teardown/cached game draw after menu; GPU errors=0\n", checks);
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
