#include "xrfg/d3d11_color_identity.hpp"
#include <wrl/client.h>
#include <bit>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
std::uint32_t mix(std::uint32_t x) { x^=x>>16; x*=0x7feb352du;
    x^=x>>15; x*=0x846ca68bu; return x^(x>>16); }

int main() {
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,
            &device,nullptr,&context)),"device");
        xrfg::D3D11ColorIdentityProbe probe(device.Get(),context.Get());
        const auto fingerprint=[&](ID3D11Texture2D* image,std::uint64_t serial) {
            const xrfg::ColorIdentityTag tag{serial,serial+100,9876,6543,2,1,0};
            check(SUCCEEDED(probe.queue(image,tag)),"queue fingerprint");
            // TEST ONLY: production never flushes/waits for readback.
            context->Flush();
            std::vector<xrfg::ColorIdentityResult> results;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(results.empty() && std::chrono::steady_clock::now()<deadline) {
                probe.poll(&results); std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(results.size()==1,"async readback missing/duplicate");
            check(results[0].tag.application_serial==serial && results[0].tag.capture_serial==serial+100 &&
                results[0].tag.pose_hash==6543 && results[0].tag.eye==1,"immutable identity tag");
            return results[0];
        };
        D3D11_TEXTURE2D_DESC d{};
        d.Width=321; d.Height=197; d.ArraySize=2; d.MipLevels=2;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> image;
        check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&image)),"float image");
        std::vector<std::array<float,4>> pixels(d.Width*d.Height);
        std::array<std::uint32_t,4> expected{};
        const std::uint32_t salts[]{0x13579bdfu,0x2468ace0u,0x98765432u,0xfedcba98u};
        for(unsigned slice=0;slice<2;++slice) {
            for(unsigned i=0;i<pixels.size();++i) {
                pixels[i]={float(i%71),float(slice),float(i%29)*0.25f,1.0f};
                for(unsigned c=0;c<4;++c) expected[c]+=mix(std::bit_cast<std::uint32_t>(pixels[i][c])^
                    mix((i+slice*static_cast<unsigned>(pixels.size()))^salts[c]));
            }
            context->UpdateSubresource(image.Get(),slice*d.MipLevels,nullptr,pixels.data(),d.Width*16,0);
        }
        // State survives the complete diagnostic compute pass.
        ComPtr<ID3D11ShaderResourceView> app_srv;
        check(SUCCEEDED(device->CreateShaderResourceView(image.Get(),nullptr,&app_srv)),"app srv");
        auto* bound=app_srv.Get(); context->CSSetShaderResources(0,1,&bound);
        D3D11_VIEWPORT viewport{3,7,99,101,0.2f,0.8f}; context->RSSetViewports(1,&viewport);
        const auto initial=fingerprint(image.Get(),1);
        check(initial.hash==expected,"full-image CPU/GPU reference: mip0, edge groups, both slices");
        ComPtr<ID3D11ShaderResourceView> restored;
        context->CSGetShaderResources(0,1,&restored);
        check(restored.Get()==app_srv.Get(),"application CS binding changed");
        D3D11_VIEWPORT actual{}; UINT count=1; context->RSGetViewports(&count,&actual);
        check(actual.TopLeftX==viewport.TopLeftX && actual.TopLeftY==viewport.TopLeftY &&
            actual.MaxDepth==viewport.MaxDepth,"viewport changed");
        const auto repeated=fingerprint(image.Get(),2);
        check(initial.hash==repeated.hash,"probe changed source color");
        // The last pixel of the second eye is outside an aligned 16x16 group.
        // A sampled-corner/one-eye probe would fail this reference/change test.
        pixels.back()[0]+=1;
        context->UpdateSubresource(image.Get(),d.MipLevels,nullptr,pixels.data(),d.Width*16,0);
        const auto changed=fingerprint(image.Get(),3);
        check(changed.hash!=initial.hash,"single far-corner pixel ignored");
        for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}) {
            d.ArraySize=1; d.MipLevels=1; d.Format=format;
            ComPtr<ID3D11Texture2D> a,b;
            check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&a)) &&
                SUCCEEDED(device->CreateTexture2D(&d,nullptr,&b)),"packed image");
            std::vector<std::uint32_t> color(d.Width*d.Height,0xff427183);
            context->UpdateSubresource(a.Get(),0,nullptr,color.data(),d.Width*4,0);
            context->CopyResource(b.Get(),a.Get());
            const auto ha=fingerprint(a.Get(),4);
            const auto hb=fingerprint(b.Get(),5);
            check(ha.hash==hb.hash,"copied actual color mismatches");
            color[d.Width*83+92]^=1;
            context->UpdateSubresource(b.Get(),0,nullptr,color.data(),d.Width*4,0);
            check(fingerprint(b.Get(),6).hash!=ha.hash,"single packed pixel ignored");
        }
        std::cout<<"Full mip0 GPU identity: CPU reference, both eyes, 1-pixel fault, copies, RGBA/BGRA, immutable tags, preserved state\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
