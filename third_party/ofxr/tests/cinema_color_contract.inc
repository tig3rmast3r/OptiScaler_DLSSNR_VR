#include "fsr_cinema_vertex_shader.hpp"
#include "fsr_cinema_pixel_shader.hpp"
#include "cinema_flatten_vertex_shader.hpp"
#include "cinema_flatten_pixel_shader.hpp"

// Exercise the production cinema shaders, not a CPU emulation. Distinct
// encoded eye colours, distinct panel positions and an sRGB XR target.
void test_cinema_color_contract(D3D12WarpFixture& fixture) {
    constexpr UINT width = 256, height = 128;
    auto source = create_source_texture(fixture, width, height);
    auto target = create_source_texture(fixture, width, height, DXGI_FORMAT_R8G8B8A8_TYPELESS);
    auto pattern = translated_motion_pattern(width, height, {0, 0});
    for (UINT eye = 0; eye < kEyeCount; ++eye)
        for (std::size_t i = 0; i < pattern[eye].size(); ++i)
            pattern[eye][i] = i % 4 == 3 ? 255 : (eye == 0 ? 64 : 128);
    upload_pattern(fixture, source.Get(), pattern);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv, rtv;
    require_hresult(fixture.device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv)), "cinema SRV heap");
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    require_hresult(fixture.device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv)), "cinema RTV heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = kFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = 2;
    fixture.device()->CreateShaderResourceView(source.Get(), &sd, srv->GetCPUDescriptorHandleForHeapStart());
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = {1, &range};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants = {0, 0, 24};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd{2, rp, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    require_hresult(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "cinema root serialize");
    ComPtr<ID3D12RootSignature> root;
    require_hresult(fixture.device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "cinema root");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.VS = {g_xrfg_fsr_cinema_vertex_shader, sizeof(g_xrfg_fsr_cinema_vertex_shader)};
    pd.PS = {g_xrfg_fsr_cinema_pixel_shader, sizeof(g_xrfg_fsr_cinema_pixel_shader)};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    require_hresult(fixture.device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline)), "cinema pipeline");
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* commands) {
        const auto barrier = transition_barrier(source.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(1, &barrier);
        ID3D12DescriptorHeap* heaps[]{srv.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetGraphicsRootSignature(root.Get());
        commands->SetPipelineState(pipeline.Get());
        commands->SetGraphicsRootDescriptorTable(0, srv->GetGPUDescriptorHandleForHeapStart());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        D3D12_RECT scissor{0, 0, width, height};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        for (UINT eye = 0; eye < 2; ++eye) {
            auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += eye * fixture.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_RENDER_TARGET_VIEW_DESC vd{};
            vd.Format = pd.RTVFormats[0];
            vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.FirstArraySlice = eye;
            vd.Texture2DArray.ArraySize = 1;
            fixture.device()->CreateRenderTargetView(target.Get(), &vd, handle);
            const float black[]{0, 0, 0, 1};
            commands->ClearRenderTargetView(handle, black, 0, nullptr);
            commands->OMSetRenderTargets(1, &handle, FALSE, nullptr);
            const float left = (eye == 0 ? -0.75F : -0.25F), right = left + 1.0F;
            struct Constants { float clip[16]; float rect[4]; UINT width, height, slice, pad; } data{
                {left, .5F, .5F, 1, right, .5F, .5F, 1, left, -.5F, .5F, 1, right, -.5F, .5F, 1},
                {0, 0, float(width), float(height)}, width, height, eye, 1};
            static_assert(sizeof(data) == 24 * sizeof(UINT));
            commands->SetGraphicsRoot32BitConstants(1, 24, &data, 0);
            commands->DrawInstanced(6, 1, 0, 0);
        }
    });
    const auto result = readback_pattern(fixture, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (UINT eye = 0; eye < 2; ++eye) {
        const UINT left = eye == 0 ? 32 : 96;
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            const bool inside = x >= left && x < left + 128 && y >= 32 && y < 96;
            const int expected = inside ? (eye == 0 ? 64 : 128) : 0;
            const auto index = (std::size_t(y) * width + x) * 4;
            require(std::abs(int(result[eye][index]) - expected) <= 1, "cinema panel colour, position or eye slice differs");
        }
    }
    fixture.require_no_debug_errors();
    // Recover flat display colour from the actual XR image. The source
    // descriptor must be sRGB, and the private flattened target UNORM.
    auto flat = create_source_texture(fixture, width, height);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    fixture.device()->CreateShaderResourceView(target.Get(), &sd, srv->GetCPUDescriptorHandleForHeapStart());
    pd.VS = {g_xrfg_cinema_flatten_vertex_shader, sizeof(g_xrfg_cinema_flatten_vertex_shader)};
    pd.PS = {g_xrfg_cinema_flatten_pixel_shader, sizeof(g_xrfg_cinema_flatten_pixel_shader)};
    pd.RTVFormats[0] = kFormat;
    ComPtr<ID3D12PipelineState> flatten;
    require_hresult(fixture.device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&flatten)), "flatten pipeline");
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* commands) {
        const auto barrier = transition_barrier(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(1, &barrier);
        ID3D12DescriptorHeap* heaps[]{srv.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetGraphicsRootSignature(root.Get());
        commands->SetPipelineState(flatten.Get());
        commands->SetGraphicsRootDescriptorTable(0, srv->GetGPUDescriptorHandleForHeapStart());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        D3D12_RECT scissor{0, 0, width, height};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        for (UINT eye = 0; eye < 2; ++eye) {
            auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += eye * fixture.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_RENDER_TARGET_VIEW_DESC vd{};
            vd.Format = kFormat;
            vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.FirstArraySlice = eye;
            vd.Texture2DArray.ArraySize = 1;
            fixture.device()->CreateRenderTargetView(flat.Get(), &vd, handle);
            commands->OMSetRenderTargets(1, &handle, FALSE, nullptr);
            const float left = eye == 0 ? -.75F : -.25F;
            struct Constants { float clip[16]; float rect[4]; UINT width, height, slice, pad; } data{
                {left, .5F, .5F, 1, left+1, .5F, .5F, 1, left, -.5F, .5F, 1, left+1, -.5F, .5F, 1},
                {}, width, height, eye, 0};
            commands->SetGraphicsRoot32BitConstants(1, 24, &data, 0);
            commands->DrawInstanced(3, 1, 0, 0);
        }
    });
    const auto recovered = readback_pattern(fixture, flat.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (UINT eye = 0; eye < 2; ++eye) {
        const double rgb_error = mean_absolute_rgb_error(recovered, pattern, width, height, eye, 4);
        std::cout << "Cinema final-colour pullback eye=" << eye << " MAE=" << rgb_error << '\n';
        require(rgb_error <= 1.0, "flattened final colour has wrong gamma, panel coordinates or eye");
    }
    fixture.require_no_debug_errors();
    std::cout << "Cinema production shader: sRGB colour, independent eye slices and panel placement passed\n";
}
