#pragma once
#include "LukeSourceContext.h"
#include <sl.h>
#include <sl_consts.h>
#include <nvsdk_ngx_vk.h>
#include "../../third_party/ofxr/include/xrfg/bridge_flight_logger.hpp"

namespace luke_source {
inline void emit(std::uint64_t id, unsigned stage, std::uint64_t c, std::uint64_t d) noexcept {
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
        static_cast<std::int64_t>(id),stage,c,d);
}
inline bool enabled() noexcept { return xrfg::bridge_flight_logger().ram_enabled(); }
inline void constants(const sl::Constants& v, unsigned frame, unsigned viewport, bool after) noexcept {
    if(!enabled()) return;
    const auto id=key(frame,viewport); const unsigned stage=after?208:200;
    auto camera=hash(reinterpret_cast<const float*>(&v.cameraViewToClip),16);
    camera=hash(reinterpret_cast<const float*>(&v.clipToCameraView),16,camera);
    camera=hash(reinterpret_cast<const float*>(&v.cameraPos),3,camera);
    camera=hash(reinterpret_cast<const float*>(&v.cameraRight),3,camera);
    camera=hash(reinterpret_cast<const float*>(&v.cameraUp),3,camera);
    camera=hash(reinterpret_cast<const float*>(&v.cameraFwd),3,camera);
    auto history=hash(reinterpret_cast<const float*>(&v.clipToPrevClip),16);
    history=hash(reinterpret_cast<const float*>(&v.prevClipToClip),16,history);
    emit(id,stage,camera,history);
    emit(id,stage+1,pair(v.mvecScale.x,v.mvecScale.y),pair(v.jitterOffset.x,v.jitterOffset.y));
    emit(id,stage+2,pair(v.cameraPinholeOffset.x,v.cameraPinholeOffset.y),
        static_cast<unsigned>(v.cameraMotionIncluded) |
        (std::uint64_t(static_cast<unsigned>(v.motionVectors3D))<<8) |
        (std::uint64_t(static_cast<unsigned>(v.motionVectorsJittered))<<16));
    emit(id,stage+3,pair(v.cameraNear,v.cameraFar),pair(v.cameraFOV,v.cameraAspectRatio));
}
inline void tag(unsigned frame, unsigned viewport, const sl::ResourceTag& t, unsigned point) noexcept {
    if(!enabled()) return;
    unsigned slot;
    if(t.type==sl::kBufferTypeMotionVectors) slot=0;
    else if(t.type==sl::kBufferTypeDepth || t.type==sl::kBufferTypeHiResDepth) slot=1;
    else if(t.type==sl::kBufferTypeScalingInputColor) slot=2;
    else if(t.type==sl::kBufferTypeScalingOutputColor) slot=3;
    else return;
    const auto id=key(frame,viewport); const unsigned stage=320+point*48+slot*8;
    emit(id,stage,t.resource?reinterpret_cast<std::uintptr_t>(t.resource->native):0,
        t.resource?reinterpret_cast<std::uintptr_t>(t.resource->view):0);
    if(!t.resource) return;
    emit(id,stage+1,(std::uint64_t(t.resource->width)<<32)|t.resource->height,
        (std::uint64_t(t.resource->nativeFormat)<<32)|t.resource->state);
    emit(id,stage+2,(std::uint64_t(t.extent.left)<<32)|t.extent.top,
        (std::uint64_t(t.extent.width)<<32)|t.extent.height);
    emit(id,stage+3,(std::uint64_t(t.resource->arrayLayers)<<32)|t.resource->mipLevels,
        (std::uint64_t(static_cast<unsigned>(t.type))<<32)|static_cast<unsigned>(t.lifecycle));
    const sl::BaseStructure* extension=t.next;
    bool precisionRecorded=false;
    for(unsigned n=0;extension && n<4;++n,extension=extension->next) {
        if(extension->structType==sl::PrecisionInfo::s_structType) {
            const auto* p=static_cast<const sl::PrecisionInfo*>(extension);
            emit(id,stage+4,static_cast<unsigned>(p->conversionFormula),pair(p->scale,p->bias));
            precisionRecorded=true;
        }
    }
    if(!precisionRecorded) emit(id,stage+4,unknown,0);
}
inline void native(const NVSDK_NGX_Parameter& p, unsigned point, std::uint64_t stream,
    std::uintptr_t command) noexcept {
    if(!enabled()) return;
    const auto id=key(active.frame,active.viewport); const unsigned stage=480+point*40;
    emit(id,stage,stream,command);
    unsigned slot=0;
    for(auto name:{NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Depth,
        NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_Output}) {
        void* borrowed=nullptr; const auto status=p.Get(name,&borrowed);
        if(status==NVSDK_NGX_Result_Success && borrowed) {
            const auto* r=static_cast<const NVSDK_NGX_Resource_VK*>(borrowed);
            if(r->Type==NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) {
                const auto& v=r->Resource.ImageViewInfo;
                emit(id,stage+1+slot*3,reinterpret_cast<std::uintptr_t>(v.Image),reinterpret_cast<std::uintptr_t>(v.ImageView));
                emit(id,stage+2+slot*3,(std::uint64_t(v.Width)<<32)|v.Height,
                    (std::uint64_t(static_cast<unsigned>(v.Format))<<32)|v.SubresourceRange.baseArrayLayer);
                emit(id,stage+3+slot*3,(std::uint64_t(v.SubresourceRange.baseMipLevel)<<32)|v.SubresourceRange.levelCount,
                    (std::uint64_t(v.SubresourceRange.aspectMask)<<32)|v.SubresourceRange.layerCount);
            } else emit(id,stage+1+slot*3,static_cast<unsigned>(status),~std::uint64_t{});
        } else emit(id,stage+1+slot*3,static_cast<unsigned>(status),0);
        ++slot;
    }
    float sx{},sy{},jx{},jy{};
    unsigned flags{},w{},h{}; int reset{};
    unsigned mask=0;
    mask|=(p.Get(NVSDK_NGX_Parameter_MV_Scale_X,&sx)==NVSDK_NGX_Result_Success)?1:0;
    mask|=(p.Get(NVSDK_NGX_Parameter_MV_Scale_Y,&sy)==NVSDK_NGX_Result_Success)?2:0;
    mask|=(p.Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&jx)==NVSDK_NGX_Result_Success)?4:0;
    mask|=(p.Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&jy)==NVSDK_NGX_Result_Success)?8:0;
    p.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&flags);
    p.Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,&w);
    p.Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,&h);
    p.Get(NVSDK_NGX_Parameter_Reset,&reset);
    emit(id,stage+14,pair(sx,sy),pair(jx,jy));
    emit(id,stage+15,(std::uint64_t(w)<<32)|h,(std::uint64_t(mask)<<32)|flags);
    emit(id,stage+16,static_cast<unsigned>(reset),active.feature);
    float ox{},oy{},delta{};
    unsigned extraMask=0;
    extraMask|=(p.Get(NVSDK_NGX_Parameter_MV_Offset_X,&ox)==NVSDK_NGX_Result_Success)?1:0;
    extraMask|=(p.Get(NVSDK_NGX_Parameter_MV_Offset_Y,&oy)==NVSDK_NGX_Result_Success)?2:0;
    const auto deltaStatus=p.Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,&delta);
    emit(id,stage+17,pair(ox,oy),extraMask);
    emit(id,stage+18,pair(delta,0.F),static_cast<unsigned>(deltaStatus));
    const char* pairs[][2]={
        {NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y},
        {NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y},
        {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y},
        {NVSDK_NGX_Parameter_OutWidth,NVSDK_NGX_Parameter_OutHeight},
        {NVSDK_NGX_Parameter_Width,NVSDK_NGX_Parameter_Height}};
    for(unsigned i=0;i<5;++i) {
        unsigned x{},y{},present=0;
        present|=(p.Get(pairs[i][0],&x)==NVSDK_NGX_Result_Success)?1:0;
        present|=(p.Get(pairs[i][1],&y)==NVSDK_NGX_Result_Success)?2:0;
        emit(id,stage+19+i,(std::uint64_t(x)<<32)|y,present);
    }
}
}
