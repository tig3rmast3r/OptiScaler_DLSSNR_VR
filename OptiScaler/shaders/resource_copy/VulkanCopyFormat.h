#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
namespace vulkan_copy {
// Khronos SPIR-V ImageFormat operands, not a reinterpretation of VkFormat.
// https://github.com/KhronosGroup/SPIRV-Headers/blob/main/include/spirv/unified1/spirv.hpp
inline uint32_t image_format(VkFormat format) {
    switch(format) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 1;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 2;
    case VK_FORMAT_R32_SFLOAT: return 3;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: return 4;
    case VK_FORMAT_R8G8B8A8_SNORM: return 5;
    case VK_FORMAT_R32G32_SFLOAT: return 6;
    case VK_FORMAT_R16G16_SFLOAT: return 7;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 8;
    case VK_FORMAT_R16_SFLOAT: return 9;
    case VK_FORMAT_R16G16B16A16_UNORM: return 10;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 11;
    case VK_FORMAT_R16G16_UNORM: return 12;
    case VK_FORMAT_R8G8_UNORM: return 13;
    case VK_FORMAT_R16_UNORM: return 14;
    case VK_FORMAT_R8_UNORM: return 15;
    case VK_FORMAT_R16G16B16A16_SNORM: return 16;
    case VK_FORMAT_R16G16_SNORM: return 17;
    case VK_FORMAT_R8G8_SNORM: return 18;
    case VK_FORMAT_R16_SNORM: return 19;
    case VK_FORMAT_R8_SNORM: return 20;
    default: return 0; // Integer, sRGB and unknown storage views cannot use float4 stores.
    }
}
inline std::vector<char> typed_shader(std::span<const unsigned char> source, VkFormat format) {
    const auto imageFormat=image_format(format);
    if(!imageFormat || source.size()%4 || source.size()<20) return {};
    std::vector<uint32_t> words(source.size()/4);std::memcpy(words.data(),source.data(),source.size());
    if(words[0]!=0x07230203) return {};
    unsigned changed=0;bool extended=false;
    for(size_t i=5;i<words.size();) {
        const auto count=words[i]>>16,opcode=words[i]&65535;
        if(!count || i+count>words.size()) return {};
        if(opcode==17 && count==2 && words[i+1]==49) extended=true;
        // Only the storage image (Sampled=2), never the sampled source type.
        if(opcode==25 && count==9 && words[i+7]==2) {words[i+8]=imageFormat;++changed;}
        i+=count;
    }
    if(changed!=1) return {};
    if(imageFormat>5 && !extended) words.insert(words.begin()+5,{(2u<<16)|17u,49u});
    std::vector<char> result(words.size()*4);std::memcpy(result.data(),words.data(),result.size());return result;
}
}
