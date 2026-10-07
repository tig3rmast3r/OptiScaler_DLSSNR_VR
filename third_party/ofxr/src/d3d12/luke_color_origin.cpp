#include "xrfg/luke_color_origin.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdio>

namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;
struct Uploads {
    ComPtr<ID3D12Device> device;
    DXGI_FORMAT format{};
    UINT height{};
    std::array<ComPtr<ID3D12Resource>, 2> colors;
};
struct Reader {
    std::mutex mutex;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> readback;
    UINT64 bytes{}, last_fence{}, next_fence{1};
    HANDLE event{};
    bool poisoned{};
    ~Reader() { if (event) CloseHandle(event); }
};
struct Cache {
    std::mutex mutex;
    std::vector<std::shared_ptr<Uploads>> uploads;
    std::vector<std::shared_ptr<Reader>> readers;
};
Cache& cache() {
    // Immutable uploads must outlive every original game's command-list use.
    // Deliberate bounded process-lifetime ownership, not an unretired frame ring.
    static Cache* value = new Cache();
    return *value;
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* color, D3D12_RESOURCE_STATES from,
    D3D12_RESOURCE_STATES to) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return barrier;
}
HRESULT immutable_upload(ID3D12Device* device, DXGI_FORMAT format, UINT height,
    bool white, ComPtr<ID3D12Resource>& output) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(256) * height;
    desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(output.GetAddressOf()));
    if (FAILED(result)) return result;
    void* mapped{}; const D3D12_RANGE no_read{0, 0};
    result = output->Map(0, &no_read, &mapped);
    if (FAILED(result)) return result;
    std::memset(mapped, 0, static_cast<SIZE_T>(desc.Width));
    auto* bytes = static_cast<std::byte*>(mapped);
    const auto pixel_bytes = luke_origin_pixel_bytes(format);
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < 2; ++x)
            luke_origin_pixel(bytes + static_cast<UINT64>(y) * 256 + x * pixel_bytes, format, white);
    const D3D12_RANGE written{0, static_cast<SIZE_T>(desc.Width)};
    output->Unmap(0, &written);
    return S_OK;
}
HRESULT get_uploads(ID3D12Device* device, DXGI_FORMAT format, UINT height,
    std::shared_ptr<Uploads>& output) {
    auto& c = cache(); std::scoped_lock lock(c.mutex);
    for (const auto& candidate : c.uploads)
        if (candidate->device.Get() == device && candidate->format == format && candidate->height == height) {
            output = candidate; return S_OK;
        }
    if (c.uploads.size() >= 16) return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    auto created = std::make_shared<Uploads>();
    created->device = device; created->format = format; created->height = height;
    HRESULT result = immutable_upload(device, format, height, false, created->colors[0]);
    if (SUCCEEDED(result)) result = immutable_upload(device, format, height, true, created->colors[1]);
    if (FAILED(result)) return result;
    c.uploads.push_back(created); output = std::move(created);
    return S_OK;
}
HRESULT get_reader(ID3D12CommandQueue* queue, std::shared_ptr<Reader>& output) {
    auto& c = cache(); std::scoped_lock lock(c.mutex);
    for (const auto& candidate : c.readers)
        if (candidate->queue.Get() == queue) { output = candidate; return S_OK; }
    if (c.readers.size() >= 8) return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    auto created = std::make_shared<Reader>(); created->queue = queue;
    HRESULT result = queue->GetDevice(IID_PPV_ARGS(created->device.GetAddressOf()));
    if (SUCCEEDED(result)) result = created->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(created->allocator.GetAddressOf()));
    if (SUCCEEDED(result)) result = created->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        created->allocator.Get(), nullptr, IID_PPV_ARGS(created->list.GetAddressOf()));
    if (SUCCEEDED(result)) result = created->list->Close();
    if (SUCCEEDED(result)) result = created->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(created->fence.GetAddressOf()));
    if (FAILED(result)) return result;
    created->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!created->event) return HRESULT_FROM_WIN32(GetLastError());
    c.readers.push_back(created); output = std::move(created);
    return S_OK;
}
HRESULT wait_reader(Reader& reader, DWORD timeout_ms) {
    if (reader.poisoned) return E_FAIL;
    if (!reader.last_fence) return S_OK;
    const auto completed = reader.fence->GetCompletedValue();
    if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if (completed >= reader.last_fence) return S_OK;
    ResetEvent(reader.event);
    HRESULT result = reader.fence->SetEventOnCompletion(reader.last_fence, reader.event);
    if (FAILED(result)) return result;
    const DWORD wait = WaitForSingleObject(reader.event, timeout_ms);
    if (wait == WAIT_TIMEOUT) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    if (wait != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(GetLastError());
    const auto after = reader.fence->GetCompletedValue();
    if (after == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    return after >= reader.last_fence ? S_OK : DXGI_ERROR_WAS_STILL_DRAWING;
}


std::atomic<std::uint64_t> g_output_extent{}, g_color_extent{};
std::uint64_t extent_bits(unsigned width, unsigned height) noexcept {
    return (static_cast<std::uint64_t>(width) << 32) | height;
}
struct ReadTelemetry {
    std::mutex mutex;
    HANDLE file{INVALID_HANDLE_VALUE};
    std::uint64_t ok{}, missing{}, errors{}, last_ms{}, written{};
    bool opened{};
    ~ReadTelemetry() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};
void report_read(HRESULT result, std::uint32_t token, UINT width, UINT height) noexcept {
    try {
        static ReadTelemetry telemetry;
        std::scoped_lock lock(telemetry.mutex);
        if (SUCCEEDED(result) && token) ++telemetry.ok;
        else if (result == E_PENDING || result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            ++telemetry.missing;
        else ++telemetry.errors;
        const auto now = GetTickCount64();
        if (telemetry.last_ms && now - telemetry.last_ms < 1000) return;
        telemetry.last_ms = now;
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, result, 122, token,
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(telemetry.ok)) << 32) |
            static_cast<std::uint32_t>(telemetry.missing + telemetry.errors));
        if (!telemetry.opened) {
            telemetry.opened = true;
            HMODULE module{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&report_read), &module)) return;
            std::vector<wchar_t> path(32768);
            const auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (!count || count >= path.size()) return;
            const auto destination = std::filesystem::path(path.data()).parent_path() /
                (L"ofxr-luke-origin-pid" + std::to_wstring(GetCurrentProcessId()) + L".log");
            telemetry.file = CreateFileW(destination.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ |
                FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (telemetry.file != INVALID_HANDLE_VALUE) {
                constexpr char header[] = "# V33008 compact token32+CRC16, disjoint256x8 coverage bands\n"
                    "# uptime_ms,read_ok,read_missing,read_error,last_hr,token,roi_width,roi_height\n";
                DWORD written{};
                WriteFile(telemetry.file, header, static_cast<DWORD>(sizeof(header)-1), &written, nullptr);
                telemetry.written = written;
            }
        }
        // Bounded tiny normal log, independent of diagnostic profile; no raw pixels,
        // per-frame flush, timing gate, altered admission, or GPU observation work.
        if (telemetry.file == INVALID_HANDLE_VALUE || telemetry.written >= 4*1024*1024) return;
        char row[256]{};
        const auto count = std::snprintf(row, sizeof(row), "%llu,%llu,%llu,%llu,%ld,%u,%u,%u\n",
            static_cast<unsigned long long>(now), static_cast<unsigned long long>(telemetry.ok),
            static_cast<unsigned long long>(telemetry.missing), static_cast<unsigned long long>(telemetry.errors),
            static_cast<long>(result), token, width, height);
        if (count > 0 && static_cast<std::size_t>(count) < sizeof(row)) {
            DWORD written{};
            if (WriteFile(telemetry.file, row, static_cast<DWORD>(count), &written, nullptr))
                telemetry.written += written;
        }
    } catch (...) { } // Observation never changes the decoder result.
}
struct ReadReport {
    std::uint32_t* token{};
    HRESULT result{E_UNEXPECTED};
    UINT width{}, height{};
    ~ReadReport() { report_read(result, token ? *token : 0, width, height); }
    HRESULT finish(HRESULT value) noexcept { result = value; return value; }
};
} // namespace

void configure_luke_color_origin_extent(unsigned width, unsigned height) noexcept {
    g_color_extent.store(extent_bits(width, height), std::memory_order_release);
}

// Bounded changed-geometry evidence only. Admission and marker layout unchanged.
static void report_origin_layout(bool writer, unsigned reason,
    std::uint64_t output, std::uint64_t eye) noexcept {
    try {
        struct State { std::uint64_t output{},eye{}; unsigned reason{},count{}; };
        static std::mutex mutex;
        static std::array<State,2> states{};
        std::scoped_lock lock(mutex);
        auto& state=states[writer?0:1];
        if(state.count>=16||(state.count&&state.output==output&&state.eye==eye&&state.reason==reason))return;
        state.output=output;state.eye=eye;state.reason=reason;++state.count;
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            reason,writer?948:949,output,eye);
    } catch (...) { }
}

HRESULT mark_luke_color_origin(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
    std::uint32_t token, D3D12_RESOURCE_STATES state, ID3D12Device* native_device,
    LukeColorOriginRect rect) noexcept {
    try {
        if (!list || !color || !token) return E_INVALIDARG;
        const auto desc = color->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
            desc.DepthOrArraySize != 1 || desc.Width > 16384 || !luke_origin_pixel_bytes(desc.Format))
            return DXGI_ERROR_UNSUPPORTED;
        if (!rect.width) rect.width = static_cast<UINT>(desc.Width);
        if (!rect.height) rect.height = desc.Height;
        if (rect.width < kLukeOriginWidth || rect.height > 8192 ||
            static_cast<UINT64>(rect.x) + rect.width > desc.Width ||
            static_cast<UINT64>(rect.y) + rect.height > desc.Height) return E_INVALIDARG;
        g_output_extent.store(extent_bits(rect.width, rect.height), std::memory_order_release);
        const auto color_extent = g_color_extent.load(std::memory_order_acquire);
        if (!color_extent) {
            report_origin_layout(true,1,extent_bits(rect.width,rect.height),0);
            return E_PENDING; // Actual eye viewport is not known during startup.
        }
        LukeColorOriginLayout layout;
        if (!compact_luke_origin_layout(rect.width, rect.height,
                static_cast<UINT>(color_extent >> 32), static_cast<UINT>(color_extent), layout))
        {
            report_origin_layout(true,2,extent_bits(rect.width,rect.height),color_extent);
            return E_PENDING;
        }
        ComPtr<ID3D12Device> device = native_device;
        if (!device) {
            const HRESULT result = list->GetDevice(IID_PPV_ARGS(device.GetAddressOf()));
            if (FAILED(result)) return result;
        }
        std::shared_ptr<Uploads> patterns;
        HRESULT result = get_uploads(device.Get(), desc.Format, kLukeOriginHeight, patterns);
        if (FAILED(result)) return result;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = color; destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        if (state != D3D12_RESOURCE_STATE_COPY_DEST) {
            auto barrier = transition(color, state, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &barrier);
        }
        for(unsigned band=0;band<layout.band_count;++band) {
            const UINT x=rect.x+layout.bands[band].x,y=rect.y+layout.bands[band].y;
            for (unsigned cell = 0; cell < kLukeOriginBits * 2; ++cell) {
            const bool white = luke_origin_white(token, cell * 2);
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = patterns->colors[white ? 1 : 0].Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Footprint = {desc.Format, 2, kLukeOriginHeight, 1, 256};
                list->CopyTextureRegion(&destination, x + cell * 2, y, 0, &source, nullptr);
            }
        }
        if (state != D3D12_RESOURCE_STATE_COPY_DEST) {
            auto barrier = transition(color, D3D12_RESOURCE_STATE_COPY_DEST, state);
            list->ResourceBarrier(1, &barrier);
        }
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT read_luke_color_origin(ID3D12CommandQueue* queue, ID3D12Resource* color,
    std::uint32_t* token, D3D12_RESOURCE_STATES state, DWORD timeout_ms) noexcept {
    if (token) *token = 0;
    ReadReport report{token};
    try {
        if (!queue || !color || !token) return report.finish(E_POINTER);
        const auto original = color->GetDesc();
        if (original.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || original.SampleDesc.Count != 1 ||
            original.DepthOrArraySize != 1 || original.Width > 16384 || original.Width < kLukeOriginWidth ||
            !original.Height || !luke_origin_pixel_bytes(original.Format))
            return report.finish(DXGI_ERROR_UNSUPPORTED);
        configure_luke_color_origin_extent(static_cast<UINT>(original.Width), original.Height);
        const auto output_extent = g_output_extent.load(std::memory_order_acquire);
        if (!output_extent) {
            report_origin_layout(false,1,0,extent_bits(static_cast<UINT>(original.Width),original.Height));
            return report.finish(E_PENDING);
        }
        LukeColorOriginLayout layout;
        if (!compact_luke_origin_layout(static_cast<UINT>(output_extent >> 32),
                static_cast<UINT>(output_extent), static_cast<UINT>(original.Width),
                original.Height, layout)) {
            report_origin_layout(false,2,output_extent,extent_bits(static_cast<UINT>(original.Width),original.Height));
            return report.finish(E_PENDING);
        }
        report.width = layout.scan.width; report.height = layout.scan.height;
        std::shared_ptr<Reader> reader;
        HRESULT result = get_reader(queue, reader);
        if (FAILED(result)) return report.finish(result);
        std::scoped_lock lock(reader->mutex);
        result = wait_reader(*reader, timeout_ms);
        if (FAILED(result)) return report.finish(result); // Retire before any allocator/readback reuse.
        auto roi_desc = original; roi_desc.Width = layout.scan.width;
        roi_desc.Height = layout.scan.height; roi_desc.MipLevels = 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes{};
        reader->device->GetCopyableFootprints(&roi_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        if (!reader->readback || reader->bytes != bytes) {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> buffer;
            result = reader->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(buffer.GetAddressOf()));
            if (FAILED(result)) return report.finish(result);
            reader->readback = std::move(buffer); reader->bytes = bytes;
        }
        result = reader->allocator->Reset();
        if (SUCCEEDED(result)) result = reader->list->Reset(reader->allocator.Get(), nullptr);
        if (FAILED(result)) return report.finish(result);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = color; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = reader->readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            auto barrier = transition(color, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            reader->list->ResourceBarrier(1, &barrier);
        }
        const D3D12_BOX box{0, 0, 0, layout.scan.width, layout.scan.height, 1};
        reader->list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            auto barrier = transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            reader->list->ResourceBarrier(1, &barrier);
        }
        result = reader->list->Close();
        if (FAILED(result)) return report.finish(result);
        ID3D12CommandList* lists[]{reader->list.Get()}; queue->ExecuteCommandLists(1, lists);
        const auto value = reader->next_fence++;
        result = queue->Signal(reader->fence.Get(), value);
        if (FAILED(result)) { reader->poisoned = true; return report.finish(result); }
        reader->last_fence = value;
        // RAM only: correlate each phase by (reader address, fence value).
        // 123->124 is queued GPU readiness; 124->125 is Map/luma/CRC decode.
        const bool ram_reader_timing = bridge_flight_logger().ram_enabled();
        if (ram_reader_timing)
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                result, 123, reinterpret_cast<std::uint64_t>(reader.get()), value);
        result = wait_reader(*reader, timeout_ms);
        if (ram_reader_timing)
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                result, 124, reinterpret_cast<std::uint64_t>(reader.get()), value);
        if (FAILED(result)) return report.finish(result);
        std::vector<float> row(layout.scan.width);
        void* mapped{}; const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
        result = reader->readback->Map(0, &range, &mapped);
        if (FAILED(result)) return report.finish(result);
        const auto pixel_bytes = luke_origin_pixel_bytes(original.Format);
        std::uint32_t exact_token{}; bool ambiguous = false;
        for (UINT y = 0; y < layout.scan.height; ++y) {
            const auto* pixels = static_cast<const std::byte*>(mapped) + footprint.Offset +
                static_cast<UINT64>(y) * footprint.Footprint.RowPitch;
            for (UINT x = 0; x < layout.scan.width; ++x)
                row[x] = luke_origin_luma(pixels + x * pixel_bytes, original.Format);
            const auto decoded = decode_luke_origin_row(row);
            ambiguous |= decoded.ambiguous;
            if (!decoded.token) continue;
            if (exact_token && exact_token != decoded.token) ambiguous = true;
            exact_token = decoded.token;
        }
        const D3D12_RANGE no_write{0, 0}; reader->readback->Unmap(0, &no_write);
        const HRESULT decode_status = ambiguous ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA)
            : exact_token ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (ram_reader_timing)
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                decode_status, 125, reinterpret_cast<std::uint64_t>(reader.get()), value);
        if (ambiguous) return report.finish(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        if (!exact_token) return report.finish(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        *token = exact_token;
        return report.finish(S_OK);
    } catch (const std::bad_alloc&) { return report.finish(E_OUTOFMEMORY); }
      catch (...) { return report.finish(E_FAIL); }
}
} // namespace xrfg
