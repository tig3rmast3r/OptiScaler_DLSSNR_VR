#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/luke_camera_units.hpp"
#include "xrfg/luke_color_token.hpp"
#include "xrfg/two_frame_capture.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <deque>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint64_t kMaximumStereoPublicationGap = 4;
constexpr std::size_t kSnapshotSlotCount = 16;
// Leave two protected slots free for producer/consumer overlap. Stored frames
// have no previous-guide chains: those pins are attached only to a resolved join.
// Keep exact delayed colors AND their N-1 guides: OFXR can make the color
// arrive 5-6 publications behind production. Retention is not a pacing delay;
// snapshots remain immutable and CPU/GPU readers still prevent slot reuse.
constexpr std::size_t kLukeOriginHistoryCount = kSnapshotSlotCount - 2;
constexpr D3D12_RESOURCE_STATES kSnapshotReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

struct SnapshotSlot {
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Device> allocation_device;
    D3D12_RESOURCE_DESC description{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    std::shared_ptr<DlssSnapshotRetention> retention;
};

struct StreamState {
    LukeCameraUnitCalibration camera_units;
    std::uint64_t epoch{1};
    std::uint64_t serial{};
    std::uint64_t first_publication{};
    float jitter_x{};
    float jitter_y{};
    std::shared_ptr<const DlssMotionVectorFrame> previous;
    std::shared_ptr<const DlssMotionVectorFrame> previous2;
    std::shared_ptr<const DlssMotionVectorFrame> latest;
    std::deque<std::shared_ptr<const DlssMotionVectorFrame>> luke_origin_history;
    // Used only by an explicitly requested output diagnostic. Normal FSR
    // consumes the bridge-owned final XR color and allocates no output ring.
    std::array<SnapshotSlot, kSnapshotSlotCount> output_snapshots;
    std::size_t next_output_snapshot{};
    std::array<SnapshotSlot, kSnapshotSlotCount> snapshots;
    std::size_t next_snapshot{};
    std::array<SnapshotSlot, kSnapshotSlotCount> depth_snapshots;
    std::size_t next_depth_snapshot{};
    ComPtr<ID3D12Fence> submission_fence;
    ComPtr<ID3D12Device> submission_fence_device;
    std::uint64_t next_submission_fence_value{1};
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, StreamState> streams;
    // A retired stream must disappear from resolution immediately, but its
    // snapshot resources can still be referenced by command lists that have
    // only been recorded. Keep those allocations alive until the layer's
    // existing GPU-idle retirement boundary.
    std::vector<StreamState> retired_streams;
    DlssMotionVectorStatistics statistics;
    // Tracking toggles and stream resets cannot reuse a color-carried key.
    std::uint64_t next_publication{1};
    // Unlike the six-guide history, this sequence survives every clear/epoch.
    LukeColorTokenAllocator color_origin_tokens;
};

Registry& registry() {
    static Registry value;
    return value;
}

std::atomic_bool g_tracking_enabled{false};

ComPtr<IUnknown> identity(IUnknown* object) noexcept {
    ComPtr<IUnknown> value;
    if (object != nullptr) {
        static_cast<void>(object->QueryInterface(IID_PPV_ARGS(value.GetAddressOf())));
    }
    return value;
}

bool same_identity(IUnknown* left, IUnknown* right) noexcept {
    const auto a = identity(left);
    const auto b = identity(right);
    return a && b && a.Get() == b.Get();
}

bool same_device(IUnknown* left, IUnknown* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    ComPtr<ID3D12DeviceChild> left_child;
    ComPtr<ID3D12DeviceChild> right_child;
    ComPtr<ID3D12Device> left_device;
    ComPtr<ID3D12Device> right_device;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(left_child.GetAddressOf()))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(right_child.GetAddressOf()))) &&
        SUCCEEDED(left_child->GetDevice(IID_PPV_ARGS(left_device.GetAddressOf()))) &&
        SUCCEEDED(right_child->GetDevice(IID_PPV_ARGS(right_device.GetAddressOf()))) &&
        same_identity(left_device.Get(), right_device.Get());
}

bool valid(const DlssMotionVectorPublication& publication) noexcept {
    if (publication.stream == 0 || publication.output == nullptr ||
        publication.motion_vectors == nullptr ||
        publication.producer_queue == nullptr ||
        publication.producer_command_list == nullptr || publication.output_width == 0 ||
        publication.output_height == 0 || publication.motion_width == 0 ||
        publication.motion_height == 0 || !std::isfinite(publication.scale_x) ||
        !std::isfinite(publication.scale_y) || !std::isfinite(publication.jitter_x) ||
        !std::isfinite(publication.jitter_y)) {
        return false;
    }
    const auto output = publication.output->GetDesc();
    const auto motion = publication.motion_vectors->GetDesc();
    const bool base_valid = output.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        motion.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        static_cast<std::uint64_t>(publication.output_x) + publication.output_width <=
            output.Width &&
        static_cast<std::uint64_t>(publication.output_y) + publication.output_height <=
            output.Height &&
        static_cast<std::uint64_t>(publication.motion_x) + publication.motion_width <=
            motion.Width &&
        static_cast<std::uint64_t>(publication.motion_y) + publication.motion_height <=
            motion.Height;
    if (!base_valid || publication.depth == nullptr) return base_valid;
    const auto depth = publication.depth->GetDesc();
    return depth.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        publication.depth_width != 0 && publication.depth_height != 0 &&
        static_cast<std::uint64_t>(publication.depth_x) + publication.depth_width <=
            depth.Width &&
        static_cast<std::uint64_t>(publication.depth_y) + publication.depth_height <=
            depth.Height &&
        std::isfinite(publication.frame_time_delta_ms) &&
        publication.frame_time_delta_ms > 0.0F &&
        std::isfinite(publication.camera_near) &&
        std::isfinite(publication.camera_far);
}

bool matching_description(
    const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    return left.Dimension == right.Dimension && left.Alignment == right.Alignment &&
        left.Width == right.Width && left.Height == right.Height &&
        left.DepthOrArraySize == right.DepthOrArraySize &&
        left.MipLevels == right.MipLevels && left.Format == right.Format &&
        left.SampleDesc.Count == right.SampleDesc.Count &&
        left.SampleDesc.Quality == right.SampleDesc.Quality &&
        left.Layout == right.Layout && left.Flags == right.Flags;
}

D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

template <std::size_t SnapshotCount>
HRESULT snapshot_resource(
    std::array<SnapshotSlot, SnapshotCount>& snapshots,
    std::size_t& next_snapshot,
    ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state,
    ID3D12GraphicsCommandList* producer_command_list,
    ID3D12CommandQueue* producer_queue,
    ID3D12Device* verified_producer_device,
    ComPtr<ID3D12Resource>* snapshot,
    D3D12_RESOURCE_STATES snapshot_state = kSnapshotReadState,
    std::shared_ptr<DlssSnapshotRetention>* retention = nullptr,
    ComPtr<ID3D12Device>* allocation_receipt = nullptr) noexcept {
    if (source == nullptr || producer_command_list == nullptr ||
        producer_queue == nullptr || snapshot == nullptr) return E_POINTER;
    snapshot->Reset();
    if (allocation_receipt) allocation_receipt->Reset();

    ComPtr<ID3D12Device> source_device;
    ComPtr<ID3D12Device> command_device;
    ComPtr<ID3D12Device> queue_device;
    HRESULT result = source->GetDevice(
        IID_PPV_ARGS(source_device.GetAddressOf()));
    if (FAILED(result)) return result;
    result = producer_command_list->GetDevice(
        IID_PPV_ARGS(command_device.GetAddressOf()));
    if (FAILED(result)) return result;
    result = producer_queue->GetDevice(
        IID_PPV_ARGS(queue_device.GetAddressOf()));
    if (FAILED(result)) return result;
    ComPtr<ID3D12Device> allocation_device;
    if (verified_producer_device != nullptr) {
        // The in-process producer has already unwrapped and identity-checked
        // the devices returned by every resource/list/queue. Comparing the
        // reported wrapper identities here would recreate the Streamline false
        // mismatch this field exists to avoid.
        allocation_device = verified_producer_device;
    } else {
        if (!same_identity(source_device.Get(), command_device.Get()) ||
            !same_identity(source_device.Get(), queue_device.Get())) {
            return E_INVALIDARG;
        }
        allocation_device = source_device;
    }

    const D3D12_RESOURCE_DESC description = source->GetDesc();
    // Ordinary producers keep four slots. Luke may use eight but NEVER
    // overwrite a CPU-pinned or GPU-in-flight guide. No producer wait/flush.
    const std::size_t capacity = retention ? SnapshotCount : 4;
    std::size_t selected = capacity;
    for (std::size_t offset = 0; offset < capacity; ++offset) {
        const auto index = (next_snapshot + offset) % capacity;
        const auto& use = snapshots[index].retention;
        if (!use || (use.use_count() == 1 && use->reusable())) { selected = index; break; }
    }
    if (selected == capacity) {
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    std::shared_ptr<DlssSnapshotRetention> lifetime;
    if (retention) {
        try { lifetime = std::make_shared<DlssSnapshotRetention>(); }
        catch (...) { return E_OUTOFMEMORY; }
    }
    SnapshotSlot& slot = snapshots[selected];
    if (!slot.resource || !matching_description(slot.description, description) ||
        !same_identity(slot.allocation_device.Get(), allocation_device.Get())) {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> resource;
        result = allocation_device->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(resource.GetAddressOf()));
        if (FAILED(result)) return result;
        slot.resource = std::move(resource);
        slot.allocation_device = allocation_device;
        slot.description = description;
        slot.state = D3D12_RESOURCE_STATE_COMMON;
    }

    std::array<D3D12_RESOURCE_BARRIER, 2> before{};
    UINT before_count = 0;
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        before[before_count++] = transition_barrier(
            source,
            source_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    if (slot.state != D3D12_RESOURCE_STATE_COPY_DEST) {
        before[before_count++] = transition_barrier(
            slot.resource.Get(), slot.state, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (before_count != 0) {
        producer_command_list->ResourceBarrier(before_count, before.data());
    }
    producer_command_list->CopyResource(slot.resource.Get(), source);

    std::array<D3D12_RESOURCE_BARRIER, 2> after{};
    UINT after_count = 0;
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        after[after_count++] = transition_barrier(
            source,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            source_state);
    }
    after[after_count++] = transition_barrier(
        slot.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, snapshot_state);
    producer_command_list->ResourceBarrier(after_count, after.data());
    slot.state = snapshot_state;
    *snapshot = slot.resource;
    if (allocation_receipt) *allocation_receipt = slot.allocation_device;
    slot.retention = std::move(lifetime);
    if (retention) *retention = slot.retention;
    next_snapshot = (selected + 1) % capacity;
    return S_OK;
}

struct Candidate {
    std::uint64_t first_publication{};
    std::shared_ptr<const DlssMotionVectorFrame> frame;
    std::shared_ptr<const DlssMotionVectorFrame> previous;
};

}  // namespace

std::shared_ptr<const DlssMotionVectorFrame> publish_dlss_motion_vectors(
    const DlssMotionVectorPublication& publication) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire)) return {};
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        if (!valid(publication)) {
            ++state.statistics.invalid_publications;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }

        const std::uint32_t color_origin_token = publication.protect_snapshot_lifetime
            ? state.color_origin_tokens.allocate() : 0;
        if (publication.protect_snapshot_lifetime && !color_origin_token) {
            ++state.statistics.invalid_publications;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }

        auto& stream = state.streams[publication.stream];
        if (publication.reset) {
            ++stream.epoch;
            stream.serial = 0;
            stream.previous.reset();
            stream.previous2.reset();
            stream.latest.reset();
            stream.luke_origin_history.clear();
        }
        ComPtr<ID3D12Resource> output_snapshot;
        if (publication.snapshot_output) {
            if (FAILED(snapshot_resource(stream.output_snapshots,
                    stream.next_output_snapshot, publication.output,
                    publication.output_resource_state,
                    publication.producer_command_list,
                    publication.producer_queue,
                    publication.verified_producer_device,
                    &output_snapshot,
                    D3D12_RESOURCE_STATE_COMMON))) {
                ++state.statistics.snapshot_failures;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return {};
            }
            ++state.statistics.snapshot_copies;
        }

        ComPtr<ID3D12Resource> motion_snapshot;
        ComPtr<ID3D12Device> motion_snapshot_device, depth_snapshot_device;
        std::shared_ptr<DlssSnapshotRetention> motion_retention, depth_retention;
        if (FAILED(snapshot_resource(stream.snapshots, stream.next_snapshot,
                publication.motion_vectors, publication.resource_state,
                publication.producer_command_list, publication.producer_queue,
                publication.verified_producer_device,
                &motion_snapshot, kSnapshotReadState,
                publication.protect_snapshot_lifetime ? &motion_retention : nullptr,
                &motion_snapshot_device))) {
            ++state.statistics.snapshot_failures;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }
        ++state.statistics.snapshot_copies;

        ComPtr<ID3D12Resource> depth_snapshot;
        if (publication.depth != nullptr) {
            if (FAILED(snapshot_resource(stream.depth_snapshots,
                    stream.next_depth_snapshot, publication.depth,
                    publication.depth_resource_state,
                    publication.producer_command_list,
                    publication.producer_queue,
                    publication.verified_producer_device, &depth_snapshot, kSnapshotReadState,
                    publication.protect_snapshot_lifetime ? &depth_retention : nullptr,
                    &depth_snapshot_device))) {
                ++state.statistics.snapshot_failures;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return {};
            }
            ++state.statistics.snapshot_copies;
            ++state.statistics.depth_publications;
        }

        auto frame = std::make_shared<DlssMotionVectorFrame>();
        frame->stream = publication.stream;
        frame->epoch = stream.epoch;
        frame->previous_serial = stream.serial;
        frame->serial = ++stream.serial;
        if (state.next_publication == 0) {
            ++state.statistics.invalid_publications;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }
        frame->publication = state.next_publication++;
        frame->color_origin_token = color_origin_token;
        ++state.statistics.published;
        frame->output = publication.snapshot_output
            ? std::move(output_snapshot)
            : publication.output;
        frame->motion_vectors = std::move(motion_snapshot);
        frame->motion_snapshot_device = std::move(motion_snapshot_device);
        frame->depth_snapshot_device = std::move(depth_snapshot_device);
        frame->producer_queue = publication.producer_queue;
        frame->verified_producer_device =
            publication.verified_producer_device;
        frame->output_x = publication.output_x;
        frame->output_y = publication.output_y;
        frame->output_width = publication.output_width;
        frame->output_height = publication.output_height;
        frame->output_slice = 0;
        frame->motion_x = publication.motion_x;
        frame->motion_y = publication.motion_y;
        frame->motion_width = publication.motion_width;
        frame->motion_height = publication.motion_height;
        frame->motion_slice = 0;
        frame->scale_x = publication.scale_x == 0.0F ? 1.0F : publication.scale_x;
        frame->scale_y = publication.scale_y == 0.0F ? 1.0F : publication.scale_y;
        frame->jitter_x = publication.jitter_x;
        frame->jitter_y = publication.jitter_y;
        frame->previous_jitter_x = stream.jitter_x;
        frame->previous_jitter_y = stream.jitter_y;
        frame->resource_state = kSnapshotReadState;
        frame->jittered = publication.jittered;
        frame->reset = publication.reset;
        frame->depth = std::move(depth_snapshot);
        frame->motion_retention = std::move(motion_retention);
        frame->depth_retention = std::move(depth_retention);
        frame->camera_frame_token = publication.camera_frame_token;
        frame->camera_frame_token_valid = publication.camera_frame_token_valid;
        frame->depth_x = publication.depth_x;
        frame->depth_y = publication.depth_y;
        frame->depth_width = publication.depth_width;
        frame->depth_height = publication.depth_height;
        frame->depth_resource_state = kSnapshotReadState;
        frame->frame_time_delta_ms = publication.frame_time_delta_ms;
        frame->camera_near = publication.camera_near;
        frame->camera_far = publication.camera_far;
        frame->depth_inverted = publication.depth_inverted;
        frame->depth_infinite = publication.depth_infinite;
        frame->camera_position = publication.camera_position;
        frame->camera_up = publication.camera_up;
        frame->camera_right = publication.camera_right;
        frame->camera_forward = publication.camera_forward;
        frame->camera_fov_vertical = publication.camera_fov_vertical;
        frame->camera_data_valid = publication.camera_data_valid;
        frame->camera_view_to_clip = publication.camera_view_to_clip;
        frame->camera_clip_to_view = publication.camera_clip_to_view;
        frame->camera_matrices_valid = publication.camera_matrices_valid;
        frame->raw_camera_motion_included = publication.raw_camera_motion_included;
        frame->raw_clip_to_previous_clip = publication.raw_clip_to_previous_clip;
        frame->raw_previous_clip_to_clip = publication.raw_previous_clip_to_clip;
        frame->producer_command_list = publication.producer_command_list;
        if (publication.protect_snapshot_lifetime) {
            // Calibrate exactly once in immutable production order. Consumer
            // queries may lag, repeat or skip; none may select newer cameras
            // to calibrate an older color-carried identity.
            const auto camera_sample = [](const auto& value) {
                return LukeUnitCamera{value->camera_position, value->camera_right,
                    value->frame_time_delta_ms, value->camera_data_valid && !value->reset};
            };
            const auto& older = stream.previous;
            const auto& middle = stream.latest;
            const bool consecutive = older && middle &&
                older->stream == frame->stream && middle->stream == frame->stream &&
                older->epoch == frame->epoch && middle->epoch == frame->epoch &&
                older->serial + 1 == middle->serial && middle->serial + 1 == frame->serial &&
                older->publication + 1 == middle->publication &&
                middle->publication + 1 == frame->publication &&
                older->color_origin_token != 0 &&
                std::uint64_t(older->color_origin_token) + 1 == middle->color_origin_token &&
                std::uint64_t(middle->color_origin_token) + 1 == frame->color_origin_token;
            const float sample = consecutive
                ? luke_camera_separation(camera_sample(older), camera_sample(middle), camera_sample(frame)) : 0;
            frame->calibrated_game_eye_baseline = stream.camera_units.observe(
                frame->epoch, frame->publication, sample);
        }
        if (stream.previous && stream.previous->epoch == frame->epoch &&
            stream.previous->serial + 2 == frame->serial &&
            stream.previous->publication + 2 == frame->publication) {
            // Exact N-2 scalar camera survives eviction of that GPU snapshot.
            frame->previous_same_native_camera=dlss_native_camera_history(*stream.previous);
        }
        if (two_frame_capture_active()) {
            TwoFrameGuideContract contract{};
            contract.stage = "ngx_ingress";
            contract.stream = frame->stream;
            contract.publication = frame->publication;
            contract.serial = frame->serial;
            contract.previous_serial = frame->previous_serial;
            contract.motion_width = frame->motion_width;
            contract.motion_height = frame->motion_height;
            contract.depth_width = frame->depth_width;
            contract.depth_height = frame->depth_height;
            contract.output_width = frame->output_width;
            contract.output_height = frame->output_height;
            contract.scale_x = frame->scale_x;
            contract.scale_y = frame->scale_y;
            contract.jitter_x = frame->jitter_x;
            contract.jitter_y = frame->jitter_y;
            contract.frame_time_delta_ms = frame->frame_time_delta_ms;
            contract.camera_near = frame->camera_near;
            contract.camera_far = frame->camera_far;
            contract.reset = frame->reset;
            contract.jittered = frame->jittered;
            contract.depth_inverted = frame->depth_inverted;
            contract.depth_infinite = frame->depth_infinite;
            contract.camera_position = frame->camera_position;
            contract.camera_right = frame->camera_right;
            contract.view_to_clip = frame->camera_view_to_clip;
            contract.raw_camera_motion_included = frame->raw_camera_motion_included;
            contract.raw_clip_to_previous_clip = frame->raw_clip_to_previous_clip;
            contract.raw_previous_clip_to_clip = frame->raw_previous_clip_to_clip;
            contract.clip_to_view = frame->camera_clip_to_view;
            two_frame_capture_guide_contract(contract);
        }
        stream.jitter_x = publication.jitter_x;
        stream.jitter_y = publication.jitter_y;
        if (stream.first_publication == 0) {
            stream.first_publication = frame->publication;
        }
        stream.previous2 = std::move(stream.previous);
        stream.previous = std::move(stream.latest);
        stream.latest = std::move(frame);
        if (publication.protect_snapshot_lifetime) {
            stream.luke_origin_history.push_back(stream.latest);
            while (stream.luke_origin_history.size() > kLukeOriginHistoryCount)
                stream.luke_origin_history.pop_front();
        } else {
            stream.luke_origin_history.clear();
        }
        if (state.statistics.used == 0) {
            state.statistics.status = DlssMotionVectorStatus::output_not_direct;
        }
        return stream.latest;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}

void configure_dlss_motion_vector_tracking(bool enabled) noexcept {
    const bool previous = g_tracking_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (enabled == previous) return;
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        if (!enabled) {
            // Do not release the private resources here. The desired setting is
            // visible to DLSS before the OpenXR layer reaches xrEndFrame and its
            // GPU-idle fence, so snapshots may still be referenced by submitted
            // synthesis work. The layer retires them explicitly after that fence.
            state.statistics.status = DlssMotionVectorStatus::disabled;
            return;
        }
        // Re-enabling without a layer-side retirement must not expose stale
        // vectors. Preserve the slot resources (and therefore any in-flight GPU
        // lifetime), but start fresh stream epochs and temporal ownership.
        for (auto& [unused_stream_id, stream] : state.streams) {
            static_cast<void>(unused_stream_id);
            ++stream.epoch;
            stream.serial = 0;
            stream.first_publication = 0;
            stream.jitter_x = 0.0F;
            stream.jitter_y = 0.0F;
            stream.previous.reset();
            stream.previous2.reset();
            stream.latest.reset();
            stream.luke_origin_history.clear();
        }
        state.statistics = {};
        state.statistics.status = DlssMotionVectorStatus::waiting_for_dlss;
    } catch (...) {
    }
}

void retire_disabled_dlss_motion_vector_resources() noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        if (g_tracking_enabled.load(std::memory_order_acquire)) return;
        state.streams.clear();
        state.retired_streams.clear();
        state.statistics = {};
        state.statistics.status = DlssMotionVectorStatus::disabled;
    } catch (...) {
    }
}

void retire_dlss_motion_vector_stream(std::uint64_t stream) noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        auto retired = state.streams.extract(stream);
        if (!retired.empty()) {
            state.retired_streams.push_back(std::move(retired.mapped()));
        }
    } catch (...) {
    }
}

void report_dlss_motion_vector_submission(
    ID3D12CommandQueue* queue,
    std::uint32_t command_list_count,
    ID3D12CommandList* const* command_lists) noexcept {
    try {
        if (queue == nullptr || command_list_count == 0 || command_lists == nullptr)
            return;
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        const auto was_submitted = [&](const auto& frame) {
            if (!frame || !frame->producer_command_list) return false;
            for (std::uint32_t index = 0; index < command_list_count; ++index) {
                if (same_identity(
                        frame->producer_command_list.Get(), command_lists[index]))
                    return true;
            }
            return false;
        };

        for (auto& [unused_stream_id, stream] : state.streams) {
            static_cast<void>(unused_stream_id);
            const bool latest_submitted = was_submitted(stream.latest);
            const bool previous_submitted = was_submitted(stream.previous);
            const bool previous2_submitted = was_submitted(stream.previous2);
            const bool origin_submitted = std::any_of(
                stream.luke_origin_history.begin(), stream.luke_origin_history.end(),
                was_submitted);
            if (!latest_submitted && !previous_submitted &&
                !previous2_submitted && !origin_submitted) continue;

            // The callback follows actual ExecuteCommandLists. Take the
            // allocation receipt from those submitted lists' snapshots;
            // queue.GetDevice may report a Streamline device proxy.
            ComPtr<ID3D12Device> submission_device;
            bool incompatible_devices = false;
            const auto inspect_owner = [&](const auto& frame, bool submitted) {
                if (!submitted) return;
                if (!frame->motion_snapshot_device ||
                    (submission_device && !same_identity(submission_device.Get(),
                        frame->motion_snapshot_device.Get())) ||
                    (frame->depth && !same_identity(frame->motion_snapshot_device.Get(),
                        frame->depth_snapshot_device.Get()))) {
                    incompatible_devices = true;
                    return;
                }
                submission_device = frame->motion_snapshot_device;
            };
            inspect_owner(stream.latest, latest_submitted);
            inspect_owner(stream.previous, previous_submitted);
            inspect_owner(stream.previous2, previous2_submitted);
            for (const auto& origin : stream.luke_origin_history)
                inspect_owner(origin, was_submitted(origin));
            if (!submission_device || incompatible_devices) {
                ++state.statistics.invalid_rejections;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                continue;
            }
            if (!stream.submission_fence || !same_identity(
                    stream.submission_fence_device.Get(), submission_device.Get())) {
                ComPtr<ID3D12Fence> fence;
                if (FAILED(submission_device->CreateFence(
                        0, D3D12_FENCE_FLAG_NONE,
                        IID_PPV_ARGS(fence.GetAddressOf())))) {
                    ++state.statistics.invalid_rejections;
                    state.statistics.status = DlssMotionVectorStatus::invalid_input;
                    continue;
                }
                stream.submission_fence = std::move(fence);
                stream.submission_fence_device = submission_device;
                stream.next_submission_fence_value = 1;
            }

            const std::uint64_t fence_value =
                stream.next_submission_fence_value++;
            if (FAILED(queue->Signal(stream.submission_fence.Get(), fence_value))) {
                ++state.statistics.invalid_rejections;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                continue;
            }

            std::unordered_map<std::uint64_t,
                std::shared_ptr<const DlssMotionVectorFrame>> updated_frames;
            const auto stamp_submission = [&](auto& stored, bool submitted) {
                if (!submitted) return;
                const auto existing = updated_frames.find(stored->publication);
                if (existing != updated_frames.end()) {
                    stored = existing->second;
                    return;
                }
                auto updated = std::make_shared<DlssMotionVectorFrame>(*stored);
                updated->producer_queue = queue;
                updated->producer_command_list.Reset();
                updated->producer_fence = stream.submission_fence;
                updated->producer_fence_device = stream.submission_fence_device;
                updated->producer_fence_value = fence_value;
                updated_frames.emplace(updated->publication, updated);
                stored = std::move(updated);
            };
            stamp_submission(stream.latest, latest_submitted);
            stamp_submission(stream.previous, previous_submitted);
            stamp_submission(stream.previous2, previous2_submitted);
            for (auto& origin : stream.luke_origin_history)
                stamp_submission(origin, was_submitted(origin));
        }
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
    }
}

std::shared_ptr<const DlssMotionVectorSet> resolve_dlss_motion_vectors(
    ID3D12Resource* output,
    ID3D12CommandQueue* consumer_queue) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire)) {
            report_dlss_motion_vector_status(DlssMotionVectorStatus::disabled);
            return {};
        }
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.resolve_calls;
        if (output == nullptr || consumer_queue == nullptr) {
            ++state.statistics.invalid_rejections;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }

        const auto description = output->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            ++state.statistics.invalid_rejections;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }
        std::uint32_t eye_count = std::min<std::uint32_t>(
            std::max<std::uint32_t>(description.DepthOrArraySize, 1U),
            kDlssMotionVectorEyeCount);

        std::vector<Candidate> candidates;
        std::uint32_t streams_with_frames = 0;
        candidates.reserve(state.streams.size());
        for (const auto& [unused_stream_id, stream] : state.streams) {
            static_cast<void>(unused_stream_id);
            if (!stream.latest) continue;
            ++streams_with_frames;
            if (same_identity(stream.latest->producer_queue.Get(), consumer_queue) ||
                (stream.latest->producer_fence &&
                 stream.latest->producer_fence_value != 0 &&
                 same_device(stream.latest->producer_fence.Get(), consumer_queue))) {
                candidates.push_back(
                    {stream.first_publication, stream.latest, stream.previous});
            }
        }
        const auto synchronize = [&](const auto& frame) {
            if (same_identity(frame->producer_queue.Get(), consumer_queue))
                return true;
            return frame->producer_fence && frame->producer_fence_value != 0 &&
                same_device(frame->producer_fence.Get(), consumer_queue) &&
                SUCCEEDED(consumer_queue->Wait(frame->producer_fence.Get(),
                    frame->producer_fence_value));
        };

        // Native Stereo can place two eye viewports side by side in one
        // array-size-one XR image while DLSS keeps one persistent stream per
        // eye. Restrict the candidate set to streams whose published output is
        // exactly one horizontal half of the XR image. This excludes desktop
        // DLSS streams and leaves ordinary mono/Sync Sequential resources on
        // the existing array-size-one path.
        if (eye_count == 1) {
            std::vector<Candidate> horizontal_eye_candidates;
            horizontal_eye_candidates.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                if (candidate.frame->output_width != 0 &&
                    candidate.frame->output_height != 0 &&
                    description.Width ==
                        static_cast<std::uint64_t>(
                            candidate.frame->output_width) * 2U &&
                    description.Height == candidate.frame->output_height) {
                    horizontal_eye_candidates.push_back(candidate);
                }
            }
            if (horizontal_eye_candidates.size() >=
                    kDlssMotionVectorEyeCount) {
                candidates = std::move(horizontal_eye_candidates);
                eye_count = kDlssMotionVectorEyeCount;
            }
        }

        // Alternating-eye renderers such as Ghost of Tsushima's AER path reuse
        // one DLSS handle for both eyes. A complete stereo guide set therefore
        // consists of two consecutive publications from that single stream.
        // Even local serials close the stable eye-0/eye-1 pair; odd serials are
        // deliberately left pending until the second eye arrives.
        if (eye_count == 2 && candidates.size() == 1) {
            const auto& current = candidates.front().frame;
            const auto& previous = candidates.front().previous;
            if (previous && current->stream == previous->stream &&
                current->epoch == previous->epoch &&
                current->serial == previous->serial + 1 &&
                (current->serial & 1U) == 0 &&
                current->publication > previous->publication &&
                current->publication - previous->publication <=
                    kMaximumStereoPublicationGap &&
                synchronize(previous) && synchronize(current)) {
                auto left = std::make_shared<DlssMotionVectorFrame>(*previous);
                auto right = std::make_shared<DlssMotionVectorFrame>(*current);
                left->previous_serial = left->serial > 2 ? left->serial - 2 : 0;
                right->previous_serial = right->serial > 2 ? right->serial - 2 : 0;
                auto result = std::make_shared<DlssMotionVectorSet>();
                result->eye_count = 2;
                result->eyes[0] = std::move(left);
                result->eyes[1] = std::move(right);
                ++state.statistics.matched;
                return result;
            }
        }

        if (candidates.size() < eye_count) {
            if (streams_with_frames >= eye_count) {
                ++state.statistics.resolve_queue_mismatches;
                state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
            } else {
                ++state.statistics.resolve_missing_streams;
                state.statistics.status = state.statistics.published == 0
                    ? DlssMotionVectorStatus::waiting_for_dlss
                    : DlssMotionVectorStatus::output_not_direct;
            }
            return {};
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            return a.frame->publication > b.frame->publication;
        });
        candidates.resize(eye_count);
        for (const auto& candidate : candidates) {
            if (!synchronize(candidate.frame)) {
                ++state.statistics.invalid_rejections;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return {};
            }
        }
        const auto newest = candidates.front().frame->publication;
        const auto oldest = candidates.back().frame->publication;
        if (newest < oldest || newest - oldest > kMaximumStereoPublicationGap) {
            ++state.statistics.resolve_stale_pairs;
            state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
            return {};
        }

        // Stable first-seen stream order defines eye order. This survives any
        // post-DLSS shader passes because the association follows the two DLSS
        // evaluation streams rather than the final color resource identity.
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            return a.first_publication < b.first_publication;
        });

        auto result = std::make_shared<DlssMotionVectorSet>();
        result->eye_count = eye_count;
        for (std::uint32_t eye = 0; eye < eye_count; ++eye) {
            result->eyes[eye] = candidates[eye].frame;
        }
        ++state.statistics.matched;
        return result;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}

std::shared_ptr<const DlssMotionVectorSet> resolve_dlss_motion_vectors(
    std::uint32_t eye_count,
    ID3D12CommandQueue* consumer_queue) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire)) {
            report_dlss_motion_vector_status(DlssMotionVectorStatus::disabled);
            return {};
        }
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.resolve_calls;
        if (eye_count == 0 || eye_count > kDlssMotionVectorEyeCount ||
            consumer_queue == nullptr) {
            ++state.statistics.invalid_rejections;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }

        std::vector<Candidate> candidates;
        std::uint32_t streams_with_frames = 0;
        candidates.reserve(state.streams.size());
        for (const auto& [unused_stream_id, stream] : state.streams) {
            static_cast<void>(unused_stream_id);
            if (!stream.latest) continue;
            ++streams_with_frames;
            if (same_identity(stream.latest->producer_queue.Get(), consumer_queue) ||
                (stream.latest->producer_fence &&
                 stream.latest->producer_fence_value != 0 &&
                 same_device(stream.latest->producer_fence.Get(), consumer_queue))) {
                candidates.push_back(
                    {stream.first_publication, stream.latest, stream.previous});
            }
        }

        const auto synchronize = [&](const auto& frame) {
            if (!frame ||
                same_identity(frame->producer_queue.Get(), consumer_queue))
                return true;
            return frame->producer_fence && frame->producer_fence_value != 0 &&
                SUCCEEDED(consumer_queue->Wait(
                    frame->producer_fence.Get(), frame->producer_fence_value));
        };

        // Alternating-eye renderers such as Ghost of Tsushima's AER path reuse
        // one DLSS handle for both eyes. A complete stereo guide set therefore
        // consists of two consecutive publications from that single stream.
        // Even local serials close the stable eye-0/eye-1 pair; odd serials are
        // deliberately left pending until the second eye arrives.
        if (eye_count == 2 && candidates.size() == 1) {
            const auto& current = candidates.front().frame;
            const auto& previous = candidates.front().previous;
            if (previous && current->stream == previous->stream &&
                current->epoch == previous->epoch &&
                current->serial == previous->serial + 1 &&
                (current->serial & 1U) == 0 &&
                current->publication > previous->publication &&
                current->publication - previous->publication <=
                    kMaximumStereoPublicationGap &&
                (same_identity(previous->producer_queue.Get(), consumer_queue) ||
                 (previous->producer_fence &&
                  previous->producer_fence_value != 0 &&
                  same_device(previous->producer_fence.Get(), consumer_queue))) &&
                synchronize(previous) && synchronize(current)) {
                auto left = std::make_shared<DlssMotionVectorFrame>(*previous);
                auto right = std::make_shared<DlssMotionVectorFrame>(*current);
                left->previous_serial = left->serial > 2 ? left->serial - 2 : 0;
                right->previous_serial = right->serial > 2 ? right->serial - 2 : 0;
                auto result = std::make_shared<DlssMotionVectorSet>();
                result->eye_count = 2;
                result->eyes[0] = std::move(left);
                result->eyes[1] = std::move(right);
                ++state.statistics.matched;
                return result;
            }
        }

        if (candidates.size() < eye_count) {
            if (streams_with_frames >= eye_count) {
                ++state.statistics.resolve_queue_mismatches;
                state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
            } else {
                ++state.statistics.resolve_missing_streams;
                state.statistics.status = state.statistics.published == 0
                    ? DlssMotionVectorStatus::waiting_for_dlss
                    : DlssMotionVectorStatus::output_not_direct;
            }
            return {};
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            return a.frame->publication > b.frame->publication;
        });
        candidates.resize(eye_count);
        for (const auto& candidate : candidates) {
            if (!synchronize(candidate.frame)) {
                ++state.statistics.invalid_rejections;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return {};
            }
        }
        const auto newest = candidates.front().frame->publication;
        const auto oldest = candidates.back().frame->publication;
        if (newest < oldest || newest - oldest > kMaximumStereoPublicationGap) {
            ++state.statistics.resolve_stale_pairs;
            state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
            return {};
        }

        // Stable first-seen stream order defines eye order. This survives any
        // post-DLSS shader passes because the association follows the two DLSS
        // evaluation streams rather than the final color resource identity.
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            return a.first_publication < b.first_publication;
        });

        auto result = std::make_shared<DlssMotionVectorSet>();
        result->eye_count = eye_count;
        for (std::uint32_t eye = 0; eye < eye_count; ++eye) {
            result->eyes[eye] = candidates[eye].frame;
        }
        ++state.statistics.matched;
        return result;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}

std::shared_ptr<const DlssMotionVectorSet> resolve_luke_alternating_guides(
    ID3D12CommandQueue* consumer_queue) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire) ||
            consumer_queue == nullptr) return {};
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.resolve_calls;
        // The Luke route is admitted only with exactly one live NGX stream.
        // Never silently choose a pair from a different producer.
        if (state.streams.size() != 1) return {};
        const auto& stream = state.streams.begin()->second;
        const auto& current = stream.latest;
        const auto& previous = stream.previous;
        const auto& previous2 = stream.previous2;
        if (!previous2 || !previous || !current ||
            previous2->stream != previous->stream ||
            previous2->epoch != previous->epoch ||
            previous2->serial + 1 != previous->serial ||
            previous2->publication + 1 != previous->publication ||
            current->stream != previous->stream ||
            current->epoch != previous->epoch ||
            current->serial != previous->serial + 1 ||
            current->publication <= previous->publication ||
            current->publication - previous->publication >
                kMaximumStereoPublicationGap) return {};
        const auto compatible = [&](const auto& frame) {
            if (same_identity(frame->producer_queue.Get(), consumer_queue))
                return true;
            return frame->producer_fence && frame->producer_fence_value != 0 &&
                same_device(frame->producer_fence.Get(), consumer_queue) &&
                SUCCEEDED(consumer_queue->Wait(frame->producer_fence.Get(),
                                              frame->producer_fence_value));
        };
        if (!compatible(previous2) || !compatible(previous) ||
            !compatible(current)) return {};
        auto older = std::make_shared<DlssMotionVectorFrame>(*previous);
        auto newer = std::make_shared<DlssMotionVectorFrame>(*current);
        older->previous_serial = older->serial > 2 ? older->serial - 2 : 0;
        newer->previous_serial = newer->serial > 2 ? newer->serial - 2 : 0;
        older->previous_opposite_guide = previous2;
        newer->previous_opposite_guide = previous;
        newer->previous_same_guide = previous2;
        auto result = std::make_shared<DlssMotionVectorSet>();
        result->eye_count = 2;
        result->eyes[0] = std::move(older);
        result->eyes[1] = std::move(newer);
        ++state.statistics.matched;
        return result;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}


std::shared_ptr<const DlssMotionVectorFrame> resolve_luke_guide_token(
    std::uint32_t color_origin_token,
    ID3D12CommandQueue* consumer_queue,
    ID3D12Device* canonical_consumer_device) noexcept {
    try {
        const bool tracking = g_tracking_enabled.load(std::memory_order_acquire);
        if (!tracking || color_origin_token == 0 || consumer_queue == nullptr ||
            canonical_consumer_device == nullptr) {
            return {};
        }
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.resolve_calls;
        if (state.streams.size() != 1) {
            ++state.statistics.resolve_missing_streams;
            return {};
        }
        const auto& stream = state.streams.begin()->second;
        const auto origin = std::find_if(stream.luke_origin_history.begin(),
            stream.luke_origin_history.end(), [&](const auto& frame) {
                return frame->color_origin_token == color_origin_token;
            });
        if (origin == stream.luke_origin_history.end()) {
            ++state.statistics.resolve_missing_streams;
            return {};
        }
        if (origin == stream.luke_origin_history.begin()) {
            ++state.statistics.resolve_stale_pairs;
            return {};
        }
        const auto& current = *origin;
        const auto& previous = *std::prev(origin);
        if (current->stream != previous->stream ||
            current->epoch != previous->epoch ||
            current->epoch != stream.epoch ||
            previous->serial + 1 != current->serial ||
            previous->publication + 1 != current->publication) {
            ++state.statistics.resolve_stale_pairs;
            return {};
        }
        const auto compatible = [&](const auto& frame) {
            // Only the owned snapshots enter AFW. The original SR output is
            // metadata for its color key, not a resource sampled by AFW.
            if (!same_identity(frame->motion_snapshot_device.Get(),
                    canonical_consumer_device)) {
                return false;
            }
            if (frame->depth && !same_identity(frame->depth_snapshot_device.Get(),
                    canonical_consumer_device)) {
                return false;
            }
            if (same_identity(frame->producer_queue.Get(), consumer_queue))
                return true;
            if (!frame->producer_fence || frame->producer_fence_value == 0) {
                return false;
            }
            if (!same_identity(frame->producer_fence_device.Get(),
                    canonical_consumer_device)) {
                return false;
            }
            const HRESULT wait = consumer_queue->Wait(frame->producer_fence.Get(),
                frame->producer_fence_value);
            if (FAILED(wait)) {
                return false;
            }
            return true;
        };
        if (!compatible(previous) || !compatible(current)) {
            ++state.statistics.resolve_queue_mismatches;
            state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
            return {};
        }
        auto resolved = std::make_shared<DlssMotionVectorFrame>(*current);
        resolved->previous_serial = resolved->serial > 2 ? resolved->serial - 2 : 0;
        resolved->previous_opposite_guide = previous;
        if (std::distance(stream.luke_origin_history.begin(), origin) >= 2) {
            const auto& native_previous = *std::prev(origin, 2);
            if (native_previous->stream == current->stream &&
                native_previous->epoch == current->epoch &&
                native_previous->serial + 2 == current->serial &&
                native_previous->publication + 2 == current->publication) {
                // Only its immutable CPU camera is used. Current and N-1
                // snapshots already passed the GPU compatibility checks above.
                resolved->previous_same_guide = native_previous;
            }
        }
        ++state.statistics.matched;
        return resolved;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}

void report_dlss_motion_vector_status(DlssMotionVectorStatus status) noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        if (status == DlssMotionVectorStatus::temporal_mismatch) {
            ++state.statistics.temporal_rejections;
        } else if (status == DlssMotionVectorStatus::invalid_input) {
            ++state.statistics.invalid_rejections;
        }
        state.statistics.status = status;
    } catch (...) {
    }
}

void report_dlss_motion_vector_use() noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.used;
        state.statistics.last_used_publication = state.next_publication - 1;
        state.statistics.status = DlssMotionVectorStatus::used;
    } catch (...) {
    }
}

DlssMotionVectorStatistics dlss_motion_vector_statistics() noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        return state.statistics;
    } catch (...) {
        return {};
    }
}

}  // namespace xrfg
