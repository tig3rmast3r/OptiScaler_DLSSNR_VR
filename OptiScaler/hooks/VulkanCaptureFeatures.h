#pragma once
#include <vulkan/vulkan.h>
#include <mutex>
#include <unordered_map>

namespace vulkan_capture {
// Stack-owned copies live through the synchronous vkCreateDevice call. Never
// modify application-owned feature nodes or confuse supported with enabled.
struct FeatureRequest {
    VkPhysicalDeviceFeatures core{};
    VkPhysicalDeviceFeatures2 core2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    bool prepare(VkDeviceCreateInfo& info, bool enable, bool supported) {
        const VkPhysicalDeviceFeatures2* requested2 = nullptr;
        for (auto* node = static_cast<const VkBaseInStructure*>(info.pNext); node; node = node->pNext)
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
                requested2 = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node);
        if (requested2) {
            if (enable && supported && info.pNext == requested2) {
                core2 = *requested2;
                core2.features.shaderStorageImageExtendedFormats = VK_TRUE;
                info.pNext = &core2;
                return true;
            }
            return requested2->features.shaderStorageImageExtendedFormats == VK_TRUE;
        }
        if (info.pEnabledFeatures) core = *info.pEnabledFeatures;
        if (enable && supported) {
            core.shaderStorageImageExtendedFormats = VK_TRUE;
            info.pEnabledFeatures = &core;
        }
        return core.shaderStorageImageExtendedFormats == VK_TRUE;
    }
};
inline std::mutex featureMutex;
inline std::unordered_map<VkDevice, bool> deviceFeatures;
inline void record(VkDevice device, bool enabled) {
    std::lock_guard lock(featureMutex); deviceFeatures[device] = enabled;
}
inline bool enabled(VkDevice device) {
    std::lock_guard lock(featureMutex);
    auto found = deviceFeatures.find(device);
    return found != deviceFeatures.end() && found->second;
}
}
