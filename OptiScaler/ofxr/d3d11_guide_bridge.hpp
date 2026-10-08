#pragma once
#include <memory>
#include <d3d11.h>
#include <nvsdk_ngx.h>
class IFeature;
namespace ofxr_bridge {
class D3D11GuideBridge final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    D3D11GuideBridge();
    ~D3D11GuideBridge();
    bool begin(ID3D11DeviceContext*, NVSDK_NGX_Parameter*, IFeature*);
    void complete(ID3D11DeviceContext*, NVSDK_NGX_Parameter*, IFeature*, bool success);
};
// Provenance observation is scoped to the requested native Assetto ingress.
void observe_d3d11_guide_context(ID3D11DeviceContext*);
}
