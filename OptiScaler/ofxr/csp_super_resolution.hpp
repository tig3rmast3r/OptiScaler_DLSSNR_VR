#pragma once
#include <d3d11_4.h>
#include <nvsdk_ngx.h>
#include <functional>
#include <memory>
namespace ofxr_bridge {
// The application keeps its small postprocess carrier. Only the exact final
// XR color draw consumes the owned, per-native-handle DLSS result.
class CspD3D11SuperResolution final {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    using Render=std::function<bool(ID3D11DeviceContext*,NVSDK_NGX_Parameter*,unsigned,unsigned)>;
    struct ParametersApi {
        std::function<NVSDK_NGX_Result(NVSDK_NGX_Parameter**)> allocate;
        std::function<void(NVSDK_NGX_Parameter*)> destroy;
    };
    CspD3D11SuperResolution(ID3D11DeviceContext*,NVSDK_NGX_Parameter*,ParametersApi,Render);
    ~CspD3D11SuperResolution();
    bool ready() const;
    bool prepare(ID3D11DeviceContext*,NVSDK_NGX_Parameter*);
    // Close callbacks before the native feature destructor. False retains the
    // unretired GPU ownership group, including the feature's SDK handle.
    bool retire();
};
}
