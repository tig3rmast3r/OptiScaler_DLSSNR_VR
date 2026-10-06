#include "../OptiScaler/hooks/KcdCameraPacket.h"
#include "fixtures/kcd_camera_packets.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    std::array<unsigned char,kcd_camera::packet_bytes> first{};
    unsigned count{};
    for(unsigned index=0;index<kcd_fixture::count;++index) {
        const auto packet=kcd_fixture::packet(index);
        kcd_camera::Camera camera{};
        if(!kcd_camera::decode(packet.data(),packet.size(),1280,720,camera))
            throw std::runtime_error("real finite F11 camera packet rejected: "+std::to_string(count));
        require(camera.projection[11]==-1&&camera.near_plane>0&&camera.far_plane>camera.near_plane,"projection/depth/basis convention");
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j) {
            double value{};for(unsigned k=0;k<4;++k)value+=double(camera.projection[k*4+i])*camera.inverse_projection[j*4+k];
            require(std::abs(value-(i==j?1.:0.))<.001,"actual projection inverse");
        }
        require(kcd_camera::equivalent(camera,camera),"same packet ambiguity");
        auto different=camera;different.position[0]+=.03F;
        require(!kcd_camera::equivalent(camera,different),"different cameras silently selected");
        if(!count)first=packet;
        ++count;
    }
    require(count==90,"real fixture packet count");kcd_camera::Camera camera{};
    require(!kcd_camera::decode(first.data(),1007,1280,720,camera),"short packet admitted");
    require(!kcd_camera::decode(first.data(),first.size(),720,1280,camera),"different viewport admitted");
    auto corrupt=first;float bad=std::numeric_limits<float>::quiet_NaN();std::memcpy(corrupt.data()+944,&bad,4);
    require(!kcd_camera::decode(corrupt.data(),corrupt.size(),1280,720,camera),"NaN camera admitted");
    corrupt=first;bad=50;std::memcpy(corrupt.data()+944+12,&bad,4);
    require(!kcd_camera::decode(corrupt.data(),corrupt.size(),1280,720,camera),"incoherent view inverse admitted");
    corrupt=first;bad=0;std::memcpy(corrupt.data()+80,&bad,4);
    require(!kcd_camera::decode(corrupt.data(),corrupt.size(),1280,720,camera),"unrelated view projection admitted");
    corrupt={};require(!kcd_camera::decode(corrupt.data(),corrupt.size(),1280,720,camera),"empty packet admitted");
    std::cout<<"KCD camera decoder: 90 real F11 packets, projection/inverse/depth, corruption/NaN/short/viewport/ambiguity rejection PASS\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
