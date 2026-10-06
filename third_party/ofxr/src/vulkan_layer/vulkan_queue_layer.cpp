// OFXR Bridge's Vulkan implicit layer: one mutex per device around every
// queue operation that Vulkan requires the application to synchronise
// itself.
//
// Why it exists. In a Vulkan session the OpenXR runtime submits on the queue
// the application handed it, inside xrEndFrame and the swapchain calls. The
// bridge's presenter thread makes those calls, so the runtime submits from
// the presenter thread while the game's own threads submit to the same
// queue - and Vulkan forbids that. Measured: No Man's Sky through
// OpenComposite lost the device under its own vkQueueSubmit on SteamVR and
// got XR_ERROR_RUNTIME_FAILURE from xrEndFrame on VDXR, both a few seconds
// into generation, both while the game was submitting heavily. The game's,
// the runtime's and the bridge's submissions all reach the driver through
// the loader, so a layer in the chain serialises all three.
//
// What it does not do. It intercepts nothing else, holds the lock only
// across the driver's own call, and behaves the same in every process it is
// loaded into, since an implicit layer loads into every Vulkan process while
// it is registered. The tray registers it only while the bridge is armed and
// the Vulkan option is on, and OFXR_DISABLE_VULKAN_QUEUE_LAYER=1 turns it
// off for a process.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <windows.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace {

// The loader stores its dispatch pointer at the start of every dispatchable
// object; objects made from a device carry the device's, which is how a
// queue finds its device's table.
[[nodiscard]] void* dispatch_key(const void* handle) noexcept {
    return *static_cast<void* const*>(handle);
}

struct InstanceTable {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    PFN_vkDestroyInstance destroy_instance{};
};

struct DeviceTable {
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    PFN_vkDestroyDevice destroy_device{};
    PFN_vkQueueSubmit queue_submit{};
    PFN_vkQueueSubmit2 queue_submit2{};
    PFN_vkQueueSubmit2 queue_submit2_khr{};
    PFN_vkQueueWaitIdle queue_wait_idle{};
    PFN_vkQueueBindSparse queue_bind_sparse{};
    PFN_vkQueuePresentKHR queue_present{};
    // One lock for every queue of the device: the race is on a queue, and a
    // device rarely has more than one that matters, so the simpler scope
    // costs nothing measurable.
    std::mutex queue_mutex;
};

std::mutex g_tables_mutex;
std::unordered_map<void*, InstanceTable> g_instances;
std::unordered_map<void*, std::shared_ptr<DeviceTable>> g_devices;

[[nodiscard]] InstanceTable instance_table(void* key) {
    std::scoped_lock lock(g_tables_mutex);
    const auto found = g_instances.find(key);
    return found == g_instances.end() ? InstanceTable{} : found->second;
}

[[nodiscard]] std::shared_ptr<DeviceTable> device_table(void* key) {
    std::scoped_lock lock(g_tables_mutex);
    const auto found = g_devices.find(key);
    return found == g_devices.end() ? nullptr : found->second;
}

template <typename Function>
[[nodiscard]] Function load(
    PFN_vkGetDeviceProcAddr get, VkDevice device, const char* name) noexcept {
    return reinterpret_cast<Function>(get(device, name));
}

VKAPI_ATTR VkResult VKAPI_CALL layer_create_instance(
    const VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkInstance* instance) {
    // Find this layer's link in the loader's chain and step past it.
    auto* chain = const_cast<VkLayerInstanceCreateInfo*>(
        static_cast<const VkLayerInstanceCreateInfo*>(create_info->pNext));
    while (chain != nullptr &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
             chain->function == VK_LAYER_LINK_INFO)) {
        chain = const_cast<VkLayerInstanceCreateInfo*>(
            static_cast<const VkLayerInstanceCreateInfo*>(chain->pNext));
    }
    if (chain == nullptr || chain->u.pLayerInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const PFN_vkGetInstanceProcAddr next_get =
        chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    const auto next_create = reinterpret_cast<PFN_vkCreateInstance>(
        next_get(VK_NULL_HANDLE, "vkCreateInstance"));
    if (next_create == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = next_create(create_info, allocator, instance);
    if (result != VK_SUCCESS) {
        return result;
    }
    InstanceTable table;
    table.get_instance_proc_addr = next_get;
    table.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
        next_get(*instance, "vkDestroyInstance"));
    std::scoped_lock lock(g_tables_mutex);
    g_instances[dispatch_key(*instance)] = table;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL layer_destroy_instance(
    VkInstance instance, const VkAllocationCallbacks* allocator) {
    if (instance == VK_NULL_HANDLE) {
        return;
    }
    InstanceTable table;
    {
        std::scoped_lock lock(g_tables_mutex);
        const auto found = g_instances.find(dispatch_key(instance));
        if (found != g_instances.end()) {
            table = found->second;
            g_instances.erase(found);
        }
    }
    if (table.destroy_instance != nullptr) {
        table.destroy_instance(instance, allocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL layer_create_device(
    VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkDevice* device) {
    auto* chain = const_cast<VkLayerDeviceCreateInfo*>(
        static_cast<const VkLayerDeviceCreateInfo*>(create_info->pNext));
    while (chain != nullptr &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
             chain->function == VK_LAYER_LINK_INFO)) {
        chain = const_cast<VkLayerDeviceCreateInfo*>(
            static_cast<const VkLayerDeviceCreateInfo*>(chain->pNext));
    }
    if (chain == nullptr || chain->u.pLayerInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const PFN_vkGetInstanceProcAddr next_instance_get =
        chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr next_device_get =
        chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    const auto next_create = reinterpret_cast<PFN_vkCreateDevice>(
        next_instance_get(VK_NULL_HANDLE, "vkCreateDevice"));
    if (next_create == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result =
        next_create(physical_device, create_info, allocator, device);
    if (result != VK_SUCCESS) {
        return result;
    }
    auto table = std::make_shared<DeviceTable>();
    table->get_device_proc_addr = next_device_get;
    table->destroy_device =
        load<PFN_vkDestroyDevice>(next_device_get, *device, "vkDestroyDevice");
    table->queue_submit =
        load<PFN_vkQueueSubmit>(next_device_get, *device, "vkQueueSubmit");
    table->queue_submit2 =
        load<PFN_vkQueueSubmit2>(next_device_get, *device, "vkQueueSubmit2");
    table->queue_submit2_khr =
        load<PFN_vkQueueSubmit2>(next_device_get, *device, "vkQueueSubmit2KHR");
    table->queue_wait_idle =
        load<PFN_vkQueueWaitIdle>(next_device_get, *device, "vkQueueWaitIdle");
    table->queue_bind_sparse =
        load<PFN_vkQueueBindSparse>(next_device_get, *device, "vkQueueBindSparse");
    table->queue_present =
        load<PFN_vkQueuePresentKHR>(next_device_get, *device, "vkQueuePresentKHR");
    std::scoped_lock lock(g_tables_mutex);
    g_devices[dispatch_key(*device)] = std::move(table);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL layer_destroy_device(
    VkDevice device, const VkAllocationCallbacks* allocator) {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    std::shared_ptr<DeviceTable> table;
    {
        std::scoped_lock lock(g_tables_mutex);
        const auto found = g_devices.find(dispatch_key(device));
        if (found != g_devices.end()) {
            table = found->second;
            g_devices.erase(found);
        }
    }
    if (table && table->destroy_device != nullptr) {
        table->destroy_device(device, allocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL layer_queue_submit(
    VkQueue queue, uint32_t submit_count, const VkSubmitInfo* submits, VkFence fence) {
    const auto table = device_table(dispatch_key(queue));
    if (!table || table->queue_submit == nullptr) {
        return VK_ERROR_DEVICE_LOST;
    }
    std::scoped_lock lock(table->queue_mutex);
    return table->queue_submit(queue, submit_count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL layer_queue_submit2(
    VkQueue queue, uint32_t submit_count, const VkSubmitInfo2* submits, VkFence fence) {
    const auto table = device_table(dispatch_key(queue));
    if (!table || table->queue_submit2 == nullptr) {
        return VK_ERROR_DEVICE_LOST;
    }
    std::scoped_lock lock(table->queue_mutex);
    return table->queue_submit2(queue, submit_count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL layer_queue_submit2_khr(
    VkQueue queue, uint32_t submit_count, const VkSubmitInfo2* submits, VkFence fence) {
    const auto table = device_table(dispatch_key(queue));
    if (!table || table->queue_submit2_khr == nullptr) {
        return VK_ERROR_DEVICE_LOST;
    }
    std::scoped_lock lock(table->queue_mutex);
    return table->queue_submit2_khr(queue, submit_count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL layer_queue_wait_idle(VkQueue queue) {
    const auto table = device_table(dispatch_key(queue));
    if (!table || table->queue_wait_idle == nullptr) {
        return VK_ERROR_DEVICE_LOST;
    }
    std::scoped_lock lock(table->queue_mutex);
    return table->queue_wait_idle(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL layer_queue_bind_sparse(
    VkQueue queue, uint32_t bind_count, const VkBindSparseInfo* binds, VkFence fence) {
    const auto table = device_table(dispatch_key(queue));
    if (!table || table->queue_bind_sparse == nullptr) {
        return VK_ERROR_DEVICE_LOST;
    }
    std::scoped_lock lock(table->queue_mutex);
    return table->queue_bind_sparse(queue, bind_count, binds, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL layer_queue_present(
    VkQueue queue, const VkPresentInfoKHR* present_info) {
    const auto table = device_table(dispatch_key(queue));
    if (!table || table->queue_present == nullptr) {
        return VK_ERROR_DEVICE_LOST;
    }
    std::scoped_lock lock(table->queue_mutex);
    return table->queue_present(queue, present_info);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_get_device_proc_addr(
    VkDevice device, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_get_instance_proc_addr(
    VkInstance instance, const char* name);

// The functions this layer answers itself. A device-level query and an
// instance-level one both have to find them: the loader asks either way.
[[nodiscard]] PFN_vkVoidFunction intercepted(std::string_view name) noexcept {
    struct Entry {
        std::string_view name;
        PFN_vkVoidFunction function;
    };
    static const Entry entries[] = {
        {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(layer_get_instance_proc_addr)},
        {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(layer_get_device_proc_addr)},
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(layer_create_instance)},
        {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(layer_destroy_instance)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(layer_create_device)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(layer_destroy_device)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(layer_queue_submit)},
        {"vkQueueSubmit2", reinterpret_cast<PFN_vkVoidFunction>(layer_queue_submit2)},
        {"vkQueueSubmit2KHR", reinterpret_cast<PFN_vkVoidFunction>(layer_queue_submit2_khr)},
        {"vkQueueWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(layer_queue_wait_idle)},
        {"vkQueueBindSparse", reinterpret_cast<PFN_vkVoidFunction>(layer_queue_bind_sparse)},
        {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(layer_queue_present)},
    };
    for (const Entry& entry : entries) {
        if (entry.name == name) {
            return entry.function;
        }
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_get_device_proc_addr(
    VkDevice device, const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    if (const auto function = intercepted(name); function != nullptr) {
        // A device that the layer did not see created has nothing to chain
        // to for these; leave them to the next layer.
        const auto table = device != VK_NULL_HANDLE ? device_table(dispatch_key(device)) : nullptr;
        if (table || std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
            return function;
        }
    }
    if (device == VK_NULL_HANDLE) {
        return nullptr;
    }
    const auto table = device_table(dispatch_key(device));
    return table && table->get_device_proc_addr
        ? table->get_device_proc_addr(device, name)
        : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_get_instance_proc_addr(
    VkInstance instance, const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    if (const auto function = intercepted(name); function != nullptr) {
        return function;
    }
    if (instance == VK_NULL_HANDLE) {
        return nullptr;
    }
    const InstanceTable table = instance_table(dispatch_key(instance));
    return table.get_instance_proc_addr != nullptr
        ? table.get_instance_proc_addr(instance, name)
        : nullptr;
}

}  // namespace

extern "C" __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
OFXR_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (version == nullptr ||
        version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        version->loaderLayerInterfaceVersion < 1) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (version->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION) {
        version->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    }
    if (version->loaderLayerInterfaceVersion >= 2) {
        version->pfnGetInstanceProcAddr = layer_get_instance_proc_addr;
        version->pfnGetDeviceProcAddr = layer_get_device_proc_addr;
        version->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    return VK_SUCCESS;
}

// The loader's fallback for a manifest without a negotiation function; kept
// so a loader older than interface version 2 still finds the layer.
extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
OFXR_vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    return layer_get_instance_proc_addr(instance, name);
}

extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
OFXR_vkGetDeviceProcAddr(VkDevice device, const char* name) {
    return layer_get_device_proc_addr(device, name);
}

// A module handle alone does not prove that this layer is in this device's
// loader chain. The embedded bridge checks the actual device table before
// making queue submissions from its XR thread.
extern "C" __declspec(dllexport) VKAPI_ATTR std::uint32_t VKAPI_CALL
OFXR_vulkanQueueDeviceSerialized(VkDevice device) {
    if (device == VK_NULL_HANDLE) return 0;
    const auto table = device_table(dispatch_key(device));
    return table && table->queue_submit && table->queue_wait_idle ? 1U : 0U;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Nothing happens per thread.
        return TRUE;
    }
    return TRUE;
}
