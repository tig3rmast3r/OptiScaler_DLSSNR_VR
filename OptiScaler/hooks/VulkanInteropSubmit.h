#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace vulkan_interop {
struct Handoff {
    VkCommandBuffer original{}, copyback{}, continuation{};
    VkSemaphore texture{}, copied{}, completed{};
    uint64_t capturedValue{}, d3d12Value{}, copiedValue{}, completedValue{};
};

// Own every modified array/pNext node until the real submit returns. Never
// mutate application-owned (often const) timeline metadata or signal values.
struct Submit1 {
    std::vector<VkSubmitInfo> submits;
    std::vector<VkCommandBuffer> prefix, suffix;
    std::vector<VkSemaphore> finalSignals;
    std::vector<uint64_t> waits, signals;
    std::vector<std::unique_ptr<unsigned char[]>> nodes;
    std::vector<uint32_t> prefixMasks, suffixMasks, finalSignalDevices;
    VkTimelineSemaphoreSubmitInfo firstTimeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    VkTimelineSemaphoreSubmitInfo copyTimeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    VkTimelineSemaphoreSubmitInfo finalTimeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    VkDeviceGroupSubmitInfo finalGroup{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO};
    VkPipelineStageFlags stage{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    Handoff handoff;

    template<class T> VkBaseOutStructure* clone(const VkBaseInStructure* node) {
        auto bytes = std::make_unique<unsigned char[]>(sizeof(T));
        std::memcpy(bytes.get(), node, sizeof(T));
        auto* result = reinterpret_cast<VkBaseOutStructure*>(bytes.get());
        result->pNext = nullptr;
        nodes.push_back(std::move(bytes));
        return result;
    }
    bool build(uint32_t count, const VkSubmitInfo* original, Handoff h) {
        handoff = h;
        submits.reserve(count + 2);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& source = original[i];
            uint32_t split = 0;
            while (split < source.commandBufferCount && source.pCommandBuffers[split] != h.original) ++split;
            if (split == source.commandBufferCount) { submits.push_back(source); continue; }
            prefix.assign(source.pCommandBuffers, source.pCommandBuffers + split + 1);
            suffix.push_back(h.continuation);
            suffix.insert(suffix.end(), source.pCommandBuffers + split + 1,
                          source.pCommandBuffers + source.commandBufferCount);
            if (source.signalSemaphoreCount)
                finalSignals.assign(source.pSignalSemaphores, source.pSignalSemaphores + source.signalSemaphoreCount);
            waits.assign(source.waitSemaphoreCount, 0);
            signals.assign(source.signalSemaphoreCount, 0);
            VkBaseOutStructure* head = nullptr;
            VkBaseOutStructure* tail = nullptr;
            for (auto* node = static_cast<const VkBaseInStructure*>(source.pNext); node; node = node->pNext) {
                if (node->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                    const auto* timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(node);
                    if (timeline->waitSemaphoreValueCount != source.waitSemaphoreCount ||
                        timeline->signalSemaphoreValueCount != source.signalSemaphoreCount) return false;
                    if (!waits.empty()) std::copy_n(timeline->pWaitSemaphoreValues, waits.size(), waits.data());
                    if (!signals.empty()) std::copy_n(timeline->pSignalSemaphoreValues, signals.size(), signals.data());
                    continue;
                }
                VkBaseOutStructure* copied = nullptr;
                switch (node->sType) {
                case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO: {
                    copied = clone<VkDeviceGroupSubmitInfo>(node);
                    auto* group = reinterpret_cast<VkDeviceGroupSubmitInfo*>(copied);
                    if (group->commandBufferCount != source.commandBufferCount ||
                        group->waitSemaphoreCount != source.waitSemaphoreCount ||
                        group->signalSemaphoreCount != source.signalSemaphoreCount) return false;
                    prefixMasks.assign(group->pCommandBufferDeviceMasks, group->pCommandBufferDeviceMasks + split + 1);
                    suffixMasks.push_back(1);
                    suffixMasks.insert(suffixMasks.end(), group->pCommandBufferDeviceMasks + split + 1,
                        group->pCommandBufferDeviceMasks + source.commandBufferCount);
                    finalGroup.commandBufferCount = static_cast<uint32_t>(suffixMasks.size());
                    finalGroup.pCommandBufferDeviceMasks = suffixMasks.data();
                    if (source.signalSemaphoreCount) finalSignalDevices.assign(group->pSignalSemaphoreDeviceIndices,
                        group->pSignalSemaphoreDeviceIndices + source.signalSemaphoreCount);
                    group->commandBufferCount = static_cast<uint32_t>(prefixMasks.size());
                    group->pCommandBufferDeviceMasks = prefixMasks.data();
                    group->signalSemaphoreCount = 0;
                    group->pSignalSemaphoreDeviceIndices = nullptr;
                    break;
                }
                case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO:
                    if (reinterpret_cast<const VkProtectedSubmitInfo*>(node)->protectedSubmit) return false;
                    copied = clone<VkProtectedSubmitInfo>(node); break;
                case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR: copied = clone<VkPerformanceQuerySubmitInfoKHR>(node); break;
#ifdef VK_NV_low_latency2
                case VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV: copied = clone<VkLatencySubmissionPresentIdNV>(node); break;
#endif
#ifdef VK_EXT_frame_boundary
                case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT: copied = clone<VkFrameBoundaryEXT>(node); break;
#endif
#ifdef VK_KHR_win32_keyed_mutex
                case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR: return false;
#endif
                default: return false; // Never guess the size of an unknown node.
                }
                if (tail) tail->pNext = copied; else head = copied;
                tail = copied;
            }
            VkSubmitInfo first = source;
            firstTimeline.pNext = head;
            firstTimeline.waitSemaphoreValueCount = static_cast<uint32_t>(waits.size());
            firstTimeline.pWaitSemaphoreValues = waits.data();
            firstTimeline.signalSemaphoreValueCount = 1;
            firstTimeline.pSignalSemaphoreValues = &handoff.capturedValue;
            first.pNext = &firstTimeline;
            first.commandBufferCount = static_cast<uint32_t>(prefix.size()); first.pCommandBuffers = prefix.data();
            first.signalSemaphoreCount = 1; first.pSignalSemaphores = &handoff.texture;
            // A group signal index is also required for the injected signal.
            static constexpr uint32_t deviceIndex = 0;
            for (auto* n = head; n; n = n->pNext) if (n->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO) {
                auto* group = reinterpret_cast<VkDeviceGroupSubmitInfo*>(n);
                group->signalSemaphoreCount = 1; group->pSignalSemaphoreDeviceIndices = &deviceIndex;
            }
            copyTimeline.waitSemaphoreValueCount = 1; copyTimeline.pWaitSemaphoreValues = &handoff.d3d12Value;
            copyTimeline.signalSemaphoreValueCount = 1; copyTimeline.pSignalSemaphoreValues = &handoff.copiedValue;
            VkSubmitInfo copy{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            copy.pNext = &copyTimeline; copy.waitSemaphoreCount = 1; copy.pWaitSemaphores = &handoff.texture;
            copy.pWaitDstStageMask = &stage; copy.commandBufferCount = 1; copy.pCommandBuffers = &handoff.copyback;
            copy.signalSemaphoreCount = 1; copy.pSignalSemaphores = &handoff.copied;
            if (handoff.completed) { finalSignals.push_back(handoff.completed); signals.push_back(handoff.completedValue); }
            finalTimeline.waitSemaphoreValueCount = 1; finalTimeline.pWaitSemaphoreValues = &handoff.copiedValue;
            finalTimeline.signalSemaphoreValueCount = static_cast<uint32_t>(signals.size());
            finalTimeline.pSignalSemaphoreValues = signals.data();
            if (!suffixMasks.empty()) {
                finalSignalDevices.resize(finalSignals.size(), 0);
                finalGroup.signalSemaphoreCount = static_cast<uint32_t>(finalSignalDevices.size());
                finalGroup.pSignalSemaphoreDeviceIndices = finalSignalDevices.data();
                finalGroup.waitSemaphoreCount = 1; finalGroup.pWaitSemaphoreDeviceIndices = &deviceIndex;
                finalTimeline.pNext = &finalGroup;
            }
            VkSubmitInfo last{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            last.pNext = &finalTimeline; last.waitSemaphoreCount = 1; last.pWaitSemaphores = &handoff.copied;
            last.pWaitDstStageMask = &stage; last.commandBufferCount = static_cast<uint32_t>(suffix.size());
            last.pCommandBuffers = suffix.data(); last.signalSemaphoreCount = static_cast<uint32_t>(finalSignals.size());
            last.pSignalSemaphores = finalSignals.data();
            submits.push_back(first); submits.push_back(copy); submits.push_back(last);
        }
        return submits.size() == count + 2;
    }
};

struct Submit2 {
    std::vector<VkSubmitInfo2> submits;
    std::vector<VkCommandBufferSubmitInfo> prefix, suffix;
    std::vector<VkSemaphoreSubmitInfo> signals;
    VkSemaphoreSubmitInfo captured{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, ready{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},
        copied{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    VkCommandBufferSubmitInfo copyCommand{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    bool build(uint32_t count, const VkSubmitInfo2* original, Handoff h) {
        submits.reserve(count + 2);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& source = original[i];
            uint32_t split = 0;
            while (split < source.commandBufferInfoCount && source.pCommandBufferInfos[split].commandBuffer != h.original) ++split;
            if (split == source.commandBufferInfoCount) { submits.push_back(source); continue; }
            if (source.flags & VK_SUBMIT_PROTECTED_BIT) return false;
            prefix.assign(source.pCommandBufferInfos, source.pCommandBufferInfos + split + 1);
            VkCommandBufferSubmitInfo continuation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            continuation.commandBuffer = h.continuation; continuation.deviceMask = 1;
            suffix.push_back(continuation);
            suffix.insert(suffix.end(), source.pCommandBufferInfos + split + 1,
                source.pCommandBufferInfos + source.commandBufferInfoCount);
            if (source.signalSemaphoreInfoCount) signals.assign(source.pSignalSemaphoreInfos,
                source.pSignalSemaphoreInfos + source.signalSemaphoreInfoCount);
            captured.semaphore = h.texture; captured.value = h.capturedValue; captured.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ready = captured; ready.value = h.d3d12Value;
            copied = captured; copied.semaphore = h.copied; copied.value = h.copiedValue;
            VkSubmitInfo2 first = source;
            first.commandBufferInfoCount = static_cast<uint32_t>(prefix.size()); first.pCommandBufferInfos = prefix.data();
            first.signalSemaphoreInfoCount = 1; first.pSignalSemaphoreInfos = &captured;
            copyCommand.commandBuffer = h.copyback; copyCommand.deviceMask = 1;
            VkSubmitInfo2 copy{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            copy.waitSemaphoreInfoCount = 1; copy.pWaitSemaphoreInfos = &ready;
            copy.commandBufferInfoCount = 1; copy.pCommandBufferInfos = &copyCommand;
            copy.signalSemaphoreInfoCount = 1; copy.pSignalSemaphoreInfos = &copied;
            if (h.completed) { auto completion = copied; completion.semaphore = h.completed;
                completion.value = h.completedValue; signals.push_back(completion); }
            VkSubmitInfo2 last{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            last.waitSemaphoreInfoCount = 1; last.pWaitSemaphoreInfos = &copied;
            last.commandBufferInfoCount = static_cast<uint32_t>(suffix.size()); last.pCommandBufferInfos = suffix.data();
            last.signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size()); last.pSignalSemaphoreInfos = signals.data();
            submits.push_back(first); submits.push_back(copy); submits.push_back(last);
        }
        return submits.size() == count + 2;
    }
};
} // namespace vulkan_interop
