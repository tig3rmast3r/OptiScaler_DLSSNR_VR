#include "xrfg/two_frame_capture.hpp"


#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::uint64_t kMaximumBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumImages = 48;

struct Shot {
    TwoFrameImageInfo info{};
    std::uint32_t frame{};
    D3D12_RESOURCE_DESC description{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    std::uint64_t row_bytes{};
    std::uint64_t bytes{};
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandList> producer_list;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t fence_value{};
    HRESULT status{S_OK};
};

struct ContractShot {
    TwoFrameGuideContract info{};
    std::uint32_t frame{};
};

struct TimelineShot {
    std::uint32_t frame{};
    const char* stream{};
    std::int64_t code{};
    std::uint64_t serial{};
    std::uint64_t display_time{};
    std::uint64_t detail{};
};

struct Session {
    std::mutex mutex;
    std::vector<std::shared_ptr<Shot>> shots;
    std::vector<ContractShot> contracts;
    std::vector<TimelineShot> timeline;
    std::filesystem::path directory;
    std::uint64_t allocated{};
    std::uint32_t dropped_images{};
    bool sealed{};
    std::atomic_bool write_finished{false};
    std::atomic_bool safe_to_release{true};
};

struct Manager {
    std::mutex mutex;
    std::shared_ptr<Session> session;
    std::atomic_uint32_t active_frame{};
    bool pending_start{};
    bool key_down{};
    bool afw_completed_frame{};
    std::uint32_t observed_end_frames{};
    std::thread writer;

    ~Manager() {
        if (writer.joinable()) writer.join();
    }
};

Manager& manager() {
    static Manager value;
    return value;
}

DXGI_FORMAT typed_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}

D3D12_RESOURCE_BARRIER barrier(
    ID3D12Resource* resource, UINT subresource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER value{};
    value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    value.Transition.pResource = resource;
    value.Transition.Subresource = subresource;
    value.Transition.StateBefore = before;
    value.Transition.StateAfter = after;
    return value;
}

ComPtr<IUnknown> identity(IUnknown* object) noexcept {
    ComPtr<IUnknown> result;
    if (object) object->QueryInterface(IID_PPV_ARGS(result.GetAddressOf()));
    return result;
}

std::filesystem::path capture_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    const auto root = length && length < path.size()
        ? std::filesystem::path(path.data()).parent_path()
        : std::filesystem::temp_directory_path();
    SYSTEMTIME time{};
    GetLocalTime(&time);
    static std::atomic_uint32_t sequence{0};
    wchar_t name[128]{};
    swprintf_s(name, L"ofxr-capture-v23286-%lu-%04u%02u%02u-%02u%02u%02u-%03u-%u",
        GetCurrentProcessId(), time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
        ++sequence);
    return root / name;
}

std::shared_ptr<Shot> prepare_shot(
    const std::shared_ptr<Session>& session, std::uint32_t frame,
    const TwoFrameImageInfo& info, ID3D12Device* device,
    ID3D12Resource* source) {
    if (!session || !device || !source || !info.stage) return {};
    auto shot = std::make_shared<Shot>();
    shot->info = info;
    shot->frame = frame;
    shot->description = source->GetDesc();
    const auto& desc = shot->description;
    const auto format = typed_format(desc.Format);
    const bool supported_format =
        format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
        format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        format == DXGI_FORMAT_R32G32_FLOAT ||
        format == DXGI_FORMAT_R10G10B10A2_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_R16G16_FLOAT ||
        format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
        format == DXGI_FORMAT_R24G8_TYPELESS ||
        format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
        format == DXGI_FORMAT_D32_FLOAT ||
        format == DXGI_FORMAT_R32_FLOAT ||
        format == DXGI_FORMAT_R8G8_UNORM ||
        format == DXGI_FORMAT_R16_FLOAT ||
        format == DXGI_FORMAT_R16_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1 || desc.MipLevels == 0 ||
        info.slice >= desc.DepthOrArraySize || !supported_format) {
        shot->status = E_INVALIDARG;
    } else {
        D3D12_RESOURCE_DESC typed = desc;
        typed.Format = format;
        const UINT subresource = info.slice * desc.MipLevels;
        device->GetCopyableFootprints(&typed, subresource, 1, 0,
            &shot->footprint, nullptr, &shot->row_bytes, &shot->bytes);
        if (shot->bytes == 0 || shot->bytes > 128ULL * 1024ULL * 1024ULL) {
            shot->status = E_OUTOFMEMORY;
        } else {
            std::scoped_lock lock(session->mutex);
            if (session->allocated + shot->bytes > kMaximumBytes) {
                shot->status = E_OUTOFMEMORY;
            } else {
                session->allocated += shot->bytes;
            }
        }
        if (SUCCEEDED(shot->status)) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = shot->bytes;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            shot->status = device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(shot->readback.GetAddressOf()));
        }
    }
    {
        std::scoped_lock lock(session->mutex);
        if (!session->sealed && session->shots.size() < kMaximumImages)
            session->shots.push_back(shot);
        else {
            ++session->dropped_images;
            return {};
        }
    }
    return shot;
}

void record_copy(
    Shot& shot, ID3D12GraphicsCommandList* list,
    ID3D12Resource* source, D3D12_RESOURCE_STATES source_state) noexcept {
    if (FAILED(shot.status) || !list || !source) return;
    const UINT subresource = shot.info.slice * shot.description.MipLevels;
    const UINT barrier_subresource =
        (shot.description.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
         shot.description.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
         shot.description.Format == DXGI_FORMAT_R24G8_TYPELESS)
            ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : subresource;
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        const auto before = barrier(source, barrier_subresource, source_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &before);
    }
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = shot.readback.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = shot.footprint;
    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = source;
    origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    origin.SubresourceIndex = subresource;
    list->CopyTextureRegion(&target, 0, 0, 0, &origin, nullptr);
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        const auto after = barrier(source, barrier_subresource,
            D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
        list->ResourceBarrier(1, &after);
    }
}

bool completed(const Shot& shot) noexcept {
    return shot.fence && shot.fence_value &&
        shot.fence->GetCompletedValue() >= shot.fence_value;
}

void write_session(const std::shared_ptr<Session>& session) {
    std::error_code ec;
    std::filesystem::create_directories(session->directory, ec);
    std::ofstream manifest(session->directory / L"manifest.tsv", std::ios::binary);
    manifest << "index\tframe\tstage\tstatus\tstream\tdisplay_time\tswapchain\tserial\teye\tslice\tx\ty\twidth\theight\tresource_width\tresource_height\tformat\tsource_format\trow_pitch\trow_bytes\tbytes\tscale_x\tscale_y\tjitter_x\tjitter_y\tfile\n";
    std::vector<std::shared_ptr<Shot>> shots;
    std::vector<ContractShot> contracts;
    std::vector<TimelineShot> timeline;
    {
        std::scoped_lock lock(session->mutex);
        shots = session->shots;
        contracts = session->contracts;
        timeline = session->timeline;
    }
    std::ofstream timeline_manifest(
        session->directory / L"xr-timeline.tsv", std::ios::binary);
    timeline_manifest << "frame\tstream\tcode\tserial\tdisplay_time\tdetail\n";
    for (const TimelineShot& shot : timeline) {
        timeline_manifest << shot.frame << '\t' << shot.stream << '\t'
            << shot.code << '\t' << shot.serial << '\t'
            << shot.display_time << '\t' << shot.detail << '\n';
    }
    std::ofstream guide_manifest(session->directory / L"guide-contracts.tsv",
        std::ios::binary);
    guide_manifest << "frame\tstage\tstream\tpublication\tserial\tprevious_serial"
        "\tsource_eye\tdestination_eye\tmode"
        "\tmotion_vectors_type\tignore_motion_threshold"
        "\tcamera_fov_vertical\tdebug\tclear_before_warping"
        "\tis_hudless_color\tdepth_dilate_mode"
        "\tmotion_width\tmotion_height"
        "\tdepth_width\tdepth_height\toutput_width\toutput_height"
        "\tinput_color_width\tinput_color_height\tinput_depth_width"
        "\tinput_depth_height\tinput_motion_width\tinput_motion_height"
        "\toutput_color_width\toutput_color_height\tdepth_crop_x"
        "\tdepth_crop_y\tmotion_crop_x\tmotion_crop_y"
        "\tscale_x\tscale_y\tjitter_x\tjitter_y\tmotion_scale_x"
        "\tmotion_scale_y\tframe_time_delta_ms\tcamera_near\tcamera_far"
        "\treset\tjittered\tdepth_inverted\tdepth_infinite"
        "\tcamera_position\tcamera_right\tcamera_up\tcamera_forward"
        "\tsource_pose\tdestination_pose"
        "\tsource_orientation\tdestination_orientation"
        "\tview_to_clip\tclip_to_view"
        "\tsource_world_to_view\tsource_view_to_world"
        "\tsource_view_to_clip\tsource_clip_to_view"
        "\tdestination_world_to_view\tdestination_view_to_world"
        "\tdestination_view_to_clip\tdestination_clip_to_view"
        "\tcamera_world_to_view\tcamera_view_to_world"
        "\tcamera_view_to_clip\tcamera_clip_to_view\n";
    const auto write_array = [&guide_manifest](const auto& values) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) guide_manifest << ',';
            guide_manifest << values[i];
        }
    };
    guide_manifest << std::setprecision(9);
    for (const auto& shot : contracts) {
        const auto& c = shot.info;
        guide_manifest << shot.frame << '\t' << c.stage << '\t'
            << c.stream << '\t' << c.publication << '\t' << c.serial
            << '\t' << c.previous_serial << '\t' << c.source_eye << '\t'
            << c.destination_eye << '\t' << c.mode << '\t'
            << c.motion_vectors_type << '\t'
            << c.ignore_motion_threshold << '\t'
            << c.camera_fov_vertical << '\t' << c.debug << '\t'
            << c.clear_before_warping << '\t' << c.is_hudless_color << '\t'
            << c.depth_dilate_mode << '\t'
            << c.motion_width << '\t' << c.motion_height << '\t'
            << c.depth_width << '\t' << c.depth_height << '\t'
            << c.output_width << '\t' << c.output_height << '\t'
            << c.input_color_width << '\t' << c.input_color_height << '\t'
            << c.input_depth_width << '\t' << c.input_depth_height << '\t'
            << c.input_motion_width << '\t' << c.input_motion_height << '\t'
            << c.output_color_width << '\t' << c.output_color_height << '\t'
            << c.depth_crop_x << '\t' << c.depth_crop_y << '\t'
            << c.motion_crop_x << '\t' << c.motion_crop_y << '\t'
            << c.scale_x << '\t' << c.scale_y << '\t'
            << c.jitter_x << '\t' << c.jitter_y << '\t'
            << c.motion_scale_x << '\t' << c.motion_scale_y << '\t'
            << c.frame_time_delta_ms << '\t' << c.camera_near << '\t'
            << c.camera_far << '\t' << c.reset << '\t' << c.jittered
            << '\t' << c.depth_inverted << '\t' << c.depth_infinite << '\t';
        write_array(c.camera_position); guide_manifest << '\t';
        write_array(c.camera_right); guide_manifest << '\t';
        write_array(c.camera_up); guide_manifest << '\t';
        write_array(c.camera_forward); guide_manifest << '\t';
        write_array(c.source_pose); guide_manifest << '\t';
        write_array(c.destination_pose); guide_manifest << '\t';
        write_array(c.source_orientation); guide_manifest << '\t';
        write_array(c.destination_orientation); guide_manifest << '\t';
        write_array(c.view_to_clip); guide_manifest << '\t';
        write_array(c.clip_to_view); guide_manifest << '\t';
        write_array(c.source_world_to_view); guide_manifest << '\t';
        write_array(c.source_view_to_world); guide_manifest << '\t';
        write_array(c.source_view_to_clip); guide_manifest << '\t';
        write_array(c.source_clip_to_view); guide_manifest << '\t';
        write_array(c.destination_world_to_view); guide_manifest << '\t';
        write_array(c.destination_view_to_world); guide_manifest << '\t';
        write_array(c.destination_view_to_clip); guide_manifest << '\t';
        write_array(c.destination_clip_to_view); guide_manifest << '\t';
        write_array(c.camera_world_to_view); guide_manifest << '\t';
        write_array(c.camera_view_to_world); guide_manifest << '\t';
        write_array(c.camera_view_to_clip); guide_manifest << '\t';
        write_array(c.camera_clip_to_view); guide_manifest << '\n';
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    for (std::size_t index = 0; index < shots.size(); ++index) {
        const auto& shot = *shots[index];
        HRESULT status{};
        {
            std::scoped_lock lock(session->mutex);
            status = shot.status;
        }
        if (SUCCEEDED(status)) {
            bool ready = false;
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::scoped_lock lock(session->mutex);
                    status = shot.status;
                    ready = completed(shot);
                }
                if (ready || FAILED(status)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!ready && SUCCEEDED(status)) {
                status = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                session->safe_to_release.store(false);
            }
        }
        std::string file;
        if (SUCCEEDED(status)) {
            void* data = nullptr;
            const D3D12_RANGE read_range{0, static_cast<SIZE_T>(shot.bytes)};
            status = shot.readback->Map(0, &read_range, &data);
            if (SUCCEEDED(status) && data) {
                file = "image-" + std::to_string(index) + ".raw";
                std::ofstream raw(session->directory / file, std::ios::binary);
                raw.write(static_cast<const char*>(data),
                    static_cast<std::streamsize>(shot.bytes));
                if (!raw) status = E_FAIL;
                const D3D12_RANGE written{0, 0};
                shot.readback->Unmap(0, &written);
            } else if (SUCCEEDED(status)) {
                status = E_FAIL;
            }
        }
        manifest << index << '\t' << shot.frame << '\t' << shot.info.stage
            << '\t' << std::hex << static_cast<std::uint32_t>(status) << std::dec
            << '\t' << shot.info.stream << '\t' << shot.info.display_time
            << '\t' << shot.info.swapchain << '\t' << shot.info.capture_serial
            << '\t' << shot.info.eye << '\t' << shot.info.slice
            << '\t' << shot.info.x << '\t' << shot.info.y
            << '\t' << shot.info.width << '\t' << shot.info.height
            << '\t' << shot.description.Width << '\t' << shot.description.Height
            << '\t' << static_cast<unsigned>(shot.footprint.Footprint.Format)
            << '\t' << static_cast<unsigned>(shot.description.Format)
            << '\t' << shot.footprint.Footprint.RowPitch
            << '\t' << shot.row_bytes << '\t' << shot.bytes
            << '\t' << shot.info.scale_x << '\t' << shot.info.scale_y
            << '\t' << shot.info.jitter_x << '\t' << shot.info.jitter_y
            << '\t' << file << '\n';
    }
    const std::string message = "OFXR V23286 capture: " +
        session->directory.string() + "\n";
    std::ofstream summary(session->directory / L"README.txt", std::ios::binary);
    summary << "V23286 AFW/OFXR frame-slot and projection-pose audit.\n"
        << "Trigger: F10; capture ends after two AFW evaluations or eight XR frames.\n"
        << "NGX source, AFW inputs, OpenXR eyes, camera matrices and xr-timeline.tsv.\n"
        << "Raw images include D3D12 row padding; see manifest.tsv.\n"
        << "Max images=48, max total readback=1 GiB. Dropped images="
        << session->dropped_images << "\n";
    OutputDebugStringA(message.c_str());
    session->write_finished.store(true);
}

}  // namespace

std::uint32_t two_frame_capture_begin_end_frame() noexcept {
    try {
        auto& state = manager();
        std::scoped_lock lock(state.mutex);
        const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const bool pressed = down && !state.key_down;
        state.key_down = down;
        if (state.session && state.session->write_finished.load() &&
            state.session->safe_to_release.load()) {
            if (state.writer.joinable()) state.writer.join();
            state.session.reset();
        }
        if (!state.session && pressed) {
            if (state.writer.joinable()) state.writer.join();
            state.session = std::make_shared<Session>();
            state.session->directory = capture_directory();
            state.pending_start = true;
            state.afw_completed_frame = false;
            state.observed_end_frames = 0;
            return 0;
        }
        return state.active_frame;
    } catch (...) {
        return 0;
    }
}

void two_frame_capture_finish_end_frame(std::uint32_t frame) noexcept {
    try {
        auto& state = manager();
        std::scoped_lock lock(state.mutex);
        if (frame == 0) {
            if (state.pending_start) {
                state.active_frame = 1;
                state.pending_start = false;
            }
            return;
        }
        if (!state.session || state.active_frame != frame) return;
        ++state.observed_end_frames;
        const bool completed = state.afw_completed_frame;
        state.afw_completed_frame = false;
        if (completed && frame == 1) {
            state.active_frame = frame + 1;
            return;
        }
        if (!completed && state.observed_end_frames < 8) return;
        state.active_frame = 0;
        const auto session = state.session;
        {
            std::scoped_lock session_lock(session->mutex);
            session->sealed = true;
        }
        state.writer = std::thread([session] {
            try {
                write_session(session);
            } catch (...) {
                session->safe_to_release.store(false);
                session->write_finished.store(true);
            }
        });
        // Keep the session available to a late producer submission. It is
        // retired on a later xrEndFrame only after the writer has finished.
    } catch (...) {
    }
}

void two_frame_capture_mark_afw_evaluation() noexcept {
    try {
        auto& state = manager();
        std::scoped_lock lock(state.mutex);
        if (state.session && state.active_frame != 0)
            state.afw_completed_frame = true;
    } catch (...) {
    }
}

bool two_frame_capture_active() noexcept {
    return manager().active_frame.load() != 0;
}

void two_frame_capture_timeline(
    const char* stream,
    std::int64_t code,
    std::uint64_t serial,
    std::uint64_t display_time,
    std::uint64_t detail) noexcept {
    if (stream == nullptr) return;
    try {
        auto& state = manager();
        std::scoped_lock lock(state.mutex);
        const std::uint32_t frame = state.active_frame.load();
        if (!frame || !state.session) return;
        std::scoped_lock session_lock(state.session->mutex);
        if (!state.session->sealed && state.session->timeline.size() < 512) {
            state.session->timeline.push_back({
                frame, stream, code, serial, display_time, detail});
        }
    } catch (...) {
    }
}

void two_frame_capture_guide_contract(
    const TwoFrameGuideContract& info) noexcept {
    if (!info.stage) return;
    try {
        auto& state = manager();
        std::scoped_lock lock(state.mutex);
        const auto frame = state.active_frame.load();
        if (!frame || !state.session) return;
        std::scoped_lock session_lock(state.session->mutex);
        if (!state.session->sealed && state.session->contracts.size() < 32)
            state.session->contracts.push_back({info, frame});
    } catch (...) {
    }
}

void two_frame_capture_note(
    const TwoFrameImageInfo& info, HRESULT status) noexcept {
    if (!info.stage || SUCCEEDED(status)) return;
    try {
        std::shared_ptr<Session> session;
        std::uint32_t frame{};
        {
            auto& state = manager();
            std::scoped_lock lock(state.mutex);
            session = state.session;
            frame = state.active_frame;
        }
        if (!session || !frame) return;
        auto shot = std::make_shared<Shot>();
        shot->info = info;
        shot->frame = frame;
        shot->status = status;
        std::scoped_lock lock(session->mutex);
        if (!session->sealed && session->shots.size() < kMaximumImages)
            session->shots.push_back(std::move(shot));
        else
            ++session->dropped_images;
    } catch (...) {
    }
}

void two_frame_capture_record_ng_x(
    const TwoFrameImageInfo& info, ID3D12Device* device,
    ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state) noexcept {
    try {
        std::shared_ptr<Session> session;
        std::uint32_t frame{};
        {
            auto& state = manager();
            std::scoped_lock lock(state.mutex);
            session = state.session;
            frame = state.active_frame;
        }
        if (!session || !frame || !list) return;
        auto shot = prepare_shot(session, frame, info, device, source);
        if (!shot) return;
        record_copy(*shot, list, source, source_state);
        if (SUCCEEDED(shot->status)) shot->producer_list = list;
    } catch (...) {
    }
}

void two_frame_capture_submission(
    ID3D12CommandQueue* queue, std::uint32_t count,
    ID3D12CommandList* const* lists) noexcept {
    if (!queue || !lists || count == 0) return;
    try {
        std::shared_ptr<Session> session;
        {
            auto& state = manager();
            std::scoped_lock lock(state.mutex);
            session = state.session;
        }
        if (!session) return;
        std::scoped_lock lock(session->mutex);
        for (const auto& shot : session->shots) {
            if (!shot->producer_list || FAILED(shot->status) || shot->fence) continue;
            const auto producer = identity(shot->producer_list.Get());
            bool match = false;
            for (std::uint32_t index = 0; index < count; ++index) {
                if (producer && producer.Get() == identity(lists[index]).Get()) {
                    match = true;
                    break;
                }
            }
            if (!match) continue;
            ComPtr<ID3D12Device> device;
            shot->status = queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()));
            if (SUCCEEDED(shot->status))
                shot->status = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(shot->fence.GetAddressOf()));
            if (SUCCEEDED(shot->status))
                shot->status = queue->Signal(shot->fence.Get(), 1);
            if (SUCCEEDED(shot->status)) shot->fence_value = 1;
            else session->safe_to_release.store(false);
            shot->producer_list.Reset();
        }
    } catch (...) {
    }
}

void two_frame_capture_annotate_projection(
    std::uint32_t frame, std::uint64_t swapchain,
    std::uint64_t capture_serial, std::uint64_t display_time,
    std::int32_t eye, std::uint32_t slice,
    std::uint32_t x, std::uint32_t y,
    std::uint32_t width, std::uint32_t height) noexcept {
    try {
        std::shared_ptr<Session> session;
        {
            auto& state = manager();
            std::scoped_lock lock(state.mutex);
            session = state.session;
        }
        if (!session) return;
        std::scoped_lock lock(session->mutex);
        for (const auto& shot : session->shots) {
            if (shot->frame != frame ||
                shot->info.swapchain != swapchain ||
                shot->info.capture_serial != capture_serial ||
                std::string_view(shot->info.stage) != "xr_final_eye") continue;
            shot->info.display_time = display_time;
            shot->info.eye = eye;
            shot->info.slice = slice;
            shot->info.x = x;
            shot->info.y = y;
            shot->info.width = width;
            shot->info.height = height;
        }
    } catch (...) {
    }
}
}  // namespace xrfg
