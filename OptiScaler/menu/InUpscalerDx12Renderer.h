#pragma once

#include "MenuGpuLifetime.h"
#include "MenuCommandState.h"
#include <d3dcompiler.h>
#include <imgui/imgui.h>
#include <unordered_map>
#include <cstring>
#include <cfloat>

// The headset menu records on the application's SR list. It cannot use the
// desktop backend's once-per-frame buffer ring or its separate upload queue.
// Each draw owns its mesh, atlas image, descriptors and optional copy target.
class InUpscalerDx12Renderer
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Image { int width{}, height{}; std::vector<unsigned char> rgba; };
    struct Texture { ComPtr<ID3D12Resource> image, upload; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; };
    struct Draw
    {
        ComPtr<ID3D12Resource> vertices, indices, output, target;
        ComPtr<ID3D12DescriptorHeap> srv, rtv;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pipeline;
        std::vector<Texture> textures;
        std::unordered_map<ImTextureID, UINT> textureIndices;
    };
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3DBlob> vs_, ps_;
    std::unordered_map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> pipelines_;
    std::unordered_map<ImTextureID, Image> images_;
    ImTextureID nextTexture_{1};
    bool initialized_{};

    static ComPtr<IUnknown> Identity(IUnknown* object)
    {
        // Streamline forwards resource/list methods but can return a proxy device.
        // Compare native COM identity, not the address of two different wrappers.
        constexpr GUID nativeGuid{0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
        ComPtr<IUnknown> native, identity;
        if (SUCCEEDED(object->QueryInterface(nativeGuid, reinterpret_cast<void**>(native.GetAddressOf()))))
            object=native.Get();
        object->QueryInterface(IID_PPV_ARGS(&identity));
        return identity;
    }
    bool Owns(ID3D12DeviceChild* child) const
    {
        ComPtr<ID3D12Device> device;
        if (FAILED(child->GetDevice(IID_PPV_ARGS(&device)))) return false;
        const auto expected=Identity(device_.Get()), actual=Identity(device.Get());
        return expected && actual && expected.Get()==actual.Get();
    }
    static D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES h{}; h.Type=type; h.CreationNodeMask=h.VisibleNodeMask=1; return h;
    }
    bool Buffer(UINT64 bytes, ComPtr<ID3D12Resource>& resource)
    {
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width=bytes; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1;
        d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto h=Heap(D3D12_HEAP_TYPE_UPLOAD);
        return SUCCEEDED(device_->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&resource)));
    }
    bool TextureResource(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                         D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& resource)
    {
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=width; d.Height=height; d.DepthOrArraySize=1; d.MipLevels=1;
        d.Format=format; d.SampleDesc.Count=1; d.Flags=flags;
        const auto h=Heap(D3D12_HEAP_TYPE_DEFAULT);
        return SUCCEEDED(device_->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,
            nullptr,IID_PPV_ARGS(&resource)));
    }
    bool Pipeline(DXGI_FORMAT format, Draw& draw)
    {
        if (!root_)
        {
            constexpr char shader[]=R"(
cbuffer Projection : register(b0) { float4x4 projectionMatrix; };
struct V { float2 pos:POSITION; float2 uv:TEXCOORD0; float4 color:COLOR0; };
struct P { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 color:COLOR0; };
P VS(V v) { P p; p.pos=mul(projectionMatrix,float4(v.pos,0,1)); p.uv=v.uv; p.color=v.color; return p; }
Texture2D atlas:register(t0); SamplerState sampleAtlas:register(s0);
float4 PS(P p):SV_Target { return p.color*atlas.Sample(sampleAtlas,p.uv); }
)";
            if (FAILED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs_,nullptr)) ||
                FAILED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PS","ps_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps_,nullptr))) return false;
            D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
            D3D12_ROOT_PARAMETER p[2]{};
            p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            p[0].Constants={0,0,16}; p[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
            p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[1].DescriptorTable={1,&range}; p[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD=FLT_MAX;
            sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC desc{2,p,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
            ComPtr<ID3DBlob> blob;
            if (FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr)) ||
                FAILED(device_->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
                    IID_PPV_ARGS(&root_)))) return false;
        }
        auto& pipeline=pipelines_[format];
        if (!pipeline)
        {
            D3D12_INPUT_ELEMENT_DESC layout[]={
                {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,(UINT)offsetof(ImDrawVert,pos),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
                {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,(UINT)offsetof(ImDrawVert,uv),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
                {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,(UINT)offsetof(ImDrawVert,col),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
            p.pRootSignature=root_.Get(); p.VS={vs_->GetBufferPointer(),vs_->GetBufferSize()};
            p.PS={ps_->GetBufferPointer(),ps_->GetBufferSize()}; p.InputLayout={layout,3};
            p.SampleMask=UINT_MAX; p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            p.NumRenderTargets=1; p.RTVFormats[0]=format; p.SampleDesc.Count=1;
            auto& blend=p.BlendState.RenderTarget[0]; blend.BlendEnable=TRUE;
            blend.SrcBlend=D3D12_BLEND_SRC_ALPHA; blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;
            blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
            p.RasterizerState.DepthClipEnable=TRUE;
            p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
            p.DepthStencilState.FrontFace.StencilFunc=p.DepthStencilState.BackFace.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;
            p.DepthStencilState.FrontFace.StencilFailOp=p.DepthStencilState.FrontFace.StencilDepthFailOp=
                p.DepthStencilState.FrontFace.StencilPassOp=D3D12_STENCIL_OP_KEEP;
            p.DepthStencilState.BackFace=p.DepthStencilState.FrontFace;
            if (FAILED(device_->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline)))) return false;
        }
        draw.root=root_; draw.pipeline=pipeline; return true;
    }
    bool UpdateImages(ImDrawData* data)
    {
        if (!data->Textures) return true;
        for (auto* tex:*data->Textures)
        {
            if (tex->Status==ImTextureStatus_WantDestroy)
            {
                images_.erase(tex->GetTexID()); tex->SetTexID(ImTextureID_Invalid);
                tex->BackendUserData=nullptr; tex->SetStatus(ImTextureStatus_Destroyed); continue;
            }
            if (tex->Status!=ImTextureStatus_WantCreate && tex->Status!=ImTextureStatus_WantUpdates) continue;
            if (!tex->Pixels || tex->Width<=0 || tex->Height<=0) return false;
            if (tex->Status==ImTextureStatus_WantCreate)
            {
                tex->SetTexID(nextTexture_++);
                tex->BackendUserData=this;
            }
            Image image; image.width=tex->Width; image.height=tex->Height;
            const auto pixels=(size_t)image.width*image.height;
            image.rgba.resize(pixels*4);
            if (tex->Format==ImTextureFormat_RGBA32) std::memcpy(image.rgba.data(),tex->Pixels,pixels*4);
            else if (tex->Format==ImTextureFormat_Alpha8)
                for(size_t i=0;i<pixels;++i) {
                    image.rgba[i*4]=image.rgba[i*4+1]=image.rgba[i*4+2]=255; image.rgba[i*4+3]=tex->Pixels[i];
                }
            else return false;
            images_[tex->GetTexID()]=std::move(image); tex->SetStatus(ImTextureStatus_OK);
        }
        return true;
    }
    static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to}; list->ResourceBarrier(1,&b);
    }
public:
    explicit InUpscalerDx12Renderer(ID3D12Device* device) : device_(device) {}
    bool Init()
    {
        if (initialized_) return true;
        auto& io=ImGui::GetIO();
        if (io.BackendRendererUserData) return false;
        io.BackendRendererUserData=this; io.BackendRendererName="OptiScaler_DX12_submission_owned";
        io.BackendFlags|=ImGuiBackendFlags_RendererHasVtxOffset|ImGuiBackendFlags_RendererHasTextures;
        initialized_=true; return true;
    }
    ~InUpscalerDx12Renderer()
    {
        if (!initialized_ || !ImGui::GetCurrentContext()) return;
        auto& io=ImGui::GetIO();
        if(io.BackendRendererUserData!=this) return;
        for (auto* tex:ImGui::GetPlatformIO().Textures)
            if(tex->BackendUserData==this) {
                tex->SetTexID(ImTextureID_Invalid); tex->BackendUserData=nullptr; tex->SetStatus(ImTextureStatus_Destroyed);
            }
        io.BackendRendererUserData=nullptr; io.BackendRendererName=nullptr;
        io.BackendFlags&=~(ImGuiBackendFlags_RendererHasVtxOffset|ImGuiBackendFlags_RendererHasTextures);
        // Draw snapshots hold all GPU references beyond the UI/context's lifetime.
    }
    bool Render(ImDrawData* data, ID3D12GraphicsCommandList* list, ID3D12Resource* output, DXGI_FORMAT format)
    {
        MenuGpuLifetime::Collect();
        if (!MenuGpuLifetime::Ready() || !MenuCommandState::hooksReady || !data || !list || !output || !Owns(list) || !Owns(output)) return false;
        if (data->TotalVtxCount<=0 || data->TotalIdxCount<=0) return true;
        const auto desc=output->GetDesc();
        if (desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize!=1 || desc.SampleDesc.Count!=1 ||
            data->DisplaySize.x<=0 || data->DisplaySize.y<=0) return false;
        if (!UpdateImages(data)) return false;
        auto draw=std::make_shared<Draw>(); draw->output=output;
        if (!Pipeline(format,*draw) || !Buffer((UINT64)data->TotalVtxCount*sizeof(ImDrawVert),draw->vertices) ||
            !Buffer((UINT64)data->TotalIdxCount*sizeof(ImDrawIdx),draw->indices)) return false;
        draw->vertices->SetName(L"OptiScaler V33022 immutable menu vertices");
        draw->indices->SetName(L"OptiScaler V33022 immutable menu indices");
        void* vertices=nullptr; void* indices=nullptr; D3D12_RANGE empty{0,0};
        if (FAILED(draw->vertices->Map(0,&empty,&vertices))) return false;
        if (FAILED(draw->indices->Map(0,&empty,&indices))) { draw->vertices->Unmap(0,nullptr); return false; }
        auto* v=(ImDrawVert*)vertices; auto* i=(ImDrawIdx*)indices;
        for (const auto* commands:data->CmdLists) {
            std::memcpy(v,commands->VtxBuffer.Data,commands->VtxBuffer.Size*sizeof(ImDrawVert)); v+=commands->VtxBuffer.Size;
            std::memcpy(i,commands->IdxBuffer.Data,commands->IdxBuffer.Size*sizeof(ImDrawIdx)); i+=commands->IdxBuffer.Size;
            for (const auto& cmd:commands->CmdBuffer) if (!cmd.UserCallback && cmd.ElemCount)
                draw->textureIndices.emplace(cmd.GetTexID(),(UINT)draw->textureIndices.size());
        }
        draw->vertices->Unmap(0,nullptr); draw->indices->Unmap(0,nullptr);
        if (draw->textureIndices.empty()) return true;
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors=(UINT)draw->textureIndices.size(); heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&draw->srv)))) return false;
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors=1; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&draw->rtv)))) return false;
        const bool direct=(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)!=0;
        if (!direct && !TextureResource((UINT)desc.Width,desc.Height,format,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,draw->target)) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format=format; rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(direct?output:draw->target.Get(),&rtv,draw->rtv->GetCPUDescriptorHandleForHeapStart());
        draw->textures.resize(draw->textureIndices.size());
        const auto increment=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for(const auto& [id,index]:draw->textureIndices)
        {
            const auto found=images_.find(id); if(found==images_.end()) return false;
            const auto& image=found->second; auto& texture=draw->textures[index];
            if(!TextureResource(image.width,image.height,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COMMON,texture.image)) return false;
            texture.image->SetName(L"OptiScaler V33022 immutable menu atlas");
            const auto td=texture.image->GetDesc(); UINT64 bytes=0;
            device_->GetCopyableFootprints(&td,0,1,0,&texture.footprint,nullptr,nullptr,&bytes);
            if(!Buffer(bytes,texture.upload)) return false;
            void* pixels=nullptr; if(FAILED(texture.upload->Map(0,&empty,&pixels))) return false;
            for(int row=0;row<image.height;++row)
                std::memcpy((char*)pixels+texture.footprint.Offset+(size_t)row*texture.footprint.Footprint.RowPitch,
                            image.rgba.data()+(size_t)row*image.width*4,(size_t)image.width*4);
            texture.upload->Unmap(0,nullptr);
            auto cpu=draw->srv->GetCPUDescriptorHandleForHeapStart(); cpu.ptr+=(SIZE_T)index*increment;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels=1; device_->CreateShaderResourceView(texture.image.Get(),&srv,cpu);
        }
        // All allocations/maps succeed before the first command is recorded.
        if(!MenuGpuLifetime::Retain(list,draw)) return false;
        MenuCommandState::ProtectedDraw restore(list);
        if(!restore.Retain()) return false;
        for(auto& texture:draw->textures)
        {
            Barrier(list,texture.image.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=texture.upload.Get();
            src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=texture.footprint;
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=texture.image.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            Barrier(list,texture.image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if(direct) Barrier(list,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RENDER_TARGET);
        else {
            Barrier(list,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(list,draw->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(draw->target.Get(),output);
            Barrier(list,draw->target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        const auto setup=[&] {
            list->SetPipelineState(draw->pipeline.Get()); list->SetGraphicsRootSignature(draw->root.Get());
            ID3D12DescriptorHeap* heaps[]={draw->srv.Get()}; list->SetDescriptorHeaps(1,heaps);
            const auto r=draw->rtv->GetCPUDescriptorHandleForHeapStart(); list->OMSetRenderTargets(1,&r,FALSE,nullptr);
            D3D12_VIEWPORT viewport{0,0,data->DisplaySize.x*data->FramebufferScale.x,data->DisplaySize.y*data->FramebufferScale.y,0,1};
            list->RSSetViewports(1,&viewport); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_VERTEX_BUFFER_VIEW vb{draw->vertices->GetGPUVirtualAddress(),(UINT)(data->TotalVtxCount*sizeof(ImDrawVert)),sizeof(ImDrawVert)};
            D3D12_INDEX_BUFFER_VIEW ib{draw->indices->GetGPUVirtualAddress(),(UINT)(data->TotalIdxCount*sizeof(ImDrawIdx)),sizeof(ImDrawIdx)==2?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT};
            list->IASetVertexBuffers(0,1,&vb); list->IASetIndexBuffer(&ib);
            const float l=data->DisplayPos.x, rpos=l+data->DisplaySize.x, t=data->DisplayPos.y, b=t+data->DisplaySize.y;
            const float projection[16]={2/(rpos-l),0,0,0,0,2/(t-b),0,0,0,0,0.5f,0,(rpos+l)/(l-rpos),(t+b)/(b-t),0.5f,1};
            list->SetGraphicsRoot32BitConstants(0,16,projection,0);
            const float blend[4]={0,0,0,0}; list->OMSetBlendFactor(blend);
        };
        setup(); int vertexOffset=0, indexOffset=0;
        for(const auto* commands:data->CmdLists)
        {
            for(const auto& cmd:commands->CmdBuffer)
            {
                if(cmd.UserCallback) {
                    if(cmd.UserCallback==ImDrawCallback_ResetRenderState) setup(); else cmd.UserCallback(commands,&cmd);
                    continue;
                }
                if(!cmd.ElemCount) continue;
                D3D12_RECT clip{
                    (LONG)(std::max)(0.0f,(cmd.ClipRect.x-data->DisplayPos.x)*data->FramebufferScale.x),
                    (LONG)(std::max)(0.0f,(cmd.ClipRect.y-data->DisplayPos.y)*data->FramebufferScale.y),
                    (LONG)(std::min)((float)desc.Width,(cmd.ClipRect.z-data->DisplayPos.x)*data->FramebufferScale.x),
                    (LONG)(std::min)((float)desc.Height,(cmd.ClipRect.w-data->DisplayPos.y)*data->FramebufferScale.y)};
                if(clip.right<=clip.left || clip.bottom<=clip.top) continue;
                list->RSSetScissorRects(1,&clip);
                auto gpu=draw->srv->GetGPUDescriptorHandleForHeapStart(); gpu.ptr+=(UINT64)draw->textureIndices.at(cmd.GetTexID())*increment;
                list->SetGraphicsRootDescriptorTable(1,gpu);
                list->DrawIndexedInstanced(cmd.ElemCount,1,cmd.IdxOffset+indexOffset,(INT)cmd.VtxOffset+vertexOffset,0);
            }
            vertexOffset+=commands->VtxBuffer.Size; indexOffset+=commands->IdxBuffer.Size;
        }
        if(direct) Barrier(list,output,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        else {
            Barrier(list,draw->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(list,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(output,draw->target.Get());
            Barrier(list,output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(list,draw->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        for(auto& texture:draw->textures) Barrier(list,texture.image.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        return true;
    }
};
