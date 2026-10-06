#pragma once
#include <nvsdk_ngx.h>
#include <initializer_list>

namespace luke_vulkan {
// Copy only the metadata consumed by the existing guide/completion contract.
// Resources are independently bound as D3D12 pointers in a local parameter map;
// never ask a Vulkan NGX implementation for its unsupported D3D12 overloads.
template<class Source, class Destination>
void copy_capture_metadata(const Source& source, Destination& destination) {
    for (auto key : {NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
        NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight,
        NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
        NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
        NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
        NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags}) {
        unsigned value{};
        if (source.Get(key, &value) == NVSDK_NGX_Result_Success) destination.Set(key, value);
    }
    for (auto key : {NVSDK_NGX_Parameter_MV_Offset_X, NVSDK_NGX_Parameter_MV_Offset_Y,
        NVSDK_NGX_Parameter_MV_Scale_X, NVSDK_NGX_Parameter_MV_Scale_Y,
        NVSDK_NGX_Parameter_Jitter_Offset_X, NVSDK_NGX_Parameter_Jitter_Offset_Y,
        NVSDK_NGX_Parameter_FrameTimeDeltaInMsec}) {
        float value{};
        if (source.Get(key, &value) == NVSDK_NGX_Result_Success) destination.Set(key, value);
    }
    int reset{};
    if (source.Get(NVSDK_NGX_Parameter_Reset, &reset) == NVSDK_NGX_Result_Success)
        destination.Set(NVSDK_NGX_Parameter_Reset, reset);
}
}
