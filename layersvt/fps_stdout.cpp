/*
 * Vulkan
 *
 * Copyright (C) 2016-2026 Valve Corporation
 * Copyright (C) 2016-2026 LunarG, Inc.
 * Copyright (C) 2016-2026 Google Inc.
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
 *
 * Author: Chris Forbes <chrisforbes@google.com>
 * Author: Tony Barbour <tony@lunarg.com>
 */
#include "vk_layer_table.h"
#include <vulkan/layer/vk_layer_settings.hpp>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unordered_map>

#include <vulkan/vulkan.h>

#if defined(_WIN32) && !defined(NDEBUG)
#include <crtdbg.h>
#endif

#define FPS_PRINT_INTERVAL_SECONDS 0.5f

struct fps_stdout_layer_data {
    VkuDeviceDispatchTable *device_dispatch_table{};
    VkuInstanceDispatchTable *instance_dispatch_table{};

    PFN_vkQueuePresentKHR pfnQueuePresentKHR{};

    VkPhysicalDevice gpu{};
    VkDevice device{};

    PFN_vkSetDeviceLoaderData pfn_dev_init{};
    int lastFrame{};
    time_t lastTime{};
    float fps{};
    int frame{};
};

static std::unordered_map<VkPhysicalDevice, VkInstance> layer_instances;
static std::unordered_map<void *, fps_stdout_layer_data *> layer_data_map;

template fps_stdout_layer_data *GetLayerDataPtr<fps_stdout_layer_data>(void *data_key,
                                                                       std::unordered_map<void *, fps_stdout_layer_data *> &data_map);

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                              const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    fprintf(stdout, "[fps_stdout] vkCreateDevice\n");
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

    fps_stdout_layer_data *my_device_data = GetLayerDataPtr(get_dispatch_key(*pDevice), layer_data_map);

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
    my_device_data->frame = 0;
    my_device_data->lastFrame = 0;
    my_device_data->fps = 0.0f;
    time(&my_device_data->lastTime);

    // Get our WSI hooks in
    VkuDeviceDispatchTable *pTable = my_device_data->device_dispatch_table;
    my_device_data->pfnQueuePresentKHR = (PFN_vkQueuePresentKHR)pTable->GetDeviceProcAddr(*pDevice, "vkQueuePresentKHR");

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                          VkPhysicalDevice *pPhysicalDevices) {
    fprintf(stdout, "[fps_stdout] vkEnumeratePhysicalDevices\n");
    fflush(stdout);
    dispatch_key key = get_dispatch_key(instance);
    fps_stdout_layer_data *my_data = GetLayerDataPtr(key, layer_data_map);
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
    fprintf(stdout, "[fps_stdout] vkEnumeratePhysicalDeviceGroups\n");
    fflush(stdout);
    dispatch_key key = get_dispatch_key(instance);
    fps_stdout_layer_data *my_data = GetLayerDataPtr(key, layer_data_map);
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
    fprintf(stdout, "[fps_stdout] vkDestroyDevice\n");
    fflush(stdout);
    dispatch_key key = get_dispatch_key(device);
    fps_stdout_layer_data *my_data = GetLayerDataPtr(key, layer_data_map);
    VkuDeviceDispatchTable *pTable = my_data->device_dispatch_table;
    pTable->DeviceWaitIdle(device);
    pTable->DestroyDevice(device, pAllocator);
    delete pTable;
    layer_data_map.erase(key);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                                VkInstance *pInstance) {
    fprintf(stdout, "[fps_stdout] vkCreateInstance\n");
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

    fps_stdout_layer_data *my_data = GetLayerDataPtr(get_dispatch_key(*pInstance), layer_data_map);
    my_data->instance_dispatch_table = new VkuInstanceDispatchTable;
    vkuInitInstanceDispatchTable(*pInstance, my_data->instance_dispatch_table, fpGetInstanceProcAddr);

    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    fprintf(stdout, "[fps_stdout] vkDestroyInstance\n");
    fflush(stdout);
    dispatch_key key = get_dispatch_key(instance);
    fps_stdout_layer_data *my_data = GetLayerDataPtr(key, layer_data_map);
    VkuInstanceDispatchTable *pTable = my_data->instance_dispatch_table;
    pTable->DestroyInstance(instance, pAllocator);
    delete pTable;
    layer_data_map.erase(key);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    fprintf(stdout, "[fps_stdout] vkQueuePresentKHR\n");
    fflush(stdout);
    fps_stdout_layer_data *my_data = GetLayerDataPtr(get_dispatch_key(queue), layer_data_map);

    time_t now;
    time(&now);
    float seconds = (float)difftime(now, my_data->lastTime);

    if (seconds > FPS_PRINT_INTERVAL_SECONDS) {
        my_data->fps = (my_data->frame - my_data->lastFrame) / seconds;
        my_data->lastFrame = my_data->frame;
        my_data->lastTime = now;
        fprintf(stdout, "FPS = %.2f\n", my_data->fps);
        fflush(stdout);
    }
    my_data->frame++;

    VkResult result = my_data->pfnQueuePresentKHR(queue, pPresentInfo);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceToolPropertiesEXT(VkPhysicalDevice physicalDevice, uint32_t *pToolCount,
                                                                    VkPhysicalDeviceToolPropertiesEXT *pToolProperties) {
    fprintf(stdout, "[fps_stdout] vkGetPhysicalDeviceToolPropertiesEXT\n");
    fflush(stdout);
    static const VkPhysicalDeviceToolPropertiesEXT fps_stdout_layer_tool_props = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES_EXT,
        nullptr,
        "FPS Stdout Layer",
        "1",
        VK_TOOL_PURPOSE_PROFILING_BIT_EXT | VK_TOOL_PURPOSE_ADDITIONAL_FEATURES_BIT_EXT,
        "The VK_LAYER_LUNARG_fps_stdout utility layer prints the real-time frames-per-second value to stdout.",
        "VK_LAYER_LUNARG_fps_stdout"};

    auto original_pToolProperties = pToolProperties;
    if (pToolProperties != nullptr) {
        *pToolProperties = fps_stdout_layer_tool_props;
        pToolProperties = ((*pToolCount > 1) ? &pToolProperties[1] : nullptr);
        (*pToolCount)--;
    }

    fps_stdout_layer_data *my_data = GetLayerDataPtr(get_dispatch_key(physicalDevice), layer_data_map);
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
    fprintf(stdout, "[fps_stdout] vkGetDeviceProcAddr\n");
    fflush(stdout);
#define ADD_HOOK(fn) \
    if (!strncmp(#fn, funcName, sizeof(#fn))) return (PFN_vkVoidFunction)fn

    ADD_HOOK(vkGetDeviceProcAddr);
    ADD_HOOK(vkDestroyDevice);
    ADD_HOOK(vkQueuePresentKHR);
#undef ADD_HOOK

    if (dev == NULL) return NULL;

    fps_stdout_layer_data *dev_data;
    dev_data = GetLayerDataPtr(get_dispatch_key(dev), layer_data_map);
    VkuDeviceDispatchTable *pTable = dev_data->device_dispatch_table;

    if (pTable->GetDeviceProcAddr == NULL) return NULL;
    return pTable->GetDeviceProcAddr(dev, funcName);
}

EXPORT_FUNCTION VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
    fprintf(stdout, "[fps_stdout] vkGetInstanceProcAddr\n");
    fflush(stdout);
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

    fps_stdout_layer_data *instance_data;
    instance_data = GetLayerDataPtr(get_dispatch_key(instance), layer_data_map);
    VkuInstanceDispatchTable *pTable = instance_data->instance_dispatch_table;

    if (pTable->GetInstanceProcAddr == NULL) return NULL;
    return pTable->GetInstanceProcAddr(instance, funcName);
}
