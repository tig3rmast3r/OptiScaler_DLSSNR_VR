#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace kcd_camera {
constexpr std::size_t packet_bytes = 1008;
struct Camera {
    std::array<float,16> projection{}, inverse_projection{};
    std::array<float,3> position{}, right{}, up{}, forward{};
    float near_plane{}, far_plane{}, fov{};
};
// KCD2's observed per-view packet. Offsets identify a layout, not an address.
// Require mutually verified view/inverse/VP and the full perspective contract.
inline bool decode(const void* bytes, std::size_t size, unsigned width,
    unsigned height, Camera& output) noexcept {
    output = {};
    if (!bytes || size < packet_bytes || !width || !height) return false;
    using Matrix = std::array<std::array<double,4>,4>;
    const auto load = [&](std::size_t offset, Matrix& m) {
        std::array<float,16> f{};
        std::memcpy(f.data(),static_cast<const unsigned char*>(bytes)+offset,sizeof(f));
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j) {
            if(!std::isfinite(f[i*4+j]))return false;
            m[i][j]=f[i*4+j];
        }
        return true;
    };
    const auto multiply = [](const Matrix& a,const Matrix& b) {
        Matrix r{};
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
            for(unsigned k=0;k<4;++k)r[i][j]+=a[i][k]*b[k][j];
        return r;
    };
    Matrix sky{},vp{},view{},inverse{};
    if(!load(0,sky)||!load(80,vp)||!load(880,view)||!load(944,inverse))return false;
    for(const auto* m:{&view,&inverse}) {
        for(unsigned j=0;j<4;++j)
            if(std::abs((*m)[3][j]-(j==3?1.:0.))>1e-5)return false;
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j) {
            double dot{};for(unsigned k=0;k<3;++k)dot+=(*m)[i][k]*(*m)[j][k];
            if(std::abs(dot-(i==j?1.:0.))>.001)return false;
        }
        const auto& a=*m;
        const double det=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-
            a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+
            a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
        if(std::abs(det-1.)>.001)return false;
    }
    for(const auto& product:{multiply(view,inverse),multiply(inverse,view)})
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
            if(std::abs(product[i][j]-(i==j?1.:0.))>.005)return false;
    // Translation-free sky VP and full VP must share exactly the same axes.
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<3;++j)
        if(std::abs(sky[i][j]-vp[i][j])>1e-5)return false;
    if(std::abs(sky[0][3])>1e-5||std::abs(sky[1][3])>1e-5||std::abs(sky[3][3])>1e-5)return false;
    auto projection=multiply(vp,inverse);
    const double sx=projection[0][0],sy=projection[1][1],a=projection[2][2],b=projection[2][3];
    if(sx<.05||sx>100||sy<.05||sy>100||a<0||a>.01||b<=0||b>100)return false;
    if(std::abs(sx/sy-double(height)/width)>.002)return false;
    if(std::abs(projection[3][2]+1.)>1e-5||std::abs(projection[3][3])>.002)return false;
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j) {
        const bool retained=(i==0&&(j==0||j==2))||(i==1&&(j==1||j==2))||
            (i==2&&(j==2||j==3))||(i==3&&j==2);
        if(!retained&&std::abs(projection[i][j])>.002)return false;
        if(!retained)projection[i][j]=0;
    }
    if(std::abs(projection[0][2])>.1||std::abs(projection[1][2])>.1)return false;
    projection[3][2]=-1.;
    const double near_plane=b/(1.+a),far_plane=a>1e-10?b/a:0.;
    if(near_plane<=0||near_plane>100||(far_plane!=0&&far_plane<=near_plane))return false;
    Matrix inv{};for(unsigned i=0;i<4;++i)inv[i][i]=1;
    auto work=projection;
    for(unsigned col=0;col<4;++col) {
        unsigned pivot=col;
        for(unsigned row=col+1;row<4;++row)if(std::abs(work[row][col])>std::abs(work[pivot][col]))pivot=row;
        if(std::abs(work[pivot][col])<1e-12)return false;
        std::swap(work[col],work[pivot]);std::swap(inv[col],inv[pivot]);
        const auto scale=work[col][col];
        for(unsigned j=0;j<4;++j){work[col][j]/=scale;inv[col][j]/=scale;}
        for(unsigned row=0;row<4;++row)if(row!=col) {
            const auto factor=work[row][col];
            for(unsigned j=0;j<4;++j){work[row][j]-=factor*work[col][j];inv[row][j]-=factor*inv[col][j];}
        }
    }
    Camera camera{};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j) {
        camera.projection[j*4+i]=static_cast<float>(projection[i][j]);
        camera.inverse_projection[j*4+i]=static_cast<float>(inv[i][j]);
        if(!std::isfinite(camera.projection[j*4+i])||!std::isfinite(camera.inverse_projection[j*4+i]))return false;
    }
    for(unsigned i=0;i<3;++i) {
        camera.position[i]=static_cast<float>(inverse[i][3]);
        camera.right[i]=static_cast<float>(inverse[i][0]);
        camera.up[i]=static_cast<float>(inverse[i][1]);
        camera.forward[i]=static_cast<float>(-inverse[i][2]);
    }
    camera.near_plane=static_cast<float>(near_plane);camera.far_plane=static_cast<float>(far_plane);
    camera.fov=static_cast<float>(2.*std::atan(1./sy));
    output=camera;return true;
}
inline bool equivalent(const Camera& a,const Camera& b) noexcept {
    for(unsigned i=0;i<3;++i)
        if(std::abs(a.position[i]-b.position[i])>.002F||std::abs(a.right[i]-b.right[i])>1e-4F||
            std::abs(a.up[i]-b.up[i])>1e-4F||std::abs(a.forward[i]-b.forward[i])>1e-4F)return false;
    for(unsigned i=0;i<16;++i)if(std::abs(a.projection[i]-b.projection[i])>1e-4F)return false;
    return true;
}
} // namespace kcd_camera
