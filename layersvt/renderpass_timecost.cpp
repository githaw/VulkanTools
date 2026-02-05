/*
 * Vulkan
 *
 * Copyright (C) 2026 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "vk_layer_table.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#if defined(_WIN32) && !defined(NDEBUG)
#include <crtdbg.h>
#endif

static constexpr uint32_t kMaxQueriesPerCmdBuffer = 1024;

struct RenderPassRecord {
    uint32_t start_query = 0;
    uint32_t end_query = 0;
    VkRenderPass renderpass = VK_NULL_HANDLE;
    uint32_t subpass = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

struct CommandBufferState {
    VkQueryPool query_pool = VK_NULL_HANDLE;
    uint32_t max_queries = 0;
    uint32_t next_query = 0;
    bool warned_overflow = false;
    int32_t active_renderpass = -1;
    VkRenderPass current_renderpass = VK_NULL_HANDLE;
    uint32_t current_subpass = 0;
    std::vector<RenderPassRecord> renderpasses;
    std::vector<uint64_t> time_stamps;
};

struct renderpass_timecost_layer_data {
    VkuDeviceDispatchTable *device_dispatch_table{};
    VkuInstanceDispatchTable *instance_dispatch_table{};

    VkPhysicalDevice gpu{};
    VkDevice device{};

    PFN_vkSetDeviceLoaderData pfn_dev_init{};
    float timestamp_period_ns{};

    std::unordered_map<VkCommandBuffer, CommandBufferState> command_buffers;
};

static std::unordered_map<VkPhysicalDevice, VkInstance> layer_instances;
static std::unordered_map<void *, renderpass_timecost_layer_data *> layer_data_map;

template renderpass_timecost_layer_data *GetLayerDataPtr<renderpass_timecost_layer_data>(
    void *data_key, std::unordered_map<void *, renderpass_timecost_layer_data *> &data_map);

static void InitCommandBufferState(renderpass_timecost_layer_data *dev_data, VkCommandBuffer command_buffer) {
    auto &state = dev_data->command_buffers[command_buffer];
    if (state.query_pool == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo pool_ci = {};
        pool_ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        pool_ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        pool_ci.queryCount = kMaxQueriesPerCmdBuffer;
        VkResult pool_res = dev_data->device_dispatch_table->CreateQueryPool(dev_data->device, &pool_ci, nullptr, &state.query_pool);
        if (pool_res != VK_SUCCESS) {
            state.query_pool = VK_NULL_HANDLE;
            state.max_queries = 0;
        } else {
            state.max_queries = kMaxQueriesPerCmdBuffer;
        }
    }

    state.next_query = 0;
    state.renderpasses.clear();
    state.warned_overflow = false;
    state.active_renderpass = -1;
    state.current_renderpass = VK_NULL_HANDLE;
    state.current_subpass = 0;
    state.time_stamps.clear();
    if (state.max_queries > 0) {
        state.time_stamps.reserve(state.max_queries);
    }
}

static bool ReserveQueryPair(CommandBufferState &state, RenderPassRecord &record) {
    if (state.query_pool == VK_NULL_HANDLE) return false;
    if (state.next_query + 1 >= state.max_queries) {
        return false;
    }
    record.start_query = state.next_query++;
    record.end_query = state.next_query++;
    state.renderpasses.push_back(record);
    return true;
}

static void DumpRenderPassTimings(renderpass_timecost_layer_data *dev_data, const char *wait_label, uint32_t fence_count,
                                  const VkFence *fences, VkBool32 wait_all) {
    if (!dev_data || !dev_data->device_dispatch_table) return;

    for (auto &entry : dev_data->command_buffers) {
        VkCommandBuffer command_buffer = entry.first;
        CommandBufferState &state = entry.second;
        if (state.query_pool == VK_NULL_HANDLE) continue;
        if (state.renderpasses.empty() || state.next_query == 0) continue;

        if (state.time_stamps.size() < state.next_query) {
            state.time_stamps.resize(state.next_query);
        }
        VkResult query_res =
            dev_data->device_dispatch_table->GetQueryPoolResults(dev_data->device, state.query_pool, 0, state.next_query,
                                                                 state.time_stamps.size() * sizeof(uint64_t),
                                                                 state.time_stamps.data(), sizeof(uint64_t),
                                                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (query_res != VK_SUCCESS) continue;

        for (size_t i = 0; i < state.renderpasses.size(); ++i) {
            const RenderPassRecord &record = state.renderpasses[i];
            uint64_t start = state.time_stamps[record.start_query];
            uint64_t end = state.time_stamps[record.end_query];
            double time_ms = (double)(end - start) * (double)dev_data->timestamp_period_ns / 1000000.0;
            if (fences && fence_count > 0) {
                fprintf(stdout,
                        "[renderpass_timecost][%s] fence_count=%u wait_all=%u fence0=%p cmd_buf=%p renderpass=%zu rp=%p subpass=%u "
                        "pipeline=%p bind_point=%d time_ms=%.3f\n",
                        wait_label, fence_count, wait_all, (void *)fences[0], (void *)command_buffer, i,
                        (void *)record.renderpass, record.subpass, (void *)record.pipeline, (int)record.bind_point, time_ms);
            } else {
                fprintf(stdout,
                        "[renderpass_timecost][%s] cmd_buf=%p renderpass=%zu rp=%p subpass=%u pipeline=%p bind_point=%d "
                        "time_ms=%.3f\n",
                        wait_label, (void *)command_buffer, i, (void *)record.renderpass, record.subpass, (void *)record.pipeline,
                        (int)record.bind_point, time_ms);
            }
        }
        fflush(stdout);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                              const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    fprintf(stdout, "[renderpass_timecost] vkCreateDevice\n");
    fflush(stdout);
    VkLayerDeviceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(layer_instances.at(gpu), "vkCreateDevice");
    if (fpCreateDevice == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fpCreateDevice(gpu, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }

    renderpass_timecost_layer_data *my_device_data = GetLayerDataPtr(get_dispatch_key(*pDevice), layer_data_map);

    // Setup device dispatch table
    my_device_data->device_dispatch_table = new VkuDeviceDispatchTable;
    vkuInitDeviceDispatchTable(*pDevice, my_device_data->device_dispatch_table, fpGetDeviceProcAddr);

    // store the loader callback for initializing created dispatchable objects
    chain_info = get_chain_info(pCreateInfo, VK_LOADER_DATA_CALLBACK);
    if (chain_info) {
        my_device_data->pfn_dev_init = chain_info->u.pfnSetDeviceLoaderData;
    } else {
        my_device_data->pfn_dev_init = NULL;
    }

    my_device_data->gpu = gpu;
    my_device_data->device = *pDevice;

    VkInstance instance = layer_instances.at(gpu);
    renderpass_timecost_layer_data *instance_data = GetLayerDataPtr(get_dispatch_key(instance), layer_data_map);
    VkPhysicalDeviceProperties props = {};
    instance_data->instance_dispatch_table->GetPhysicalDeviceProperties(gpu, &props);
    my_device_data->timestamp_period_ns = props.limits.timestampPeriod;

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                          VkPhysicalDevice *pPhysicalDevices) {
    fprintf(stdout, "[renderpass_timecost] vkEnumeratePhysicalDevices\n");
    fflush(stdout);
    dispatch_key key = get_dispatch_key(instance);
    renderpass_timecost_layer_data *my_data = GetLayerDataPtr(key, layer_data_map);
    VkuInstanceDispatchTable *pTable = my_data->instance_dispatch_table;

    VkResult result = pTable->EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);

    if (pPhysicalDevices != nullptr) {
        for (int i = 0; i < *pPhysicalDeviceCount; ++i) {
            if (layer_instances.count(pPhysicalDevices[i]) == 0) {
                layer_instances.insert({pPhysicalDevices[i], instance});
            }
        }
    }

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *pPhysicalDeviceGroupCount,
                                                               VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties) {
    fprintf(stdout, "[renderpass_timecost] vkEnumeratePhysicalDeviceGroups\n");
    fflush(stdout);
    dispatch_key key = get_dispatch_key(instance);
    renderpass_timecost_layer_data *my_data = GetLayerDataPtr(key, layer_data_map);
    VkuInstanceDispatchTable *pTable = my_data->instance_dispatch_table;

    VkResult result = pTable->EnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);

    if (pPhysicalDeviceGroupProperties != nullptr) {
        for (int i = 0; i < *pPhysicalDeviceGroupCount; ++i) {
            for (int j = 0; j < pPhysicalDeviceGroupProperties[i].physicalDeviceCount; ++j) {
                if (layer_instances.count(pPhysicalDeviceGroupProperties[i].physicalDevices[j]) == 0) {
                    layer_instances.insert({pPhysicalDeviceGroupProperties[i].physicalDevices[j], instance});
                }
            }
        }
    }

    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    fprintf(stdout, "[renderpass_timecost] vkDestroyDevice\n");
    fflush(stdout);
    dispatch_key key = get_dispatch_key(device);
    renderpass_timecost_layer_data *my_data = GetLayerDataPtr(key, layer_data_map);
    VkuDeviceDispatchTable *pTable = my_data->device_dispatch_table;

    for (auto &entry : my_data->command_buffers) {
        if (entry.second.query_pool != VK_NULL_HANDLE) {
            pTable->DestroyQueryPool(device, entry.second.query_pool, nullptr);
        }
    }
    my_data->command_buffers.clear();

    pTable->DeviceWaitIdle(device);
    pTable->DestroyDevice(device, pAllocator);
    delete pTable;
    layer_data_map.erase(key);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                                VkInstance *pInstance) {
    fprintf(stdout, "[renderpass_timecost] vkCreateInstance\n");
    fflush(stdout);
#if defined(_WIN32) && defined(_CRTDBG_MODE_FILE)
#if !defined(NDEBUG)
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    // Avoid "Abort, Retry, Ignore" dialog boxes
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

    VkLayerInstanceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (fpCreateInstance == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    renderpass_timecost_layer_data *my_data = GetLayerDataPtr(get_dispatch_key(*pInstance), layer_data_map);
    my_data->instance_dispatch_table = new VkuInstanceDispatchTable;
    vkuInitInstanceDispatchTable(*pInstance, my_data->instance_dispatch_table, fpGetInstanceProcAddr);

    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    fprintf(stdout, "[renderpass_timecost] vkDestroyInstance\n");
    fflush(stdout);
    dispatch_key key = get_dispatch_key(instance);
    renderpass_timecost_layer_data *my_data = GetLayerDataPtr(key, layer_data_map);
    VkuInstanceDispatchTable *pTable = my_data->instance_dispatch_table;
    pTable->DestroyInstance(instance, pAllocator);
    delete pTable;
    layer_data_map.erase(key);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
                                                        VkCommandBuffer *pCommandBuffers) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    VkResult result = pTable->AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
    if (result != VK_SUCCESS) return result;

    if (dev_data->pfn_dev_init) {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            dev_data->pfn_dev_init(device, (void *)pCommandBuffers[i]);
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
                                                const VkCommandBuffer *pCommandBuffers) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;

    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        auto it = dev_data->command_buffers.find(pCommandBuffers[i]);
        if (it != dev_data->command_buffers.end()) {
            if (it->second.query_pool != VK_NULL_HANDLE) {
                pTable->DestroyQueryPool(device, it->second.query_pool, nullptr);
            }
            dev_data->command_buffers.erase(it);
        }
    }

    pTable->FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo) {
    fprintf(stdout, "[renderpass_timecost] vkBeginCommandBuffer\n");
    fflush(stdout);
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;

    VkResult result = pTable->BeginCommandBuffer(commandBuffer, pBeginInfo);
    if (result != VK_SUCCESS) return result;

    InitCommandBufferState(dev_data, commandBuffer);
    auto &state = dev_data->command_buffers[commandBuffer];
    if (state.query_pool != VK_NULL_HANDLE) {
        if (pTable->CmdResetQueryPool) {
            pTable->CmdResetQueryPool(commandBuffer, state.query_pool, 0, state.max_queries);
        }
    }

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags) {
    fprintf(stdout, "[renderpass_timecost] vkResetCommandBuffer\n");
    fflush(stdout);
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    VkResult result = pTable->ResetCommandBuffer(commandBuffer, flags);
    if (result == VK_SUCCESS) {
        auto it = dev_data->command_buffers.find(commandBuffer);
        if (it != dev_data->command_buffers.end()) {
            it->second.next_query = 0;
            it->second.renderpasses.clear();
            it->second.warned_overflow = false;
            it->second.active_renderpass = -1;
            it->second.current_renderpass = VK_NULL_HANDLE;
            it->second.current_subpass = 0;
            it->second.time_stamps.clear();
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin,
                                                VkSubpassContents contents) {
    fprintf(stdout, "[renderpass_timecost] vkCmdBeginRenderPass\n");
    fflush(stdout);
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    auto &state = dev_data->command_buffers[commandBuffer];

    RenderPassRecord record = {};
    if (ReserveQueryPair(state, record)) {
        pTable->CmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, state.query_pool, record.start_query);
        state.active_renderpass = static_cast<int32_t>(state.renderpasses.size() - 1);
        state.current_renderpass = pRenderPassBegin->renderPass;
        state.current_subpass = 0;
        RenderPassRecord &active = state.renderpasses.back();
        active.renderpass = pRenderPassBegin->renderPass;
        active.subpass = state.current_subpass;
    } else {
        if (!state.warned_overflow) {
            fprintf(stdout, "[renderpass_timecost] Query pool overflow, skip recording render pass\n");
            fflush(stdout);
            state.warned_overflow = true;
        }
        state.active_renderpass = -1;
    }

    pTable->CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer) {
    fprintf(stdout, "[renderpass_timecost] vkCmdEndRenderPass\n");
    fflush(stdout);
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    auto &state = dev_data->command_buffers[commandBuffer];

    pTable->CmdEndRenderPass(commandBuffer);

    if (state.active_renderpass >= 0 && state.active_renderpass < static_cast<int32_t>(state.renderpasses.size())) {
        const auto &record = state.renderpasses[static_cast<size_t>(state.active_renderpass)];
        pTable->CmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, state.query_pool, record.end_query);
    }
    state.active_renderpass = -1;
    state.current_renderpass = VK_NULL_HANDLE;
    state.current_subpass = 0;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin,
                                                 const VkSubpassBeginInfo *pSubpassBeginInfo) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    auto &state = dev_data->command_buffers[commandBuffer];

    RenderPassRecord record = {};
    if (ReserveQueryPair(state, record)) {
        pTable->CmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, state.query_pool, record.start_query);
        state.active_renderpass = static_cast<int32_t>(state.renderpasses.size() - 1);
        state.current_renderpass = pRenderPassBegin->renderPass;
        state.current_subpass = 0;
        RenderPassRecord &active = state.renderpasses.back();
        active.renderpass = pRenderPassBegin->renderPass;
        active.subpass = state.current_subpass;
    } else {
        if (!state.warned_overflow) {
            fprintf(stdout, "[renderpass_timecost] Query pool overflow, skip recording render pass\n");
            fflush(stdout);
            state.warned_overflow = true;
        }
        state.active_renderpass = -1;
    }

    if (pTable->CmdBeginRenderPass2) {
        pTable->CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    } else if (pTable->CmdBeginRenderPass2KHR) {
        pTable->CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfo *pSubpassEndInfo) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    auto &state = dev_data->command_buffers[commandBuffer];

    if (pTable->CmdEndRenderPass2) {
        pTable->CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
    } else if (pTable->CmdEndRenderPass2KHR) {
        pTable->CmdEndRenderPass2KHR(commandBuffer, pSubpassEndInfo);
    }

    if (state.active_renderpass >= 0 && state.active_renderpass < static_cast<int32_t>(state.renderpasses.size())) {
        const auto &record = state.renderpasses[static_cast<size_t>(state.active_renderpass)];
        pTable->CmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, state.query_pool, record.end_query);
    }
    state.active_renderpass = -1;
    state.current_renderpass = VK_NULL_HANDLE;
    state.current_subpass = 0;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin,
                                                    const VkSubpassBeginInfo *pSubpassBeginInfo) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    auto &state = dev_data->command_buffers[commandBuffer];

    RenderPassRecord record = {};
    if (ReserveQueryPair(state, record)) {
        pTable->CmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, state.query_pool, record.start_query);
        state.active_renderpass = static_cast<int32_t>(state.renderpasses.size() - 1);
        state.current_renderpass = pRenderPassBegin->renderPass;
        state.current_subpass = 0;
        RenderPassRecord &active = state.renderpasses.back();
        active.renderpass = pRenderPassBegin->renderPass;
        active.subpass = state.current_subpass;
    } else {
        if (!state.warned_overflow) {
            fprintf(stdout, "[renderpass_timecost] Query pool overflow, skip recording render pass\n");
            fflush(stdout);
            state.warned_overflow = true;
        }
        state.active_renderpass = -1;
    }

    if (pTable->CmdBeginRenderPass2KHR) {
        pTable->CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    } else {
        pTable->CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2KHR(VkCommandBuffer commandBuffer, const VkSubpassEndInfo *pSubpassEndInfo) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    auto &state = dev_data->command_buffers[commandBuffer];

    if (pTable->CmdEndRenderPass2KHR) {
        pTable->CmdEndRenderPass2KHR(commandBuffer, pSubpassEndInfo);
    } else {
        pTable->CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
    }

    if (state.active_renderpass >= 0 && state.active_renderpass < static_cast<int32_t>(state.renderpasses.size())) {
        const auto &record = state.renderpasses[static_cast<size_t>(state.active_renderpass)];
        pTable->CmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, state.query_pool, record.end_query);
    }
    state.active_renderpass = -1;
    state.current_renderpass = VK_NULL_HANDLE;
    state.current_subpass = 0;
}

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    auto &state = dev_data->command_buffers[commandBuffer];

    if (pTable->CmdNextSubpass) {
        pTable->CmdNextSubpass(commandBuffer, contents);
    }

    if (state.current_renderpass != VK_NULL_HANDLE) {
        state.current_subpass++;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2(VkCommandBuffer commandBuffer, const VkSubpassBeginInfo *pSubpassBeginInfo,
                                             const VkSubpassEndInfo *pSubpassEndInfo) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    auto &state = dev_data->command_buffers[commandBuffer];

    if (pTable->CmdNextSubpass2) {
        pTable->CmdNextSubpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
    } else if (pTable->CmdNextSubpass2KHR) {
        pTable->CmdNextSubpass2KHR(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
    }

    if (state.current_renderpass != VK_NULL_HANDLE) {
        state.current_subpass++;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2KHR(VkCommandBuffer commandBuffer, const VkSubpassBeginInfo *pSubpassBeginInfo,
                                                const VkSubpassEndInfo *pSubpassEndInfo) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    auto &state = dev_data->command_buffers[commandBuffer];

    if (pTable->CmdNextSubpass2KHR) {
        pTable->CmdNextSubpass2KHR(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
    } else if (pTable->CmdNextSubpass2) {
        pTable->CmdNextSubpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
    }

    if (state.current_renderpass != VK_NULL_HANDLE) {
        state.current_subpass++;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                                             VkPipeline pipeline) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(commandBuffer), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;
    auto &state = dev_data->command_buffers[commandBuffer];

    if (pTable->CmdBindPipeline) {
        pTable->CmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
    }

    if (state.active_renderpass >= 0 && state.active_renderpass < static_cast<int32_t>(state.renderpasses.size())) {
        RenderPassRecord &record = state.renderpasses[static_cast<size_t>(state.active_renderpass)];
        record.pipeline = pipeline;
        record.bind_point = pipelineBindPoint;
        record.subpass = state.current_subpass;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(queue), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;

    VkResult result = pTable->QueueWaitIdle(queue);
    if (result != VK_SUCCESS) return result;

    DumpRenderPassTimings(dev_data, "vkQueueWaitIdle", 0, nullptr, VK_FALSE);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;

    VkResult result = pTable->DeviceWaitIdle(device);
    if (result != VK_SUCCESS) return result;

    DumpRenderPassTimings(dev_data, "vkDeviceWaitIdle", 0, nullptr, VK_FALSE);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences, VkBool32 waitAll,
                                               uint64_t timeout) {
    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;

    VkResult result = pTable->WaitForFences(device, fenceCount, pFences, waitAll, timeout);
    if (result != VK_SUCCESS) return result;

    DumpRenderPassTimings(dev_data, "vkWaitForFences", fenceCount, pFences, waitAll);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceToolPropertiesEXT(VkPhysicalDevice physicalDevice, uint32_t *pToolCount,
                                                                    VkPhysicalDeviceToolPropertiesEXT *pToolProperties) {
    fprintf(stdout, "[renderpass_timecost] vkGetPhysicalDeviceToolPropertiesEXT\n");
    fflush(stdout);
    static const VkPhysicalDeviceToolPropertiesEXT layer_tool_props = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES_EXT,
        nullptr,
        "RenderPass Timecost Layer",
        "1",
        VK_TOOL_PURPOSE_PROFILING_BIT_EXT | VK_TOOL_PURPOSE_ADDITIONAL_FEATURES_BIT_EXT,
        "The VK_LAYER_LUNARG_renderpass_timecost utility layer prints per-renderpass time cost to stdout.",
        "VK_LAYER_LUNARG_renderpass_timecost"};

    auto original_pToolProperties = pToolProperties;
    if (pToolProperties != nullptr) {
        *pToolProperties = layer_tool_props;
        pToolProperties = ((*pToolCount > 1) ? &pToolProperties[1] : nullptr);
        (*pToolCount)--;
    }

    renderpass_timecost_layer_data *my_data = GetLayerDataPtr(get_dispatch_key(physicalDevice), layer_data_map);
    VkResult result =
        my_data->instance_dispatch_table->GetPhysicalDeviceToolPropertiesEXT(physicalDevice, pToolCount, pToolProperties);

    if (original_pToolProperties != nullptr) {
        pToolProperties = original_pToolProperties;
    }

    (*pToolCount)++;

    return result;
}

#if defined(__GNUC__) && __GNUC__ >= 4
#define EXPORT_FUNCTION __attribute__((visibility("default")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x590)
#define EXPORT_FUNCTION __attribute__((visibility("default")))
#else
#define EXPORT_FUNCTION
#endif

EXPORT_FUNCTION VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *funcName) {

#define ADD_HOOK(fn) \
    if (!strncmp(#fn, funcName, sizeof(#fn))) return (PFN_vkVoidFunction)fn

    ADD_HOOK(vkGetDeviceProcAddr);
    ADD_HOOK(vkDestroyDevice);
    ADD_HOOK(vkQueueWaitIdle);
    ADD_HOOK(vkDeviceWaitIdle);
    ADD_HOOK(vkWaitForFences);
    ADD_HOOK(vkAllocateCommandBuffers);
    ADD_HOOK(vkFreeCommandBuffers);
    ADD_HOOK(vkBeginCommandBuffer);
    ADD_HOOK(vkResetCommandBuffer);
    ADD_HOOK(vkCmdBeginRenderPass);
    ADD_HOOK(vkCmdEndRenderPass);
    ADD_HOOK(vkCmdBeginRenderPass2);
    ADD_HOOK(vkCmdEndRenderPass2);
    ADD_HOOK(vkCmdBeginRenderPass2KHR);
    ADD_HOOK(vkCmdEndRenderPass2KHR);
    ADD_HOOK(vkCmdNextSubpass);
    ADD_HOOK(vkCmdNextSubpass2);
    ADD_HOOK(vkCmdNextSubpass2KHR);
    ADD_HOOK(vkCmdBindPipeline);
#undef ADD_HOOK

    if (dev == NULL) return NULL;

    renderpass_timecost_layer_data *dev_data = GetLayerDataPtr(get_dispatch_key(dev), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;

    if (pTable->GetDeviceProcAddr == NULL) return NULL;
    return pTable->GetDeviceProcAddr(dev, funcName);
}

EXPORT_FUNCTION VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
#define ADD_HOOK(fn) \
    if (!strncmp(#fn, funcName, sizeof(#fn))) return (PFN_vkVoidFunction)fn

    ADD_HOOK(vkCreateInstance);
    ADD_HOOK(vkEnumeratePhysicalDevices);
    ADD_HOOK(vkEnumeratePhysicalDeviceGroups);
    ADD_HOOK(vkCreateDevice);
    ADD_HOOK(vkDestroyInstance);
    ADD_HOOK(vkGetInstanceProcAddr);
    ADD_HOOK(vkGetPhysicalDeviceToolPropertiesEXT);
#undef ADD_HOOK

    if (instance == NULL) return NULL;

    renderpass_timecost_layer_data *instance_data = GetLayerDataPtr(get_dispatch_key(instance), layer_data_map);
    VkuInstanceDispatchTable *pTable = instance_data->instance_dispatch_table;

    if (pTable->GetInstanceProcAddr == NULL) return NULL;
    return pTable->GetInstanceProcAddr(instance, funcName);
}
