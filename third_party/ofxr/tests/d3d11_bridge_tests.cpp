// The D3D11 bridge's three paths against real devices on one adapter: the
// direct one, the depth one (shared if the driver allows, copied if not) and
// the multisampled one (resolved). No runtime, no layer: a "runtime image" is
// a D3D12 texture in the state the runtime would hold it in, and the check
// is what that texture holds after a release.

#include "xrfg/d3d11_bridge.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

struct Devices {
    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D12Device> d3d12;
    ComPtr<ID3D12CommandQueue> queue;
};

[[nodiscard]] bool create_devices(Devices* devices) {
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            devices->d3d11.GetAddressOf(), &level,
            devices->context.GetAddressOf()))) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(devices->d3d11.As(&dxgi_device)) ||
        FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf())) ||
        FAILED(D3D12CreateDevice(
            adapter.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(devices->d3d12.GetAddressOf())))) {
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    return SUCCEEDED(devices->d3d12->CreateCommandQueue(
        &queue_description, IID_PPV_ARGS(devices->queue.GetAddressOf())));
}

// A runtime image: what XR_KHR_D3D12_enable hands out, resting in
// RENDER_TARGET or DEPTH_WRITE.
[[nodiscard]] ComPtr<ID3D12Resource> create_runtime_image(
    ID3D12Device* device, DXGI_FORMAT format, bool depth, UINT array_size,
    UINT mip_levels = 1) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = 8;
    description.Height = 8;
    description.DepthOrArraySize = static_cast<UINT16>(array_size);
    description.MipLevels = static_cast<UINT16>(mip_levels);
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = depth
        ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
        : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> image;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                  : D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr, IID_PPV_ARGS(image.GetAddressOf())))) {
        return nullptr;
    }
    return image;
}

// Mip 0, slice 0 of a runtime image, read back through the queue.
[[nodiscard]] bool read_back(
    const Devices& devices, ID3D12Resource* image, bool depth,
    std::vector<std::uint8_t>* bytes, UINT* row_pitch) {
    const D3D12_RESOURCE_DESC description = image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    devices.d3d12->GetCopyableFootprints(
        &description, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(devices.d3d12->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(readback.GetAddressOf()))) ||
        FAILED(devices.d3d12->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(devices.d3d12->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(list.GetAddressOf()))) ||
        FAILED(devices.d3d12->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())))) {
        return false;
    }
    const D3D12_RESOURCE_STATES resting = depth
        ? D3D12_RESOURCE_STATE_DEPTH_WRITE
        : D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = image;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = resting;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = image;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = resting;
    list->ResourceBarrier(1, &barrier);
    if (FAILED(list->Close())) {
        return false;
    }
    ID3D12CommandList* const lists[] = {list.Get()};
    devices.queue->ExecuteCommandLists(1, lists);
    if (FAILED(devices.queue->Signal(fence.Get(), 1))) {
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        return false;
    }
    bool completed = false;
    if (SUCCEEDED(fence->SetEventOnCompletion(1, event))) {
        completed = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    }
    CloseHandle(event);
    if (!completed) {
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    if (FAILED(readback->Map(0, &range, &mapped))) {
        return false;
    }
    bytes->assign(
        static_cast<const std::uint8_t*>(mapped),
        static_cast<const std::uint8_t*>(mapped) + total);
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    *row_pitch = footprint.Footprint.RowPitch;
    return true;
}

void test_direct(const Devices& devices) {
    std::array<ComPtr<ID3D12Resource>, 2> runtime;
    for (auto& image : runtime) {
        image = create_runtime_image(devices.d3d12.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, false, 2);
    }
    expect(runtime[0] && runtime[1], "direct: runtime images created");
    const ID3D12Resource* const raw[] = {runtime[0].Get(), runtime[1].Get()};
    std::vector<ID3D12Resource*> images{const_cast<ID3D12Resource*>(raw[0]), const_cast<ID3D12Resource*>(raw[1])};

    xrfg::D3D11BridgeSwapchain bridge;
    xrfg::D3D11BridgeSwapchainDescription requested{};
    requested.requested_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    requested.requested_sample_count = 1;
    std::uint32_t stage = 0;
    const HRESULT init = bridge.initialize(
        devices.d3d11.Get(), devices.context.Get(), devices.d3d12.Get(),
        devices.queue.Get(), images, requested, &stage);
    expect(SUCCEEDED(init), "direct: bridge initialised");
    if (FAILED(init)) {
        std::cerr << "  stage " << stage << " hr 0x" << std::hex << init << std::dec << '\n';
        return;
    }
    expect(bridge.path() == xrfg::D3D11BridgePath::direct, "direct: path is direct");
    expect(bridge.d3d11_images().size() == 2, "direct: two application textures");

    // The application paints image 1 and releases it.
    std::vector<std::uint8_t> pixels(8 * 8 * 4, 0);
    for (std::size_t index = 0; index < pixels.size(); index += 4) {
        pixels[index] = 200;
        pixels[index + 3] = 255;
    }
    expect(SUCCEEDED(bridge.before_write(1)), "direct: before_write");
    devices.context->UpdateSubresource(
        bridge.d3d11_images()[1], 0, nullptr, pixels.data(), 8 * 4, 0);
    expect(SUCCEEDED(bridge.release(1)), "direct: release");
    expect(SUCCEEDED(bridge.mark_read(1)), "direct: mark_read");
    expect(SUCCEEDED(bridge.wait_for_idle()), "direct: idle");
    std::vector<std::uint8_t> bytes;
    UINT pitch = 0;
    expect(read_back(devices, runtime[1].Get(), false, &bytes, &pitch), "direct: readback");
    expect(!bytes.empty() && bytes[0] == 200 && bytes[3] == 255,
        "direct: the runtime image holds what the application painted");
    // Image 0 was never released and stays untouched.
    expect(read_back(devices, runtime[0].Get(), false, &bytes, &pitch), "direct: readback 0");
    expect(!bytes.empty() && bytes[0] == 0, "direct: an unreleased image is untouched");
}

// Cyberpunk 2077 asks for four mip levels on its colour swapchains, and
// D3D11 refused to open the first build's shared texture for it.
void test_mipmapped(const Devices& devices) {
    ComPtr<ID3D12Resource> runtime = create_runtime_image(
        devices.d3d12.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, false, 2, 4);
    expect(runtime != nullptr, "mips: runtime image created");
    if (!runtime) return;
    std::vector<ID3D12Resource*> images{runtime.Get()};
    xrfg::D3D11BridgeSwapchain bridge;
    xrfg::D3D11BridgeSwapchainDescription requested{};
    requested.requested_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    requested.requested_sample_count = 1;
    std::uint32_t stage = 0;
    const HRESULT init = bridge.initialize(
        devices.d3d11.Get(), devices.context.Get(), devices.d3d12.Get(),
        devices.queue.Get(), images, requested, &stage);
    expect(SUCCEEDED(init), "mips: bridge initialised");
    if (FAILED(init)) {
        std::cerr << "  stage " << stage << " hr 0x" << std::hex << init << std::dec << '\n';
        return;
    }
    std::cout << "mip path: " << static_cast<unsigned>(bridge.path()) << '\n';
    ID3D11Texture2D* const texture = bridge.d3d11_images()[0];
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    expect(description.MipLevels == 4, "mips: the application's texture has four mips");
    std::vector<std::uint8_t> pixels(8 * 8 * 4, 0);
    for (std::size_t index = 0; index < pixels.size(); index += 4) {
        pixels[index] = 90;
        pixels[index + 3] = 255;
    }
    expect(SUCCEEDED(bridge.before_write(0)), "mips: before_write");
    devices.context->UpdateSubresource(texture, 0, nullptr, pixels.data(), 8 * 4, 0);
    expect(SUCCEEDED(bridge.release(0)), "mips: release");
    expect(SUCCEEDED(bridge.mark_read(0)), "mips: mark_read");
    expect(SUCCEEDED(bridge.wait_for_idle()), "mips: idle");
    std::vector<std::uint8_t> bytes;
    UINT pitch = 0;
    expect(read_back(devices, runtime.Get(), false, &bytes, &pitch), "mips: readback");
    expect(!bytes.empty() && bytes[0] == 90, "mips: the runtime image holds mip 0");
}

void test_depth(const Devices& devices) {
    ComPtr<ID3D12Resource> runtime =
        create_runtime_image(devices.d3d12.Get(), DXGI_FORMAT_D32_FLOAT, true, 1);
    expect(runtime != nullptr, "depth: runtime image created");
    if (!runtime) return;
    std::vector<ID3D12Resource*> images{runtime.Get()};

    xrfg::D3D11BridgeSwapchain bridge;
    xrfg::D3D11BridgeSwapchainDescription requested{};
    requested.requested_format = DXGI_FORMAT_D32_FLOAT;
    requested.requested_sample_count = 1;
    requested.depth_stencil = true;
    std::uint32_t stage = 0;
    const HRESULT init = bridge.initialize(
        devices.d3d11.Get(), devices.context.Get(), devices.d3d12.Get(),
        devices.queue.Get(), images, requested, &stage);
    expect(SUCCEEDED(init), "depth: bridge initialised on one of its two paths");
    if (FAILED(init)) {
        std::cerr << "  stage " << stage << " hr 0x" << std::hex << init << std::dec << '\n';
        return;
    }
    std::cout << "depth path: "
              << (bridge.path() == xrfg::D3D11BridgePath::direct ? "shared" : "copied")
              << '\n';

    // The application clears its depth view to 0.25 and releases.
    ID3D11Texture2D* const texture = bridge.d3d11_images()[0];
    D3D11_DEPTH_STENCIL_VIEW_DESC view_description{};
    view_description.Format = DXGI_FORMAT_D32_FLOAT;
    view_description.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> view;
    expect(SUCCEEDED(devices.d3d11->CreateDepthStencilView(
        texture, &view_description, view.GetAddressOf())),
        "depth: the application can create its depth view on the texture");
    if (!view) return;
    expect(SUCCEEDED(bridge.before_write(0)), "depth: before_write");
    devices.context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, 0.25F, 0);
    expect(SUCCEEDED(bridge.release(0)), "depth: release");
    expect(SUCCEEDED(bridge.mark_read(0)), "depth: mark_read");
    expect(SUCCEEDED(bridge.wait_for_idle()), "depth: idle");
    std::vector<std::uint8_t> bytes;
    UINT pitch = 0;
    expect(read_back(devices, runtime.Get(), true, &bytes, &pitch), "depth: readback");
    float value = 0.0F;
    if (bytes.size() >= sizeof(float)) {
        std::memcpy(&value, bytes.data(), sizeof(float));
    }
    expect(value > 0.24F && value < 0.26F,
        "depth: the runtime's depth image holds the cleared value");
}

// The packed depth-stencil family D3D11 will not open shared in any form:
// the bridge keeps the depth on the application's side and the runtime's
// image is left alone (depth_private). Should a driver ever share it, the
// copy path is just as acceptable; the test only requires that the creation
// succeeds and the application can render depth into what it is given.
void test_packed_depth(const Devices& devices) {
    ComPtr<ID3D12Resource> runtime = create_runtime_image(
        devices.d3d12.Get(), DXGI_FORMAT_D24_UNORM_S8_UINT, true, 2);
    expect(runtime != nullptr, "packed depth: runtime image created");
    if (!runtime) return;
    std::vector<ID3D12Resource*> images{runtime.Get()};

    xrfg::D3D11BridgeSwapchain bridge;
    xrfg::D3D11BridgeSwapchainDescription requested{};
    requested.requested_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    requested.requested_sample_count = 1;
    requested.depth_stencil = true;
    std::uint32_t stage = 0;
    const HRESULT init = bridge.initialize(
        devices.d3d11.Get(), devices.context.Get(), devices.d3d12.Get(),
        devices.queue.Get(), images, requested, &stage);
    expect(SUCCEEDED(init), "packed depth: the creation never fails");
    if (FAILED(init)) {
        std::cerr << "  stage " << stage << " hr 0x" << std::hex << init << std::dec << '\n';
        return;
    }
    std::cout << "packed depth path: " << static_cast<unsigned>(bridge.path()) << '\n';
    expect(bridge.d3d11_images().size() == 1, "packed depth: one application texture");
    if (bridge.d3d11_images().empty()) return;

    ID3D11Texture2D* const texture = bridge.d3d11_images()[0];
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    expect(description.ArraySize == 2, "packed depth: the application's texture keeps the array size");
    D3D11_DEPTH_STENCIL_VIEW_DESC view_description{};
    view_description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    view_description.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    view_description.Texture2DArray.ArraySize = 2;
    ComPtr<ID3D11DepthStencilView> view;
    expect(SUCCEEDED(devices.d3d11->CreateDepthStencilView(
        texture, &view_description, view.GetAddressOf())),
        "packed depth: the application can create its depth-stencil view");
    if (!view) return;
    expect(SUCCEEDED(bridge.before_write(0)), "packed depth: before_write");
    devices.context->ClearDepthStencilView(
        view.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.5F, 1);
    expect(SUCCEEDED(bridge.release(0)), "packed depth: release");
    expect(SUCCEEDED(bridge.mark_read(0)), "packed depth: mark_read");
    expect(SUCCEEDED(bridge.wait_for_idle()), "packed depth: idle");
}

void test_resolve(const Devices& devices) {
    ComPtr<ID3D12Resource> runtime =
        create_runtime_image(devices.d3d12.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, false, 2);
    expect(runtime != nullptr, "resolve: runtime image created");
    if (!runtime) return;
    std::vector<ID3D12Resource*> images{runtime.Get()};

    xrfg::D3D11BridgeSwapchain bridge;
    xrfg::D3D11BridgeSwapchainDescription requested{};
    requested.requested_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    requested.requested_sample_count = 4;
    std::uint32_t stage = 0;
    const HRESULT init = bridge.initialize(
        devices.d3d11.Get(), devices.context.Get(), devices.d3d12.Get(),
        devices.queue.Get(), images, requested, &stage);
    expect(SUCCEEDED(init), "resolve: bridge initialised");
    if (FAILED(init)) {
        std::cerr << "  stage " << stage << " hr 0x" << std::hex << init << std::dec << '\n';
        return;
    }
    expect(bridge.path() == xrfg::D3D11BridgePath::resolve, "resolve: path is resolve");
    ID3D11Texture2D* const texture = bridge.d3d11_images()[0];
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    expect(description.SampleDesc.Count == 4, "resolve: the application's texture is multisampled");

    // The application clears its render target to (0, 160, 0) and releases.
    D3D11_RENDER_TARGET_VIEW_DESC view_description{};
    view_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view_description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
    view_description.Texture2DMSArray.FirstArraySlice = 0;
    view_description.Texture2DMSArray.ArraySize = 1;
    ComPtr<ID3D11RenderTargetView> view;
    expect(SUCCEEDED(devices.d3d11->CreateRenderTargetView(
        texture, &view_description, view.GetAddressOf())),
        "resolve: the application can create its render target view");
    if (!view) return;
    const float colour[4] = {0.0F, 160.0F / 255.0F, 0.0F, 1.0F};
    expect(SUCCEEDED(bridge.before_write(0)), "resolve: before_write");
    devices.context->ClearRenderTargetView(view.Get(), colour);
    expect(SUCCEEDED(bridge.release(0)), "resolve: release");
    expect(SUCCEEDED(bridge.mark_read(0)), "resolve: mark_read");
    expect(SUCCEEDED(bridge.wait_for_idle()), "resolve: idle");
    std::vector<std::uint8_t> bytes;
    UINT pitch = 0;
    expect(read_back(devices, runtime.Get(), false, &bytes, &pitch), "resolve: readback");
    expect(bytes.size() >= 4 && bytes[0] == 0 && bytes[1] >= 158 && bytes[1] <= 162 && bytes[3] == 255,
        "resolve: the runtime's single-sample image holds the resolved colour");
}

} // namespace

int main() {
    Devices devices;
    if (!create_devices(&devices)) {
        std::cerr << "no hardware D3D11 and D3D12 device on one adapter; skipping\n";
        return 77;
    }
    test_direct(devices);
    test_mipmapped(devices);
    test_depth(devices);
    test_packed_depth(devices);
    test_resolve(devices);
    if (g_failures != 0) {
        std::cerr << g_failures << " D3D11 bridge check(s) failed\n";
        return 1;
    }
    std::cout << "D3D11 bridge tests passed\n";
    return 0;
}
