
#include "xrfg/luke_color_origin.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <limits>

namespace {
using Microsoft::WRL::ComPtr;
unsigned checks{};
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void check(HRESULT result, const char* message) { require(SUCCEEDED(result), message); }
void stamp(std::vector<float>& row, double start, double scale, std::uint32_t token) {
    const auto original = [&](int x) {
        return x >= 0 && x < static_cast<int>(xrfg::kLukeOriginWidth)
            ? (xrfg::luke_origin_white(token, static_cast<unsigned>(x)) ? 1. : 0.) : .5;
    };
    for (std::size_t x=0;x<row.size();++x) {
        const double source=(x+.5-start)/scale-.5;
        const auto a=static_cast<int>(std::floor(source));
        row[x]=static_cast<float>(original(a)+(original(a+1)-original(a))*(source-a));
    }
}
void codec_tests() {
    require(xrfg::kLukeOriginBits==64 && xrfg::kLukeOriginWidth==256 &&
        xrfg::kLukeOriginHeight==8, "compact copy dimensions");
    for (const auto token : {1u,17u,0x12345678u,0xfedcba98u,UINT32_MAX})
        for (const double scale : {.5,.75,.9,1.,1.1,1.25,1.5,2.})
            for (const double offset : {.0,.2,.5,.8}) {
                std::vector<float> row(1024,.5f);
                stamp(row,43+offset,scale,token);
                auto decoded=xrfg::decode_luke_origin_row(row);
                if (scale == .5 && offset == .5) {
                    require(!decoded.token, "erased Manchester cells invented identity");
                    continue;
                }
                if (decoded.token!=token || decoded.ambiguous) {
                    std::cerr << "token=" << token << " scale=" << scale << " offset=" << offset
                        << " decoded=" << decoded.token << " ambiguous=" << decoded.ambiguous << '\n';
                }
                require(decoded.token==token && !decoded.ambiguous,"resampled compact token");
                for (auto& pixel:row) pixel=static_cast<float>(.4+pixel*(3./255));
                decoded=xrfg::decode_luke_origin_row(row);
                require(decoded.token==token && !decoded.ambiguous,"dimmed compact token");
            }
    std::vector<float> row(1024,.5f);
    require(!xrfg::decode_luke_origin_row(row).token,"blank token invented");
    stamp(row,32,1,0x12345678u);
    for (unsigned x=32+27*4;x<32+28*4;++x) row[x]=1.f-row[x];
    require(!xrfg::decode_luke_origin_row(row).token,"CRC corruption accepted");
    row.assign(1024,.5f);
    for (unsigned x=0;x<256;++x) row[32+x]=xrfg::luke_origin_white(1,x)?1.f:0.f;
    for (unsigned x=0;x<256;++x) row[512+x]=xrfg::luke_origin_white(2,x)?1.f:0.f;
    require(xrfg::decode_luke_origin_row(row).ambiguous,"conflicting tokens accepted");
    stamp(row,32,1,0);
    require(!xrfg::decode_luke_origin_row(row).token,"zero token accepted");
    xrfg::LukeColorOriginLayout layout;
    require(xrfg::compact_luke_origin_layout(3344,3344,3063,3344,layout),"Horizon crop layout");
    require(layout.band_count==1 && layout.bands[0].x==283 && layout.bands[0].y==2 && layout.bands[0].width==256 &&
        layout.bands[0].height==8 && layout.scan.width==541 && layout.scan.height==12,"derived corner crop");
    for (unsigned crop=0;crop<=281;++crop)
        require(layout.bands[0].x>=crop && layout.bands[0].x+256<=crop+3063,"either-eye crop erases marker");
    require(xrfg::compact_luke_origin_layout(2048,1400,1700,1200,layout),"vertical crop layout");
    for (unsigned crop=0;crop<=200;++crop)
        require(layout.bands[0].y>=crop && layout.bands[0].y+8<=crop+1200,"vertical crop erases band");
    require(xrfg::compact_luke_origin_layout(2560,1440,1319,1440,layout),"KCD2 measured crop layout");
    require(layout.band_count==2&&layout.scan.width==1319&&layout.scan.height==12,"KCD2 bounded two-band ROI");
    for(unsigned i=0;i<layout.band_count;++i) {
        const auto& b=layout.bands[i];
        require(b.x+b.width<=2560&&b.y+b.height<=1440,"band outside producer");
        if(i)require(layout.bands[i-1].x+layout.bands[i-1].width<=b.x,"copies overwrite Manchester cells");
    }
    constexpr std::uint32_t coverage_token=0xa5c31236u;
    std::vector<float> producer(2560,.5F);
    for(unsigned i=0;i<layout.band_count;++i)for(unsigned x=0;x<xrfg::kLukeOriginWidth;++x)
        producer[layout.bands[i].x+x]=xrfg::luke_origin_white(coverage_token,x)?1.F:0.F;
    // Decode real cell patterns through EVERY possible KCD2 crop origin, not
    // merely the left/right endpoints or a matching implementation formula.
    for(unsigned crop=0;crop<=2560-1319;++crop) {
        const auto decoded=xrfg::decode_luke_origin_row(std::span<const float>(producer).subspan(crop,1319));
        require(decoded.token==coverage_token&&!decoded.ambiguous,"KCD2 crop lost/corrupted token");
    }
    // Both copies present in one eye must still reject conflicting identities.
    for(unsigned x=0;x<xrfg::kLukeOriginWidth;++x)
        producer[layout.bands[1].x+x]=xrfg::luke_origin_white(coverage_token+1,x)?1.F:0.F;
    require(xrfg::decode_luke_origin_row(std::span<const float>(producer).subspan(620,1319)).ambiguous,
        "distinct copied tokens selected silently");
    require(!xrfg::compact_luke_origin_layout(1024,1024,400,1024,layout),"uncoverable disjoint crop admitted");
    require(!xrfg::compact_luke_origin_layout(UINT32_MAX,1440,1319,1440,layout),"oversized/overflow layout admitted");
    require(!xrfg::compact_luke_origin_layout(512,8,512,8,layout),"insufficient band margin accepted");
    require(!xrfg::compact_luke_origin_layout(512,32,1024,32,layout),"upscaled/crop mismatch accepted");
}
void luma_tests() {
    const auto close=[](float actual,float expected,float tolerance=1.e-6F) {
        return std::isfinite(actual) && std::abs(actual-expected)<tolerance;
    };
    constexpr float red=.2126F,green=.7152F,blue=.0722F;
    for(auto format:{DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_TYPELESS,
        DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        const bool bgra=format==DXGI_FORMAT_B8G8R8A8_TYPELESS ||
            format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for(unsigned alpha:{0U,255U}) {
            const std::array<unsigned char,4> pixel{static_cast<unsigned char>(bgra?233:17),119,
                static_cast<unsigned char>(bgra?17:233),static_cast<unsigned char>(alpha)};
            require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(pixel.data()),format),
                (red*17+green*119+blue*233)/255),"encoded RGB/BGRA luma ordering or alpha");
        }
    }
    for(unsigned alpha:{0U,1U,2U,3U}) {
        const std::uint32_t packed=37U|(511U<<10)|(900U<<20)|(alpha<<30);
        require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(&packed),DXGI_FORMAT_R10G10B10A2_UNORM),
            (red*37+green*511+blue*900)/1023),"packed10 RGB luma ordering or alpha");
    }
    std::array<float,4> floating{-.5F,.25F,.75F,std::numeric_limits<float>::quiet_NaN()};
    require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(floating.data()),DXGI_FORMAT_R32G32B32A32_FLOAT),
        -.5F*red+.25F*green+.75F*blue),"float RGB signed luma or NaN alpha");
    for(unsigned channel=0;channel<3;++channel) for(float invalid:{std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) {
        auto broken=floating;broken[channel]=invalid;
        require(!std::isfinite(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(broken.data()),DXGI_FORMAT_R32G32B32A32_FLOAT)),
            "invalid float RGB component accepted");
    }
    std::array<std::uint16_t,4> half{0xb800,0x3400,0x3a00,0x7e00}; // -.5,.25,.75,NaN alpha
    require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(half.data()),DXGI_FORMAT_R16G16B16A16_FLOAT),
        -.5F*red+.25F*green+.75F*blue),"half RGB signed luma or NaN alpha");
    const std::array<std::uint16_t,4> subnormal{0x8001,0x0001,0x0000,0x7c00};
    require(close(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(subnormal.data()),DXGI_FORMAT_R16G16B16A16_FLOAT),
        (green-red)*std::ldexp(1.F,-24),1.e-12F),"half signed subnormals or infinite alpha");
    for(unsigned channel=0;channel<3;++channel) for(std::uint16_t invalid:{std::uint16_t(0x7e00),std::uint16_t(0x7c00),std::uint16_t(0xfc00)}) {
        auto broken=half;broken[channel]=invalid;
        require(std::isnan(xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(broken.data()),DXGI_FORMAT_R16G16B16A16_FLOAT)),
            "invalid half RGB component accepted");
    }
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT}) {
        std::array<std::byte,16> pixel{};
        for(bool white:{false,true}) {
            xrfg::luke_origin_pixel(pixel.data(),format,white);
            require(close(xrfg::luke_origin_luma(pixel.data(),format),white?1.F:0.F),"black/white stamp luminance changed");
        }
    }
    // Dim chromatic overlay: B quantizes to the SAME103 in both Manchester
    // cells, while R/G retain only three R8 steps. First-byte BGRA cannot decode.
    constexpr std::uint32_t token=0x12345678U;
    std::vector<std::array<unsigned char,4>> chromatic(1024,{103,103,103,255});
    for(unsigned x=0;x<xrfg::kLukeOriginWidth;++x) {
        const unsigned char level=xrfg::luke_origin_white(token,x)?105:102;
        chromatic[32+x]={103,level,level,static_cast<unsigned char>(x&255)};
    }
    std::vector<float> rgb_row(chromatic.size()),erased_blue(chromatic.size(),103.F/255);
    for(auto format:{DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_TYPELESS,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        for(std::size_t x=0;x<chromatic.size();++x)
            rgb_row[x]=xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(chromatic[x].data()),format);
        const auto decoded=xrfg::decode_luke_origin_row(rgb_row);
        require(decoded.token==token && !decoded.ambiguous,"dim RGB Manchester token lost despite R/G contrast");
    }
    require(!xrfg::decode_luke_origin_row(erased_blue).token,"erased blue pairs invented identity");
    for(unsigned x=32+27*4;x<32+28*4;++x) {
        chromatic[x][1]=static_cast<unsigned char>(207-chromatic[x][1]);
        chromatic[x][2]=static_cast<unsigned char>(207-chromatic[x][2]);
    }
    for(std::size_t x=0;x<chromatic.size();++x)
        rgb_row[x]=xrfg::luke_origin_luma(reinterpret_cast<const std::byte*>(chromatic[x].data()),DXGI_FORMAT_B8G8R8A8_UNORM);
    require(!xrfg::decode_luke_origin_row(rgb_row).token,"RGB luminance bypassed CRC corruption");
}
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Gpu() {
        ComPtr<IDXGIFactory6> factory; ComPtr<IDXGIAdapter> adapter;
        check(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())),"DXGI");
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.GetAddressOf())),"WARP");
        check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(device.GetAddressOf())),"device");
        const D3D12_COMMAND_QUEUE_DESC desc{};
        check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(queue.GetAddressOf())),"queue");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.GetAddressOf())),"allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(list.GetAddressOf())),"list");
    }
    ComPtr<ID3D12Resource> texture(unsigned width,unsigned height) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,
            nullptr,IID_PPV_ARGS(result.GetAddressOf())),"texture");
        return result;
    }
    void submit() {
        check(list->Close(),"close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    }
    void reset() {check(allocator->Reset(),"reset allocator");check(list->Reset(allocator.Get(),nullptr),"reset list");}
    void transition(ID3D12Resource* color,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={color,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
    }
    void crop(ID3D12Resource* source,ID3D12Resource* destination,unsigned x,unsigned y,unsigned width,unsigned height) {
        transition(source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(destination,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION s{},d{};s.pResource=source;s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.pResource=destination;d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{x,y,0,x+width,y+height,1};
        list->CopyTextureRegion(&d,0,0,0,&s,&box);
        transition(source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        transition(destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    }
};
void gpu_tests() {
    Gpu gpu;
    auto source=gpu.texture(1024,64),left=gpu.texture(800,64),right=gpu.texture(800,64);
    xrfg::configure_luke_color_origin_extent(0,0);
    require(xrfg::mark_luke_color_origin(gpu.list.Get(),source.Get(),1,D3D12_RESOURCE_STATE_COMMON,
        gpu.device.Get())==E_PENDING,"startup fabricated unknown viewport");
    // The first consumer supplies the true color extent; no legacy center/fullheight path.
    std::uint32_t token{};
    require(xrfg::read_luke_color_origin(gpu.queue.Get(),left.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000)
        ==HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && token==0,"bootstrap invented token");
    constexpr std::uint32_t first=0x12340000u;
    for (unsigned frame=1;frame<=75;++frame)
        check(xrfg::mark_luke_color_origin(gpu.list.Get(),source.Get(),first+frame,
            D3D12_RESOURCE_STATE_COMMON,gpu.device.Get()),"immutable mark after64");
    gpu.crop(source.Get(),left.Get(),0,0,800,64);
    gpu.crop(source.Get(),right.Get(),224,0,800,64);gpu.submit();
    for (auto* eye : {left.Get(),right.Get()}) {
        check(xrfg::read_luke_color_origin(gpu.queue.Get(),eye,&token,D3D12_RESOURCE_STATE_COMMON,5000),"cropped eye read");
        require(token==first+75,"cropped eye token drift");
    }
    gpu.reset();
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),source.Get(),UINT32_MAX,D3D12_RESOURCE_STATE_COMMON,
        gpu.device.Get()),"full32bit marker");
    gpu.crop(source.Get(),left.Get(),224,0,800,64);gpu.submit();
    check(xrfg::read_luke_color_origin(gpu.queue.Get(),left.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000),"max token read");
    require(token==UINT32_MAX,"32bit token truncated");
    ComPtr<ID3D12Fence> gate;
    check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf())),"gate fence");
    check(gpu.queue->Wait(gate.Get(),1),"gate reader");
    token=99;
    const auto stalled=xrfg::read_luke_color_origin(gpu.queue.Get(),left.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5);
    check(gate->Signal(1),"release reader gate");
    require(stalled==HRESULT_FROM_WIN32(WAIT_TIMEOUT) && !token,"stalled read fabricated token");
    check(xrfg::read_luke_color_origin(gpu.queue.Get(),left.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000),"read after timeout");
    require(token==UINT32_MAX,"readback/allocator prematurely reused");
    // Final Output differs from the intermediate and has an explicit active rectangle.
    gpu.reset();
    auto final=gpu.texture(1024,64),active=gpu.texture(800,32);
    xrfg::configure_luke_color_origin_extent(800,32);
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),final.Get(),0xfedcba98u,D3D12_RESOURCE_STATE_COMMON,
        gpu.device.Get(),{64,16,896,32}),"final active rectangle");
    gpu.crop(final.Get(),active.Get(),160,16,800,32);gpu.submit();
    check(xrfg::read_luke_color_origin(gpu.queue.Get(),active.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000),"final active read");
    require(token==0xfedcba98u,"marker attached to intermediate/incorrect active rectangle");
    // Actual KCD2 producer and eye dimensions; endpoint and interior crops.
    gpu.reset();auto kcd_source=gpu.texture(2560,1440);
    const std::array<unsigned,8> crops{0,1,257,258,620,1059,1060,1241};
    std::array<ComPtr<ID3D12Resource>,8> eyes;
    xrfg::configure_luke_color_origin_extent(1319,1440);
    constexpr std::uint32_t kcd_token=0x50520731u;
    check(xrfg::mark_luke_color_origin(gpu.list.Get(),kcd_source.Get(),kcd_token,
        D3D12_RESOURCE_STATE_COMMON,gpu.device.Get()),"KCD2 measured producer marker");
    for(unsigned i=0;i<eyes.size();++i) {
        eyes[i]=gpu.texture(1319,1440);
        gpu.crop(kcd_source.Get(),eyes[i].Get(),crops[i],0,1319,1440);
    }
    gpu.submit();
    for(auto& eye:eyes) {
        check(xrfg::read_luke_color_origin(gpu.queue.Get(),eye.Get(),&token,D3D12_RESOURCE_STATE_COMMON,5000),
            "KCD2 actual GPU endpoint/interior crop read");
        require(token==kcd_token,"KCD2 GPU crop changed publication identity");
    }

}
}
int main() {
    try { codec_tests();luma_tests();gpu_tests();std::cout<<"Compact origin: "<<checks<<" checks PASS\n";return 0; }
    catch(const std::exception& e) {std::cerr<<"Compact origin FAIL: "<<e.what()<<"\n";return 1;}
}
