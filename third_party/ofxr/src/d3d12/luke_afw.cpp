#include "xrfg/luke_afw.hpp"
#include "xrfg/luke_depth_format.hpp"
#include "xrfg/luke_motion_format.hpp"
#include "xrfg/luke_native_motion.hpp"
#include "xrfg/luke_recording_scope.hpp"
#include "xrfg/luke_guide_window.hpp"
#include "xrfg/luke_physical_projection.hpp"
#include "xrfg/luke_pose_camera.hpp"
#include "xrfg/luke_world_scale.hpp"
#include "xrfg/luke_world_scale_config.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/two_frame_capture.hpp"
#include "luke_depth_convert_shader.hpp"
#include "luke_compose_motion_shader.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <limits>
#include <new>
#include <utility>

namespace xrfg {
namespace {

struct Matrix4x4 { float values[16]{}; };

struct DeviceParams {
    ID3D11Device* d3d11_device{};
    ID3D11DeviceContext* d3d11_context{};
    ID3D12Device* d3d12_device{};
    ID3D12CommandQueue* d3d12_queue{};
    IDXGIAdapter* dxgi_adapter{};
};

struct FrameWarpInitParams {
    int hmd_width{};
    int hmd_height{};
    DXGI_FORMAT eye_format{DXGI_FORMAT_R8G8B8A8_UNORM};
    DXGI_FORMAT backbuffer_format{DXGI_FORMAT_R8G8B8A8_UNORM};
};

enum EyeIndex { EyeLeft = 0, EyeRight = 1 };
enum FrameWarpMode { None = 0, AlternateEyeWarping = 1, PreviousFrameWarping = 2, CombinedWarping = 3 };
enum ImageType { Image = 0, Depth = 1 };
enum MotionVectorType { Normal = 0, FromOtherEye = 1, ObjectOnly = 2 };

struct TextureDesc {
    ImageType type{Image};
    ID3D12Resource* texture{};
    int srv_position{-1};
    int uav_position{-1};
    D3D12_GPU_DESCRIPTOR_HANDLE shader_resource_view{};
    D3D12_GPU_DESCRIPTOR_HANDLE unordered_access_view{};
    union {
        D3D12_CPU_DESCRIPTOR_HANDLE render_target_view{};
        D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil_view;
    };
    D3D12_RESOURCE_STATES initial_state{D3D12_RESOURCE_STATE_COMMON};
};

struct FrameBufferDesc { TextureDesc color; TextureDesc depth; TextureDesc motion_vectors; };
struct EyeFrameBuffers { FrameBufferDesc eye_frame_buffers[2]; };

struct CameraData {
    Matrix4x4 destination_world_to_view;
    Matrix4x4 destination_view_to_world;
    Matrix4x4 destination_view_to_clip;
    Matrix4x4 destination_clip_to_view;
    Matrix4x4 source_world_to_view;
    Matrix4x4 source_view_to_world;
    Matrix4x4 source_view_to_clip;
    Matrix4x4 source_clip_to_view;
    Matrix4x4 camera_world_to_view;
    Matrix4x4 camera_view_to_world;
    Matrix4x4 camera_view_to_clip;
    Matrix4x4 camera_clip_to_view;
};

struct FrameWarpEvaluateParams {
    void* command_list{};
    FrameBufferDesc* input_eye_frame_buffer{};
    FrameBufferDesc* output_eye_frame_buffer{};
    TextureDesc* ui_color_alpha{};
    float ui_scale[2]{1.0F, 1.0F};
    float ui_position[3]{0.0F, 0.0F, -1.0F};
    float motion_scale[2]{};
    FrameWarpMode mode{};
    EyeIndex eye_index{};
    CameraData* camera_data{};
    bool clear_before_warping{};
    float ignore_motion_threshold{2.5F};
    bool is_hudless_color{true};
    MotionVectorType motion_vectors_type{Normal};
    bool debug{};
    bool is_foveated{};
    RECT foveated_area{};
    TextureDesc* ue_velocity_buffer{};
    bool use_uint64{};
    float reserved[12]{};
};

static_assert(sizeof(TextureDesc) == 56);
static_assert(sizeof(FrameBufferDesc) == 168);
static_assert(sizeof(EyeFrameBuffers) == 336);
static_assert(sizeof(CameraData) == 768);
static_assert(sizeof(FrameWarpEvaluateParams) == 184);
static_assert(offsetof(FrameWarpEvaluateParams, reserved) == 132);

struct RendererApi;
using InitDeviceFn = RendererApi* (__stdcall*)(DeviceParams);
using InitFrameWarpFn = EyeFrameBuffers (__stdcall*)(FrameWarpInitParams);
using EvaluateFrameWarpFn = void (__stdcall*)(FrameWarpEvaluateParams&);
using SetupTextureDescFn = void (__fastcall*)(RendererApi*, TextureDesc&);
using CreateTextureFn = bool (__fastcall*)(RendererApi*, int, int, DXGI_FORMAT,
    D3D12_RESOURCE_STATES, TextureDesc&, bool);
using CopyTextureFn = void (__fastcall*)(RendererApi*, ID3D12GraphicsCommandList*,
    TextureDesc&, TextureDesc&, D3D12_BOX, UINT, UINT);

constexpr std::size_t kSetupTextureDescSlot = 14;
constexpr std::size_t kCreateTextureSlot = 16;
constexpr std::size_t kCopyTextureSlot = 20;

enum class AfwStage : std::uint64_t {
    enter = 1,
    arguments_valid,
    descriptions_ready,
    camera_ready,
    frame_warp_init_begin,
    frame_warp_init_end,
    depth_create_begin,
    depth_create_end,
    motion_allocate_begin,
    motion_resource_created,
    motion_descriptor_ready,
    resources_ready,
    producer_waited,
    command_list_reset,
    color_copied,
    depth_converted,
    motion_copied,
    evaluate_begin,
    evaluate_end,
    output_copied,
    command_list_closed,
    command_list_executed,
    fence_signaled,
    debug_toggle,
    configuration,
    allocator_wait_begin,
    allocator_wait_end,
};

void report_stage(AfwStage stage, std::int64_t result, std::uint64_t detail,
    std::uint64_t publication) noexcept {
    bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
        result, static_cast<std::uint64_t>(stage), detail, publication);
}

void identity(Matrix4x4& value) noexcept {
    value = {};
    value.values[0] = value.values[5] = value.values[10] = value.values[15] = 1.0F;
}

bool invert(const Matrix4x4& source, Matrix4x4& output) noexcept {
    float a[4][8]{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column)
            a[row][column] = source.values[row * 4 + column];
        a[row][4 + row] = 1.0F;
    }
    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row)
            if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) pivot = row;
        if (std::fabs(a[pivot][column]) < 1.0e-7F) return false;
        if (pivot != column) std::swap(a[pivot], a[column]);
        const float divisor = a[column][column];
        for (float& item : a[column]) item /= divisor;
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column) continue;
            const float factor = a[row][column];
            for (std::size_t item = 0; item < 8; ++item)
                a[row][item] -= factor * a[column][item];
        }
    }
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            output.values[row * 4 + column] = a[row][4 + column];
    return true;
}

Matrix4x4 multiply(const Matrix4x4& left,
    const Matrix4x4& right) noexcept {
    Matrix4x4 product{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t term = 0; term < 4; ++term)
                product.values[row * 4 + column] +=
                    left.values[row * 4 + term] *
                    right.values[term * 4 + column];
    return product;
}

void view_to_world(const std::array<float, 3>& right,
    const std::array<float, 3>& up, const std::array<float, 3>& forward,
    const std::array<float, 3>& position, Matrix4x4& output) noexcept {
    output = {};
    output.values[0] = right[0];
    output.values[1] = right[1];
    output.values[2] = right[2];
    output.values[4] = up[0];
    output.values[5] = up[1];
    output.values[6] = up[2];
    output.values[8] = forward[0];
    output.values[9] = forward[1];
    output.values[10] = forward[2];
    output.values[12] = position[0];
    output.values[13] = position[1];
    output.values[14] = position[2];
    output.values[15] = 1.0F;
}

float luke_world_scale(const std::filesystem::path& plugin_path) {
    return luke_world_scale_from_ini(plugin_path.parent_path() / L"RealVR.ini");
}

bool camera_data(const D3D12ReprojectionView& source,
    const D3D12ReprojectionView& destination,
    const DlssMotionVectorFrame& guide, std::uint32_t source_eye,
    float world_scale, float xr_eye_baseline, float units_per_metre, CameraData& output) noexcept {
    output = {};
    if (!guide.camera_data_valid || !guide.camera_matrices_valid ||
        source_eye > 1) return false;
    float game_baseline{};
    // AER submits cross-time eye poses. Their distance includes head motion,
    // not just IPD. Use ONE simultaneous XR pair, including the middle peer.
    if (!luke_game_eye_baseline(xr_eye_baseline, world_scale,
            units_per_metre, game_baseline))
        return false;

    auto source_projection = guide.camera_view_to_clip;
    auto destination_projection = guide.camera_view_to_clip;
    if (!luke_physical_eye_projection(source_projection,
            source.fov.angle_left, source.fov.angle_right,
            source.fov.angle_up, source.fov.angle_down) ||
        !luke_physical_eye_projection(destination_projection,
            destination.fov.angle_left, destination.fov.angle_right,
            destination.fov.angle_up, destination.fov.angle_down))
        return false;
    std::copy(source_projection.begin(), source_projection.end(),
        output.source_view_to_clip.values);
    std::copy(destination_projection.begin(), destination_projection.end(),
        output.destination_view_to_clip.values);
    if (!invert(output.source_view_to_clip, output.source_clip_to_view) ||
        !invert(output.destination_view_to_clip,
            output.destination_clip_to_view)) return false;

    LukeCameraBasis source_basis{};
    if (!luke_view_camera_basis({guide.camera_right, guide.camera_up,
            guide.camera_forward}, guide.camera_view_to_clip[11], source_basis))
        return false;
    view_to_world(source_basis[0], source_basis[1], source_basis[2],
        guide.camera_position, output.source_view_to_world);
    if (!invert(output.source_view_to_world, output.source_world_to_view))
        return false;
    const float direction = source_eye == 0 ? 1.0F : -1.0F;
    std::array<float, 3> destination_position{
        guide.camera_position[0] + direction * guide.camera_right[0] * game_baseline,
        guide.camera_position[1] + direction * guide.camera_right[1] * game_baseline,
        guide.camera_position[2] + direction * guide.camera_right[2] * game_baseline};
    LukeCameraBasis destination_basis{};
    if (!luke_destination_view_camera_basis(source.pose.orientation,
            destination.pose.orientation, source_basis,
            guide.camera_view_to_clip[11], destination_basis)) return false;
    view_to_world(destination_basis[0], destination_basis[1],
        destination_basis[2], destination_position,
        output.destination_view_to_world);
    if (!invert(output.destination_view_to_world,
                output.destination_world_to_view)) return false;
    // PureDark reserves these four matrices for optional UI rendering;
    // source/destination matrices above drive the scene warp.
    identity(output.camera_world_to_view);
    identity(output.camera_view_to_world);
    identity(output.camera_view_to_clip);
    identity(output.camera_clip_to_view);
    return true;
}

bool compatible_color(const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    return left.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        right.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        left.Width == right.Width && left.Height == right.Height &&
        left.Format == right.Format && left.DepthOrArraySize == 1 &&
        right.DepthOrArraySize == 1 && left.SampleDesc.Count == 1 &&
        right.SampleDesc.Count == 1;
}

enum class ColorFormatGroup {
    none,
    rgba8,
    bgra8,
};

ColorFormatGroup color_format_group(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return ColorFormatGroup::rgba8;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return ColorFormatGroup::bgra8;
    default:
        return ColorFormatGroup::none;
    }
}

DXGI_FORMAT linear_color_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}

bool copy_compatible_color(const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    const ColorFormatGroup left_group = color_format_group(left.Format);
    const bool compatible_format = left.Format == right.Format ||
        (left_group != ColorFormatGroup::none &&
            left_group == color_format_group(right.Format));
    return left.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        right.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        left.Width == right.Width && left.Height == right.Height &&
        compatible_format && left.DepthOrArraySize == 1 &&
        right.DepthOrArraySize == 1 && left.SampleDesc.Count == 1 &&
        right.SampleDesc.Count == 1;
}

}  // namespace

struct LukeAfwSynthesizer::Impl {
    HMODULE module{};
    InitDeviceFn init_device{};
    InitFrameWarpFn init_frame_warp{};
    EvaluateFrameWarpFn evaluate{};
    RendererApi* renderer{};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> command_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> submission_fence;
    HANDLE submission_event{};
    std::uint64_t last_submission_fence{};
    std::uint64_t next_submission_fence{1};
    EyeFrameBuffers eye_buffers{};
    TextureDesc depth;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_staging;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> depth_descriptor_heap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> depth_root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> depth_pipeline;
    D3D12_RESOURCE_STATES depth_staging_state{D3D12_RESOURCE_STATE_COMMON};
    UINT depth_descriptor_size{};
    DXGI_FORMAT depth_source_format{};
    UINT depth_source_width{}, depth_source_height{};
    UINT depth_width{};
    UINT depth_height{};
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_resource;
    TextureDesc motion;
    Microsoft::WRL::ComPtr<ID3D12Resource> composed_motion_staging;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> composed_motion_descriptors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> composed_motion_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> composed_motion_pipeline;
    UINT composed_motion_descriptor_size{};
    UINT composed_motion_width{};
    UINT composed_motion_height{};
    D3D12_RESOURCE_DESC color_description{};
    D3D12_RESOURCE_DESC motion_description{};
    float world_scale{};
    float calibrated_units_per_metre{};
    std::uint64_t unit_epoch{};
    std::array<ULONGLONG, 64> input_rejection_report_ticks{};
    bool frame_buffers_ready{};

    ~Impl() {
        if (submission_event != nullptr) CloseHandle(submission_event);
        if (module != nullptr) FreeLibrary(module);
    }

    template <typename Function>
    Function method(std::size_t slot) const noexcept {
        if (renderer == nullptr) return nullptr;
        void** table = *reinterpret_cast<void***>(renderer);
        return table == nullptr ? nullptr : reinterpret_cast<Function>(table[slot]);
    }

    HRESULT ensure_float_depth(UINT width, UINT height,
        const D3D12_RESOURCE_DESC& source) noexcept {
        const auto format = luke_depth_format(source.Format);
        if (format.resource == DXGI_FORMAT_UNKNOWN ||
            source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            source.Width > INT_MAX || source.DepthOrArraySize != 1 ||
            source.SampleDesc.Count != 1) return DXGI_ERROR_UNSUPPORTED;
        if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX)
            return E_INVALIDARG;
        if (depth.texture != nullptr && depth_staging != nullptr &&
            depth_width == width && depth_height == height &&
            depth_source_format == source.Format && depth_source_width == source.Width &&
            depth_source_height == source.Height) return S_OK;

        const auto create = method<CreateTextureFn>(kCreateTextureSlot);
        if (create == nullptr) return E_NOINTERFACE;
        TextureDesc created{};
        created.type = Depth;
        if (!create(renderer, static_cast<int>(width), static_cast<int>(height),
                DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                created, true) ||
            created.texture == nullptr) return E_FAIL;
        created.type = Depth;
        created.initial_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

        D3D12_HEAP_PROPERTIES heap_properties{};
        heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_properties.CreationNodeMask = 1;
        heap_properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC staging_description{};
        staging_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        staging_description.Width = source.Width;
        staging_description.Height = source.Height;
        staging_description.DepthOrArraySize = 1;
        staging_description.MipLevels = 1;
        staging_description.Format = format.resource;
        staging_description.SampleDesc.Count = 1;
        staging_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        staging_description.Flags = D3D12_RESOURCE_FLAG_NONE;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        HRESULT result = device->CreateCommittedResource(&heap_properties,
            D3D12_HEAP_FLAG_NONE, &staging_description,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(staging.GetAddressOf()));
        if (FAILED(result)) return result;

        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_description.NumDescriptors = 2;
        heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
        result = device->CreateDescriptorHeap(
            &heap_description, IID_PPV_ARGS(descriptors.GetAddressOf()));
        if (FAILED(result)) return result;

        std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 0;
        std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
        for (UINT index = 0; index < ranges.size(); ++index) {
            parameters[index].ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[index].DescriptorTable.NumDescriptorRanges = 1;
            parameters[index].DescriptorTable.pDescriptorRanges = &ranges[index];
            parameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        parameters[2].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.ShaderRegister = 0;
        parameters[2].Constants.Num32BitValues = 4;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = static_cast<UINT>(parameters.size());
        root_description.pParameters = parameters.data();
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        result = D3D12SerializeRootSignature(&root_description,
            D3D_ROOT_SIGNATURE_VERSION_1, serialized.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
        result = device->CreateRootSignature(queue->GetDesc().NodeMask,
            serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(root_signature.GetAddressOf()));
        if (FAILED(result)) return result;

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description{};
        pipeline_description.pRootSignature = root_signature.Get();
        pipeline_description.CS = {g_xrfg_luke_depth_convert_shader,
            sizeof(g_xrfg_luke_depth_convert_shader)};
        pipeline_description.NodeMask = queue->GetDesc().NodeMask;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        result = device->CreateComputePipelineState(&pipeline_description,
            IID_PPV_ARGS(pipeline.GetAddressOf()));
        if (FAILED(result)) return result;

        const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            descriptors->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format.srv;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(staging.Get(), &srv, cpu);
        cpu.ptr += descriptor_size;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(created.texture, nullptr, &uav, cpu);
        result = device->GetDeviceRemovedReason();
        if (FAILED(result)) return result;

        depth = created;
        depth_staging = std::move(staging);
        depth_descriptor_heap = std::move(descriptors);
        depth_root_signature = std::move(root_signature);
        depth_pipeline = std::move(pipeline);
        depth_staging_state = D3D12_RESOURCE_STATE_COMMON;
        depth_descriptor_size = descriptor_size;
        depth_source_format = source.Format;
        depth_source_width = static_cast<UINT>(source.Width);
        depth_source_height = source.Height;
        depth_width = width;
        depth_height = height;
        return S_OK;
    }

    HRESULT convert_depth(ID3D12GraphicsCommandList* list,
        ID3D12Resource* source, D3D12_RESOURCE_STATES source_state,
        UINT source_x, UINT source_y, bool depth_inverted,
        int depth_dilate_mode) noexcept {
        if (list == nullptr || source == nullptr || depth.texture == nullptr ||
            depth_staging == nullptr || depth_descriptor_heap == nullptr ||
            depth_root_signature == nullptr || depth_pipeline == nullptr)
            return E_POINTER;
        const auto source_description = source->GetDesc();
        if ((source_description.Format != depth_source_format ||
                source_description.Width != depth_source_width ||
                source_description.Height != depth_source_height) ||
            static_cast<std::uint64_t>(source_x) + depth_width >
                source_description.Width ||
            static_cast<std::uint64_t>(source_y) + depth_height >
                source_description.Height)
            return DXGI_ERROR_UNSUPPORTED;

        const auto transition = [](ID3D12Resource* resource,
            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            return barrier;
        };
        std::array<D3D12_RESOURCE_BARRIER, 2> before_copy{};
        UINT before_count = 0;
        if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            before_copy[before_count++] = transition(
                source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (depth_staging_state != D3D12_RESOURCE_STATE_COPY_DEST)
            before_copy[before_count++] = transition(depth_staging.Get(),
                depth_staging_state, D3D12_RESOURCE_STATE_COPY_DEST);
        if (before_count != 0) list->ResourceBarrier(before_count, before_copy.data());

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = depth_staging.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = source;
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source_location.SubresourceIndex = 0;
        // Depth/stencil planes require a full-subresource copy. Crop only
        // when sampling the private shader-readable depth plane.
        list->CopyTextureRegion(&destination, 0, 0, 0,
            &source_location, nullptr);

        std::array<D3D12_RESOURCE_BARRIER, 3> before_dispatch{};
        UINT dispatch_count = 0;
        if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            before_dispatch[dispatch_count++] = transition(source,
                D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
        before_dispatch[dispatch_count++] = transition(depth_staging.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        before_dispatch[dispatch_count++] = transition(depth.texture,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(dispatch_count, before_dispatch.data());
        depth_staging_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        ID3D12DescriptorHeap* heaps[] = {depth_descriptor_heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(depth_root_signature.Get());
        list->SetPipelineState(depth_pipeline.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            depth_descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(0, gpu);
        gpu.ptr += depth_descriptor_size;
        list->SetComputeRootDescriptorTable(1, gpu);
        const UINT depth_options[4]{depth_inverted ? 1U : 0U,
            static_cast<UINT>(depth_dilate_mode), source_x, source_y};
        list->SetComputeRoot32BitConstants(2, 4, depth_options, 0);
        list->Dispatch((depth_width + 7) / 8, (depth_height + 7) / 8, 1);

        std::array<D3D12_RESOURCE_BARRIER, 2> after_dispatch{};
        after_dispatch[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        after_dispatch[0].UAV.pResource = depth.texture;
        after_dispatch[1] = transition(depth.texture,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        list->ResourceBarrier(static_cast<UINT>(after_dispatch.size()),
            after_dispatch.data());
        return S_OK;
    }

    HRESULT ensure_motion(const D3D12_RESOURCE_DESC& description,
        UINT roi_width, UINT roi_height,
        std::uint64_t publication) noexcept {
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            description.Width == 0 || description.Width > INT_MAX ||
            description.Height == 0 || description.Height > INT_MAX ||
            roi_width == 0 || roi_height == 0 ||
            roi_width > description.Width || roi_height > description.Height ||
            description.DepthOrArraySize != 1 || description.SampleDesc.Count != 1 ||
            luke_motion_srv_format(description.Format) == DXGI_FORMAT_UNKNOWN ||
            (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
            return E_INVALIDARG;
        }
        if (motion_resource != nullptr &&
            motion_description.Width == roi_width &&
            motion_description.Height == roi_height &&
            motion_description.Format == kLukeAfwMotionFormat &&
            motion_description.Flags == description.Flags) {
            report_stage(AfwStage::motion_resource_created, S_FALSE,
                reinterpret_cast<std::uint64_t>(motion_resource.Get()), publication);
            report_stage(AfwStage::motion_descriptor_ready, S_FALSE,
                static_cast<std::uint64_t>(motion.srv_position), publication);
            return S_OK;
        }
        const auto setup = method<SetupTextureDescFn>(kSetupTextureDescSlot);
        if (setup == nullptr) return E_NOINTERFACE;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        // Composition reads the producer's typed float2 SRV and writes the
        // standard AFW RG16F texture. Preserve compatible producer flags, not
        // its precision; expose only the populated/cropped motion window.
        D3D12_RESOURCE_DESC clean = description;
        clean.Width = roi_width;
        clean.Height = roi_height;
        clean.Format = kLukeAfwMotionFormat;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        const HRESULT result = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &clean,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(resource.GetAddressOf()));
        report_stage(AfwStage::motion_resource_created, result,
            reinterpret_cast<std::uint64_t>(resource.Get()), publication);
        if (FAILED(result)) return result;

        TextureDesc descriptor{};
        descriptor.texture = resource.Get();
        descriptor.initial_state = D3D12_RESOURCE_STATE_COMMON;
        setup(renderer, descriptor);
        const HRESULT device_status = device->GetDeviceRemovedReason();
        const HRESULT descriptor_status = FAILED(device_status)
            ? device_status
            : descriptor.srv_position < 0 ||
                    descriptor.shader_resource_view.ptr == 0
                ? E_FAIL
                : S_OK;
        report_stage(AfwStage::motion_descriptor_ready, descriptor_status,
            static_cast<std::uint64_t>(descriptor.srv_position), publication);
        if (FAILED(descriptor_status)) return descriptor_status;
        motion_resource = std::move(resource);
        descriptor.texture = motion_resource.Get();
        motion = descriptor;
        motion_description = clean;
        return S_OK;
    }

    HRESULT ensure_composed_motion(UINT width, UINT height) noexcept {
        if (width == 0 || height == 0) return E_INVALIDARG;
        if (composed_motion_staging &&
            composed_motion_width == width &&
            composed_motion_height == height) return S_OK;

        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
        support.Format = kLukeAfwMotionFormat;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                &support, sizeof(support))) ||
            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
            return DXGI_ERROR_UNSUPPORTED;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = width;
        texture.Height = height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = kLukeAfwMotionFormat;
        texture.SampleDesc.Count = 1;
        texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        HRESULT result = device->CreateCommittedResource(&heap,
            D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(staging.GetAddressOf()));
        if (FAILED(result)) return result;

        D3D12_DESCRIPTOR_HEAP_DESC descriptor_description{};
        descriptor_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptor_description.NumDescriptors = 3;
        descriptor_description.Flags =
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
        result = device->CreateDescriptorHeap(&descriptor_description,
            IID_PPV_ARGS(descriptors.GetAddressOf()));
        if (FAILED(result)) return result;

        std::array<D3D12_DESCRIPTOR_RANGE, 3> ranges{};
        std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
        for (UINT index = 0; index < ranges.size(); ++index) {
            ranges[index].RangeType = index == 2
                ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[index].NumDescriptors = 1;
            ranges[index].BaseShaderRegister = index == 1 ? 1 : 0;
            ranges[index].OffsetInDescriptorsFromTableStart = 0;
            parameters[index].ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[index].DescriptorTable.NumDescriptorRanges = 1;
            parameters[index].DescriptorTable.pDescriptorRanges =
                &ranges[index];
            parameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        parameters[3].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[3].Constants.ShaderRegister = 0;
        parameters[3].Constants.Num32BitValues = 44;
        parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters =
            static_cast<UINT>(parameters.size());
        root_description.pParameters = parameters.data();
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        result = D3D12SerializeRootSignature(&root_description,
            D3D_ROOT_SIGNATURE_VERSION_1, serialized.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        result = device->CreateRootSignature(queue->GetDesc().NodeMask,
            serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(root.GetAddressOf()));
        if (FAILED(result)) return result;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description{};
        pipeline_description.pRootSignature = root.Get();
        pipeline_description.CS = {g_xrfg_luke_compose_motion_shader,
            sizeof(g_xrfg_luke_compose_motion_shader)};
        pipeline_description.NodeMask = queue->GetDesc().NodeMask;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        result = device->CreateComputePipelineState(&pipeline_description,
            IID_PPV_ARGS(pipeline.GetAddressOf()));
        if (FAILED(result)) return result;

        const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            descriptors->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += 2 * descriptor_size;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = kLukeAfwMotionFormat;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(staging.Get(), nullptr, &uav, cpu);
        result = device->GetDeviceRemovedReason();
        if (FAILED(result)) return result;
        composed_motion_staging = std::move(staging);
        composed_motion_descriptors = std::move(descriptors);
        composed_motion_root = std::move(root);
        composed_motion_pipeline = std::move(pipeline);
        composed_motion_descriptor_size = descriptor_size;
        composed_motion_width = width;
        composed_motion_height = height;
        return S_OK;
    }

    HRESULT compose_motion(ID3D12GraphicsCommandList* list,
        const DlssMotionVectorFrame& guide,
        const LukeNativeMotionConstants& constants, UINT crop_x,
        UINT crop_y, UINT width, UINT height) noexcept {
        if (!list || !guide.motion_vectors || !depth.texture ||
            !motion_resource ||
            !composed_motion_staging || !composed_motion_descriptors ||
            !composed_motion_root || !composed_motion_pipeline ||
            crop_x + width > guide.motion_width ||
            crop_y + height > guide.motion_height)
            return E_INVALIDARG;

        for(unsigned index=0; index<2; ++index) {
            ID3D12Resource* field=index==0?guide.motion_vectors.Get():depth.texture;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format=index==0?luke_motion_srv_format(field->GetDesc().Format):DXGI_FORMAT_R32_FLOAT;
            if(srv.Format==DXGI_FORMAT_UNKNOWN) return DXGI_ERROR_UNSUPPORTED;
            srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels=1;
            auto cpu=composed_motion_descriptors->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr+=index*composed_motion_descriptor_size;
            device->CreateShaderResourceView(field,&srv,cpu);
        }
        const HRESULT descriptor_status=device->GetDeviceRemovedReason();
        if(FAILED(descriptor_status)) return descriptor_status;

        const auto transition = [](ID3D12Resource* resource,
            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            return barrier;
        };
        auto to_uav = transition(composed_motion_staging.Get(),
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &to_uav);
        ID3D12DescriptorHeap* heaps[] =
            {composed_motion_descriptors.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(composed_motion_root.Get());
        list->SetPipelineState(composed_motion_pipeline.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            composed_motion_descriptors->GetGPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < 3; ++index) {
            list->SetComputeRootDescriptorTable(index, gpu);
            gpu.ptr += composed_motion_descriptor_size;
        }
        list->SetComputeRoot32BitConstants(3, 44, &constants, 0);
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

        std::array<D3D12_RESOURCE_BARRIER, 3> before_copy{};
        before_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        before_copy[0].UAV.pResource = composed_motion_staging.Get();
        before_copy[1] = transition(composed_motion_staging.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        before_copy[2] = transition(motion_resource.Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(static_cast<UINT>(before_copy.size()),
            before_copy.data());
        list->CopyResource(motion_resource.Get(),
            composed_motion_staging.Get());
        std::array<D3D12_RESOURCE_BARRIER, 2> after_copy{
            transition(composed_motion_staging.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON),
            transition(motion_resource.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COMMON)};
        list->ResourceBarrier(static_cast<UINT>(after_copy.size()),
            after_copy.data());
        return S_OK;
    }
};

LukeAfwSynthesizer::LukeAfwSynthesizer() noexcept = default;
LukeAfwSynthesizer::~LukeAfwSynthesizer() = default;

HRESULT LukeAfwSynthesizer::initialize(ID3D12Device* device,
    ID3D12CommandQueue* queue, const std::filesystem::path& plugin_path) noexcept {
    try {
        if (device == nullptr || queue == nullptr || plugin_path.empty()) return E_INVALIDARG;
        std::scoped_lock lock(mutex_);
        auto candidate = std::make_unique<Impl>();
        candidate->world_scale = luke_world_scale(plugin_path);
        if (candidate->world_scale == 0.0F) return E_INVALIDARG;
        candidate->module = LoadLibraryExW(plugin_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (candidate->module == nullptr) return HRESULT_FROM_WIN32(GetLastError());
        candidate->init_device = reinterpret_cast<InitDeviceFn>(
            GetProcAddress(candidate->module, "InitDevice"));
        candidate->init_frame_warp = reinterpret_cast<InitFrameWarpFn>(
            GetProcAddress(candidate->module, "InitFrameWarp"));
        candidate->evaluate = reinterpret_cast<EvaluateFrameWarpFn>(
            GetProcAddress(candidate->module, "EvaluateFrameWarp"));
        if (candidate->init_device == nullptr || candidate->init_frame_warp == nullptr ||
            candidate->evaluate == nullptr) return E_NOINTERFACE;
        DeviceParams params{};
        params.d3d12_device = device;
        params.d3d12_queue = queue;
        candidate->renderer = candidate->init_device(params);
        if (candidate->renderer == nullptr) return E_FAIL;
        const D3D12_COMMAND_QUEUE_DESC queue_description = queue->GetDesc();
        HRESULT result = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(candidate->command_allocator.GetAddressOf()));
        if (FAILED(result)) return result;
        result = device->CreateCommandList(queue_description.NodeMask,
            D3D12_COMMAND_LIST_TYPE_DIRECT, candidate->command_allocator.Get(),
            nullptr, IID_PPV_ARGS(candidate->command_list.GetAddressOf()));
        if (FAILED(result)) return result;
        result = candidate->command_list->Close();
        if (FAILED(result)) return result;
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(candidate->submission_fence.GetAddressOf()));
        if (FAILED(result)) return result;
        candidate->submission_event = CreateEventW(
            nullptr, FALSE, FALSE, nullptr);
        if (candidate->submission_event == nullptr)
            return HRESULT_FROM_WIN32(GetLastError());
        candidate->device = device;
        candidate->queue = queue;
        impl_ = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT LukeAfwSynthesizer::synthesize(ID3D12Resource* source_color,
    ID3D12Resource* destination_color, const DlssMotionVectorFrame& guide,
    const D3D12ReprojectionView& source_view,
    const D3D12ReprojectionView& destination_view,
    std::uint32_t source_eye,
    float xr_eye_baseline,
    const embedded::AfwSettings& settings) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        report_stage(AfwStage::enter, S_OK, source_eye, guide.publication);
        if (!impl_ || source_color == nullptr || destination_color == nullptr ||
            source_eye > 1 || guide.motion_vectors == nullptr ||
            guide.depth == nullptr ||
            settings.mode < 1 || settings.mode > 3)
            return E_INVALIDARG;
        report_stage(AfwStage::arguments_valid, S_OK, source_eye,
            guide.publication);
        const auto color_desc = source_color->GetDesc();
        const auto destination_desc = destination_color->GetDesc();
        const auto depth_desc = guide.depth->GetDesc();
        const auto motion_desc = guide.motion_vectors->GetDesc();
        const std::uint64_t color_extent =
            (static_cast<std::uint64_t>(color_desc.Width) << 32) |
            color_desc.Height;
        report_stage(AfwStage::descriptions_ready,
            static_cast<std::int64_t>(color_desc.Format), color_extent,
            guide.publication);
        const auto reject_input = [&](std::uint32_t reason) {
            // At most one complete diagnostic per reason/second, so the
            // circular flight log retains the cause. No change
            // to admission, images, synchronization or plugin arguments.
            auto& logger = bridge_flight_logger();
            if (logger.enabled() || logger.ram_enabled()) {
                const auto now = GetTickCount64();
                auto& last = impl_->input_rejection_report_ticks[reason];
                if (last && now - last < 1000) return DXGI_ERROR_UNSUPPORTED;
                last = now;
                logger.event(BridgeFlightOperation::luke_afw_stage,
                    reason, 95, guide.publication, source_eye);
                const auto extent = [](std::uint64_t w, std::uint32_t h) {
                    return (w << 32) | h;
                };
                const D3D12_RESOURCE_DESC descriptors[]{color_desc, destination_desc, depth_desc, motion_desc};
                for (std::uint32_t slot = 0; slot < 4; ++slot) {
                    const auto& d = descriptors[slot];
                    logger.event(BridgeFlightOperation::luke_afw_stage,
                        slot, 96, extent(d.Width, d.Height),
                        static_cast<std::uint64_t>(d.Format) |
                        (static_cast<std::uint64_t>(d.Dimension) << 16) |
                        (static_cast<std::uint64_t>(d.DepthOrArraySize) << 24) |
                        (static_cast<std::uint64_t>(d.SampleDesc.Count) << 40));
                }
                logger.event(BridgeFlightOperation::luke_afw_stage, 0, 97,
                    extent(guide.output_width, guide.output_height), extent(guide.output_x, guide.output_y));
                logger.event(BridgeFlightOperation::luke_afw_stage, 1, 97,
                    extent(guide.motion_width, guide.motion_height), extent(guide.motion_x, guide.motion_y));
                logger.event(BridgeFlightOperation::luke_afw_stage, 2, 97,
                    extent(guide.depth_width, guide.depth_height), extent(guide.depth_x, guide.depth_y));
                const auto floats = [](float x, float y) {
                    std::uint32_t a{}, b{};
                    std::memcpy(&a, &x, sizeof(a)); std::memcpy(&b, &y, sizeof(b));
                    return (static_cast<std::uint64_t>(a) << 32) | b;
                };
                for (std::uint32_t row = 0; row < 8; ++row)
                    logger.event(BridgeFlightOperation::luke_afw_stage, row, 98,
                        floats(guide.camera_view_to_clip[row * 2], guide.camera_view_to_clip[row * 2 + 1]),
                        guide.publication);
                logger.event(BridgeFlightOperation::luke_afw_stage, 0, 99,
                    floats(source_view.fov.angle_left, source_view.fov.angle_right),
                    floats(source_view.fov.angle_up, source_view.fov.angle_down));
            }
            return DXGI_ERROR_UNSUPPORTED;
        };
        if (!compatible_color(color_desc, destination_desc)) return reject_input(1);
        if (color_desc.Width > INT_MAX || color_desc.Height > INT_MAX) return reject_input(2);
        if (!guide.motion_width || !guide.motion_height || !guide.output_width || !guide.output_height)
            return reject_input(3);
        if (color_desc.Width > guide.output_width || color_desc.Height > guide.output_height)
            return reject_input(4);
        if (luke_motion_srv_format(motion_desc.Format) == DXGI_FORMAT_UNKNOWN) return reject_input(5);
        if (depth_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            motion_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return reject_input(6);
        if (depth_desc.DepthOrArraySize != 1 || motion_desc.DepthOrArraySize != 1) return reject_input(7);
        if (guide.depth_x + guide.depth_width > depth_desc.Width ||
            guide.depth_y + guide.depth_height > depth_desc.Height) return reject_input(8);
        if (guide.motion_x + guide.motion_width > motion_desc.Width ||
            guide.motion_y + guide.motion_height > motion_desc.Height) return reject_input(9);

        // The final OpenXR eye FOV is slightly inset from the apparent edge
        // crop of Luke's square DLSS output. Project that physical FOV through
        // the producer's exact matrix and take the same viewport from depth
        // and motion. At 3344/1672 this is about X30/X345, not X0/X375;
        // the numbers are derived, never stored as calibration offsets.
        LukeGuideWindowFailure depth_failure{}, motion_failure{};
        const auto depth_window = luke_guide_window(
            guide.camera_view_to_clip,
            source_view.fov.angle_left, source_view.fov.angle_right,
            source_view.fov.angle_up, source_view.fov.angle_down,
            guide.output_width, guide.output_height,
            guide.depth_width, guide.depth_height,
            static_cast<UINT>(color_desc.Width), color_desc.Height, &depth_failure);
        const auto motion_window = luke_guide_window(
            guide.camera_view_to_clip,
            source_view.fov.angle_left, source_view.fov.angle_right,
            source_view.fov.angle_up, source_view.fov.angle_down,
            guide.output_width, guide.output_height,
            guide.motion_width, guide.motion_height,
            static_cast<UINT>(color_desc.Width), color_desc.Height);
        if (!depth_window) return reject_input(16 + static_cast<std::uint32_t>(depth_failure));
        if (!motion_window) return reject_input(32 + static_cast<std::uint32_t>(motion_failure));
        const UINT cropped_depth_width = depth_window->width;
        const UINT cropped_depth_height = depth_window->height;
        const UINT cropped_motion_width = motion_window->width;
        const UINT cropped_motion_height = motion_window->height;
        const UINT depth_crop_x = depth_window->x;
        const UINT depth_crop_y = depth_window->y;
        const UINT motion_crop_x = motion_window->x;
        const UINT motion_crop_y = motion_window->y;

        if(cropped_depth_width!=cropped_motion_width || cropped_depth_height!=cropped_motion_height ||
            depth_crop_x!=motion_crop_x || depth_crop_y!=motion_crop_y) return reject_input(42);
        LukeNativeMotionConstants native_motion{};unsigned native_failure{};
        const bool have_native_motion=luke_native_motion_constants(guide,motion_crop_x,motion_crop_y,native_motion,native_failure);
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,native_failure,950,guide.publication,
            guide.previous_same_native_camera?guide.previous_same_native_camera->publication:0);
        if(!have_native_motion) return HRESULT_FROM_WIN32(ERROR_NOT_READY);

        // The allocator, plugin eye buffers and private depth/motion targets
        // are reused by the next evaluation. V23181 skipped that evaluation
        // whenever the previous GPU submission had not yet retired, visibly
        // exposing the unwarped AER peer. Wait before touching any of them.
        if (impl_->last_submission_fence != 0 &&
            impl_->submission_fence->GetCompletedValue() <
                impl_->last_submission_fence) {
            report_stage(AfwStage::allocator_wait_begin, S_OK,
                impl_->last_submission_fence, guide.publication);
            const HRESULT event_result =
                impl_->submission_fence->SetEventOnCompletion(
                    impl_->last_submission_fence, impl_->submission_event);
            if (FAILED(event_result)) return event_result;
            constexpr DWORD kAllocatorWaitMilliseconds = 250;
            const DWORD wait_result = WaitForSingleObject(
                impl_->submission_event, kAllocatorWaitMilliseconds);
            const HRESULT completion = wait_result == WAIT_OBJECT_0
                ? S_OK
                : wait_result == WAIT_TIMEOUT
                    ? DXGI_ERROR_WAS_STILL_DRAWING
                    : HRESULT_FROM_WIN32(GetLastError());
            report_stage(AfwStage::allocator_wait_end, completion,
                wait_result, guide.publication);
            if (FAILED(completion)) return completion;
        }

        if (!std::isfinite(xr_eye_baseline) || xr_eye_baseline < .01F || xr_eye_baseline > .20F)
            return E_INVALIDARG;
        if (impl_->unit_epoch != guide.epoch) {
            impl_->unit_epoch = guide.epoch;
            impl_->calibrated_units_per_metre = 0;
        }
        if (impl_->calibrated_units_per_metre == 0 && guide.calibrated_game_eye_baseline > 0)
            impl_->calibrated_units_per_metre = guide.calibrated_game_eye_baseline * impl_->world_scale / xr_eye_baseline;
        const float units = impl_->calibrated_units_per_metre;
        float game_baseline{};
        const bool units_ready = luke_game_eye_baseline(xr_eye_baseline, impl_->world_scale, units, game_baseline);
        std::uint32_t baseline_bits_units{}, units_bits{};
        std::memcpy(&baseline_bits_units, &game_baseline, sizeof(baseline_bits_units));
        std::memcpy(&units_bits, &units, sizeof(units_bits));
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            units_ready ? S_OK : S_FALSE, 104, guide.publication,
            (std::uint64_t(baseline_bits_units) << 32) | units_bits);
        if (!units_ready) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        CameraData camera{};
        if (!camera_data(source_view, destination_view, guide, source_eye,
                         impl_->world_scale, xr_eye_baseline, units, camera)) return E_INVALIDARG;
        std::uint32_t baseline_bits{};
        std::memcpy(&baseline_bits, &xr_eye_baseline, sizeof(baseline_bits));
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            source_eye, 88, guide.publication, baseline_bits);
        if (bridge_flight_logger().ram_enabled()) {
            const auto pair = [](float x, float y) noexcept {
                std::uint32_t a{}, b{};
                std::memcpy(&a, &x, sizeof(a)); std::memcpy(&b, &y, sizeof(b));
                return (static_cast<std::uint64_t>(a) << 32) | b;
            };
            const Matrix4x4* matrices[]{&camera.source_view_to_world,
                &camera.destination_view_to_world, &camera.source_view_to_clip,
                &camera.destination_view_to_clip};
            for (int matrix = 0; matrix < 4; ++matrix)
                for (int row = 0; row < 4; ++row) {
                    const auto* v = matrices[matrix]->values + row * 4;
                    bridge_flight_logger().event(BridgeFlightOperation::luke_render_camera,
                        source_eye * 100 + matrix * 10 + row, guide.publication,
                        pair(v[0], v[1]), pair(v[2], v[3]));
                }
            bridge_flight_logger().event(BridgeFlightOperation::luke_render_camera,
                source_eye * 100 + 40, guide.publication,
                pair(impl_->world_scale, guide.camera_fov_vertical), 0);
        }
        report_stage(AfwStage::camera_ready, S_OK, source_eye,
            guide.publication);

        if (!impl_->frame_buffers_ready ||
            !compatible_color(impl_->color_description, color_desc)) {
            FrameWarpInitParams params{};
            params.hmd_width = static_cast<int>(color_desc.Width);
            params.hmd_height = static_cast<int>(color_desc.Height);
            // PureDark allocates typed UAV-capable internal eye textures.
            // Horizon exposes the backing resource as RGBA8 typeless (DXGI 27)
            // and its OpenXR view as sRGB. Use the copy-compatible typed linear
            // sibling only inside AFW; application resources and presentation
            // remain unchanged.
            params.eye_format = linear_color_format(color_desc.Format);
            params.backbuffer_format = params.eye_format;
            report_stage(AfwStage::frame_warp_init_begin,
                static_cast<std::int64_t>(params.eye_format), color_extent,
                guide.publication);
            impl_->eye_buffers = impl_->init_frame_warp(params);
            report_stage(AfwStage::frame_warp_init_end, S_OK, source_eye,
                guide.publication);
            if (impl_->eye_buffers.eye_frame_buffers[0].color.texture == nullptr ||
                impl_->eye_buffers.eye_frame_buffers[1].color.texture == nullptr)
                return E_FAIL;
            impl_->color_description = color_desc;
            impl_->frame_buffers_ready = true;
        }
        if (impl_->depth.texture == nullptr ||
            impl_->depth_width != cropped_depth_width ||
            impl_->depth_height != cropped_depth_height ||
            impl_->depth_source_format != depth_desc.Format ||
            impl_->depth_source_width != depth_desc.Width ||
            impl_->depth_source_height != depth_desc.Height) {
            report_stage(AfwStage::depth_create_begin,
                static_cast<std::int64_t>(depth_desc.Format), source_eye,
                guide.publication);
            HRESULT result = impl_->ensure_float_depth(
                cropped_depth_width, cropped_depth_height, depth_desc);
            const HRESULT device_status = impl_->device->GetDeviceRemovedReason();
            if (SUCCEEDED(result) && FAILED(device_status)) result = device_status;
            report_stage(AfwStage::depth_create_end, result, source_eye,
                guide.publication);
            if (FAILED(result)) return result;
        }
        // PureDark's CreateTexture helper removes Horizon's wrapped D3D12
        // device for RG16F, while binding the producer snapshot directly is
        // rejected inside EvaluateFrameWarp. Allocate an exact private clone
        // of the working producer contract, then register it with PureDark.
        report_stage(AfwStage::motion_allocate_begin,
            static_cast<std::int64_t>(motion_desc.Format),
            reinterpret_cast<std::uint64_t>(guide.motion_vectors.Get()),
            guide.publication);
        HRESULT result = impl_->ensure_motion(motion_desc,
            cropped_motion_width, cropped_motion_height, guide.publication);
        if (FAILED(result)) return result;
        result = impl_->ensure_composed_motion(cropped_motion_width,
            cropped_motion_height);
        if (FAILED(result)) return result;
        report_stage(AfwStage::resources_ready, S_OK, source_eye,
            guide.publication);
        if(guide.producer_fence && guide.producer_fence_value) {
            const HRESULT waited=impl_->queue->Wait(guide.producer_fence.Get(),guide.producer_fence_value);
            if(FAILED(waited)) return waited;
        }
        report_stage(AfwStage::producer_waited, S_OK, source_eye,
            guide.publication);
        const auto copy = impl_->method<CopyTextureFn>(kCopyTextureSlot);
        if (copy == nullptr) return E_NOINTERFACE;
        result = impl_->command_allocator->Reset();
        if (FAILED(result)) {
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, result, 100, source_eye, guide.publication);
            return result;
        }
        result = impl_->command_list->Reset(
            impl_->command_allocator.Get(), nullptr);
        if (FAILED(result)) {
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, result, 101, source_eye, guide.publication);
            return result;
        }
        const auto initial_depth_state = impl_->depth_staging_state;
        LukeRecordingScope recording(impl_->command_list.Get(), [&](HRESULT aborted_close) noexcept {
            impl_->depth_staging_state = initial_depth_state;
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, result, 102, source_eye, guide.publication);
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage, aborted_close, 103, source_eye, guide.publication);
        });
        ID3D12GraphicsCommandList* command_list = impl_->command_list.Get();
        report_stage(AfwStage::command_list_reset, S_OK, source_eye,
            guide.publication);
        if (two_frame_capture_active()) {
            if (guide.output != nullptr) {
                TwoFrameImageInfo output{};
                output.stage = "afw_ngx_output_at_consume";
                output.stream = guide.stream;
                output.capture_serial = guide.publication;
                output.eye = static_cast<std::int32_t>(source_eye);
                output.x = guide.output_x;
                output.y = guide.output_y;
                output.width = guide.output_width;
                output.height = guide.output_height;
                two_frame_capture_record_ng_x(output, impl_->device.Get(),
                    command_list, guide.output.Get(),
                    D3D12_RESOURCE_STATE_COMMON);
            }
            TwoFrameImageInfo mv{};
            mv.stage = "afw_ngx_mv_before_crop";
            mv.stream = guide.stream;
            mv.capture_serial = guide.publication;
            mv.eye = static_cast<std::int32_t>(source_eye);
            mv.x = guide.motion_x;
            mv.y = guide.motion_y;
            mv.width = guide.motion_width;
            mv.height = guide.motion_height;
            mv.scale_x = guide.scale_x;
            mv.scale_y = guide.scale_y;
            mv.jitter_x = guide.jitter_x;
            mv.jitter_y = guide.jitter_y;
            two_frame_capture_record_ng_x(mv, impl_->device.Get(),
                command_list, guide.motion_vectors.Get(), guide.resource_state);
            TwoFrameImageInfo color{};
            color.stage = "afw_source_color";
            color.stream = guide.stream;
            color.capture_serial = guide.publication;
            color.eye = static_cast<std::int32_t>(source_eye);
            color.width = static_cast<std::uint32_t>(color_desc.Width);
            color.height = color_desc.Height;
            two_frame_capture_record_ng_x(color, impl_->device.Get(),
                command_list, source_color, D3D12_RESOURCE_STATE_COMMON);
        }

        FrameBufferDesc input = impl_->eye_buffers.eye_frame_buffers[source_eye];
        if (two_frame_capture_active()) {
            const auto capture_prior_eye = [&](std::uint32_t eye,
                const char* stage) {
                const TextureDesc& prior =
                    impl_->eye_buffers.eye_frame_buffers[eye].color;
                if (prior.texture == nullptr) return;
                const auto extent = prior.texture->GetDesc();
                TwoFrameImageInfo image{};
                image.stage = stage;
                image.stream = guide.stream;
                image.capture_serial = guide.publication;
                image.eye = static_cast<std::int32_t>(eye);
                image.width = static_cast<std::uint32_t>(extent.Width);
                image.height = extent.Height;
                two_frame_capture_record_ng_x(image, impl_->device.Get(),
                    command_list, prior.texture, prior.initial_state);
            };
            capture_prior_eye(source_eye, "afw_prior_source_eye_color");
            capture_prior_eye(1U - source_eye, "afw_prior_peer_eye_color");
        }
        TextureDesc color_source{};
        color_source.texture = source_color;
        color_source.initial_state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_BOX color_box{0, 0, 0, static_cast<UINT>(color_desc.Width),
            color_desc.Height, 1};
        copy(impl_->renderer, command_list, input.color, color_source,
            color_box, 0, 0);
        report_stage(AfwStage::color_copied, S_OK, source_eye,
            guide.publication);

        result = impl_->convert_depth(command_list, guide.depth.Get(),
            guide.depth_resource_state,
            guide.depth_x + depth_crop_x, guide.depth_y + depth_crop_y,
            guide.depth_inverted, settings.depth_dilate_mode);
        report_stage(AfwStage::depth_converted, result, source_eye,
            guide.publication);
        if (FAILED(result)) return result;
        // Preserve native object flow. Rebase only depth-projected camera motion
        // from the opposite N-1 reference to the exact native same-eye N-2 camera.
        result = impl_->compose_motion(command_list, guide, native_motion,
            motion_crop_x, motion_crop_y, cropped_motion_width,
            cropped_motion_height);
        report_stage(AfwStage::motion_copied, result, source_eye,
            guide.publication);
        if (FAILED(result)) return result;

        input.depth = impl_->depth;
        input.motion_vectors = impl_->motion;
        if (two_frame_capture_active()) {
            const auto capture_plugin_input = [&](const TextureDesc& texture,
                const char* stage) {
                if (texture.texture == nullptr) return;
                const auto extent = texture.texture->GetDesc();
                TwoFrameImageInfo image{};
                image.stage = stage;
                image.stream = guide.stream;
                image.capture_serial = guide.publication;
                image.eye = static_cast<std::int32_t>(source_eye);
                image.width = static_cast<std::uint32_t>(extent.Width);
                image.height = extent.Height;
                two_frame_capture_record_ng_x(image, impl_->device.Get(),
                    command_list, texture.texture, texture.initial_state);
            };
            capture_plugin_input(input.color, "afw_plugin_input_color");
            capture_plugin_input(input.depth, "afw_plugin_input_depth");
            capture_plugin_input(input.motion_vectors, "afw_plugin_input_mv");
        }

        FrameWarpEvaluateParams params{};
        params.command_list = command_list;
        params.input_eye_frame_buffer = &input;
        params.motion_scale[0] = guide.scale_x *
            static_cast<float>(guide.output_width) /
            static_cast<float>(guide.motion_width);
        params.motion_scale[1] = guide.scale_y *
            static_cast<float>(guide.output_height) /
            static_cast<float>(guide.motion_height);
        params.mode = static_cast<FrameWarpMode>(settings.mode);
        const std::uint32_t interpreted_eye = source_eye;
        params.eye_index = source_eye == 0 ? EyeLeft : EyeRight;
        params.camera_data = &camera;
        params.clear_before_warping = settings.clear_before_warping;
        params.ignore_motion_threshold = settings.ignore_motion_threshold;
        params.is_hudless_color = true;
        params.motion_vectors_type = Normal;
        params.debug = settings.debug;
        if (two_frame_capture_active()) {
            TwoFrameGuideContract contract{};
            contract.stage = "afw_evaluate";
            contract.stream = guide.stream;
            contract.publication = guide.publication;
            contract.serial = guide.serial;
            contract.previous_serial = guide.previous_serial;
            contract.source_eye = static_cast<std::int32_t>(source_eye);
            contract.destination_eye = static_cast<std::int32_t>(1U - source_eye);
            contract.mode = settings.mode;
            contract.motion_vectors_type = params.motion_vectors_type;
            contract.ignore_motion_threshold =
                params.ignore_motion_threshold;
            contract.camera_fov_vertical = guide.camera_fov_vertical;
            contract.debug = params.debug;
            contract.depth_dilate_mode = settings.depth_dilate_mode;
            contract.clear_before_warping =
                params.clear_before_warping;
            contract.is_hudless_color = params.is_hudless_color;
            contract.motion_width = guide.motion_width;
            contract.motion_height = guide.motion_height;
            contract.depth_width = guide.depth_width;
            contract.depth_height = guide.depth_height;
            contract.output_width = guide.output_width;
            contract.output_height = guide.output_height;
            const auto input_color_desc = input.color.texture->GetDesc();
            const auto input_depth_desc = input.depth.texture->GetDesc();
            const auto input_motion_desc =
                input.motion_vectors.texture->GetDesc();
            contract.input_color_width =
                static_cast<std::uint32_t>(input_color_desc.Width);
            contract.input_color_height = input_color_desc.Height;
            contract.input_depth_width =
                static_cast<std::uint32_t>(input_depth_desc.Width);
            contract.input_depth_height = input_depth_desc.Height;
            contract.input_motion_width =
                static_cast<std::uint32_t>(input_motion_desc.Width);
            contract.input_motion_height = input_motion_desc.Height;
            contract.output_color_width =
                static_cast<std::uint32_t>(destination_desc.Width);
            contract.output_color_height = destination_desc.Height;
            contract.depth_crop_x = depth_crop_x;
            contract.depth_crop_y = depth_crop_y;
            contract.motion_crop_x = motion_crop_x;
            contract.motion_crop_y = motion_crop_y;
            contract.scale_x = guide.scale_x;
            contract.scale_y = guide.scale_y;
            contract.jitter_x = guide.jitter_x;
            contract.jitter_y = guide.jitter_y;
            contract.motion_scale_x = params.motion_scale[0];
            contract.motion_scale_y = params.motion_scale[1];
            contract.frame_time_delta_ms = guide.frame_time_delta_ms;
            contract.camera_near = guide.camera_near;
            contract.camera_far = guide.camera_far;
            contract.reset = guide.reset;
            contract.jittered = guide.jittered;
            contract.depth_inverted = guide.depth_inverted;
            contract.depth_infinite = guide.depth_infinite;
            contract.native_previous_publication=guide.previous_opposite_guide->publication;
            contract.native_same_publication=guide.previous_same_native_camera->publication;
            contract.native_clip_to_opposite=native_motion.opposite;
            contract.native_clip_to_same=native_motion.same;
            contract.native_history_jitter_x=native_motion.jitter_difference[0];
            contract.native_history_jitter_y=native_motion.jitter_difference[1];
            contract.raw_camera_motion_included = guide.raw_camera_motion_included;
            contract.raw_clip_to_previous_clip = guide.raw_clip_to_previous_clip;
            contract.raw_previous_clip_to_clip = guide.raw_previous_clip_to_clip;
            contract.camera_position = guide.camera_position;
            contract.camera_right = guide.camera_right;
            contract.camera_up = guide.camera_up;
            std::copy(camera.source_view_to_world.values + 8,
                camera.source_view_to_world.values + 11, contract.camera_forward.begin());
            contract.source_pose = {source_view.pose.position.x,
                source_view.pose.position.y, source_view.pose.position.z};
            contract.destination_pose = {destination_view.pose.position.x,
                destination_view.pose.position.y, destination_view.pose.position.z};
            contract.source_orientation = {source_view.pose.orientation.x,
                source_view.pose.orientation.y, source_view.pose.orientation.z,
                source_view.pose.orientation.w};
            contract.destination_orientation = {
                destination_view.pose.orientation.x,
                destination_view.pose.orientation.y,
                destination_view.pose.orientation.z,
                destination_view.pose.orientation.w};
            contract.view_to_clip = guide.camera_view_to_clip;
            contract.clip_to_view = guide.camera_clip_to_view;
            const auto copy_matrix = [](const Matrix4x4& source,
                std::array<float, 16>& destination) {
                std::copy(std::begin(source.values), std::end(source.values),
                    destination.begin());
            };
            copy_matrix(camera.source_world_to_view,
                contract.source_world_to_view);
            copy_matrix(camera.source_view_to_world,
                contract.source_view_to_world);
            copy_matrix(camera.source_view_to_clip,
                contract.source_view_to_clip);
            copy_matrix(camera.source_clip_to_view,
                contract.source_clip_to_view);
            copy_matrix(camera.destination_world_to_view,
                contract.destination_world_to_view);
            copy_matrix(camera.destination_view_to_world,
                contract.destination_view_to_world);
            copy_matrix(camera.destination_view_to_clip,
                contract.destination_view_to_clip);
            copy_matrix(camera.destination_clip_to_view,
                contract.destination_clip_to_view);
            copy_matrix(camera.camera_world_to_view,
                contract.camera_world_to_view);
            copy_matrix(camera.camera_view_to_world,
                contract.camera_view_to_world);
            copy_matrix(camera.camera_view_to_clip,
                contract.camera_view_to_clip);
            copy_matrix(camera.camera_clip_to_view,
                contract.camera_clip_to_view);
            two_frame_capture_guide_contract(contract);
        }
        const std::uint64_t configuration =
            (static_cast<std::uint64_t>(settings.mode) << 0) |
            (static_cast<std::uint64_t>(settings.debug) << 12) |
            (static_cast<std::uint64_t>(settings.clear_before_warping) << 13);
        report_stage(AfwStage::configuration,
            static_cast<std::int64_t>(configuration), interpreted_eye,
            guide.publication);
        report_stage(AfwStage::evaluate_begin, S_OK, source_eye,
            guide.publication);
        impl_->evaluate(params);
        report_stage(AfwStage::evaluate_end, S_OK, source_eye,
            guide.publication);
        result = S_OK;
        if (params.output_eye_frame_buffer == nullptr ||
            params.output_eye_frame_buffer->color.texture == nullptr) {
            result = E_FAIL;
        } else {
            TextureDesc destination{};
            destination.texture = destination_color;
            destination.initial_state = D3D12_RESOURCE_STATE_COMMON;
            TextureDesc generated = params.output_eye_frame_buffer->color;
            const auto generated_desc = generated.texture->GetDesc();
            if (!copy_compatible_color(generated_desc, destination_desc)) {
                result = E_INVALIDARG;
            } else {
                copy(impl_->renderer, command_list, destination, generated,
                    color_box, 0, 0);
                report_stage(AfwStage::output_copied, S_OK, source_eye,
                    guide.publication);
                if (two_frame_capture_active()) {
                    TwoFrameImageInfo output{};
                    output.stage = "afw_generated_eye";
                    output.stream = guide.stream;
                    output.capture_serial = guide.publication;
                    output.eye = static_cast<std::int32_t>(1U - source_eye);
                    output.width = static_cast<std::uint32_t>(color_desc.Width);
                    output.height = color_desc.Height;
                    two_frame_capture_record_ng_x(output, impl_->device.Get(),
                        command_list, destination_color,
                        D3D12_RESOURCE_STATE_COMMON);
                }
            }
        }
        const HRESULT close_result = recording.close();
        report_stage(AfwStage::command_list_closed, close_result, source_eye,
            guide.publication);
        if (FAILED(close_result) || FAILED(result)) {
            impl_->depth_staging_state = initial_depth_state;
            return FAILED(close_result) ? close_result : result;
        }
        ID3D12CommandList* lists[] = {command_list};
        guide.motion_retention->begin_gpu_read();
        if (guide.depth_retention) guide.depth_retention->begin_gpu_read();
        impl_->queue->ExecuteCommandLists(1, lists);
        if (two_frame_capture_active())
            two_frame_capture_submission(impl_->queue.Get(), 1, lists);
        report_stage(AfwStage::command_list_executed, S_OK, source_eye,
            guide.publication);
        const std::uint64_t fence_value = impl_->next_submission_fence++;
        result = impl_->queue->Signal(impl_->submission_fence.Get(), fence_value);
        report_stage(AfwStage::fence_signaled, result, source_eye,
            guide.publication);
        if (SUCCEEDED(result)) {
            for(const auto& retention:{guide.motion_retention,guide.depth_retention}) {
                if(!retention) continue;
                const HRESULT retired=retention->retire_gpu_read(impl_->submission_fence.Get(),fence_value);
                if(FAILED(retired) && SUCCEEDED(result)) result=retired;
            }
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                result, 87, guide.publication, fence_value);
        }
        if (SUCCEEDED(result)) {
            impl_->last_submission_fence = fence_value;
        }
        return result;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

bool LukeAfwSynthesizer::initialized() const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ != nullptr;
}

}  // namespace xrfg
