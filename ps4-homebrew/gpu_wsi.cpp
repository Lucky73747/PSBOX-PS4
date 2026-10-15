// SPDX-License-Identifier: GPL-3.0-or-later
// Small Vulkan upload/present path for the PS4 homebrew bootstrap.
#include "gpu_wsi.h"

#include <orbis/libkernel.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>

namespace xc::ps4 {
namespace {

void logLine(const char* fmt, ...) {
    char body[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[900];
    std::snprintf(line, sizeof(line), "[%s] %s\n", kBuild, body);
}

[[noreturn]] void fail(const char* step, VkResult result = VK_SUCCESS) {
    if (result == VK_SUCCESS) logLine("FATAL step=%s", step);
    else logLine("FATAL step=%s VkResult=%d", step, static_cast<int>(result));
    logLine("App remains idle for klog inspection; close from PS button menu");
    for (;;) sceKernelUsleep(1000000);
}

void must(VkResult result, const char* step) {
    if (result != VK_SUCCESS) fail(step, result);
}

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags& chosen) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const auto flags = props.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (flags & required) == required && (flags & preferred) == preferred) {
            chosen = flags;
            return i;
        }
    }
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const auto flags = props.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (flags & required) == required) {
            chosen = flags;
            return i;
        }
    }
    fail("host-visible-memory-type");
}

void makeCanvasPixels(const uint32_t* source, uint32_t sourceWidth, uint32_t sourceHeight,
                      uint32_t width, uint32_t height, uint32_t* destination) {
    if (sourceWidth == width && sourceHeight == height) {
        std::memcpy(destination, source, static_cast<size_t>(width) * height * 4);
        return;
    }
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t sy = std::min(sourceHeight - 1, static_cast<uint32_t>((uint64_t)y * sourceHeight / height));
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t sx = std::min(sourceWidth - 1, static_cast<uint32_t>((uint64_t)x * sourceWidth / width));
            destination[static_cast<size_t>(y) * width + x] = source[static_cast<size_t>(sy) * sourceWidth + sx];
        }
    }
}
} // namespace

[[noreturn]] void presentCanvas(uint32_t* rgba, uint32_t canvasWidth, uint32_t canvasHeight,
    if (!rgba || !canvasWidth || !canvasHeight) fail("invalid-canvas");
    logLine("backend=mesa-orbis-wsi surface=VK_EXT_headless_surface present=VideoOut");

    uint32_t extCount = 0;
    must(vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr), "instance-ext-count");
    std::vector<VkExtensionProperties> instanceExts(extCount);
    must(vkEnumerateInstanceExtensionProperties(nullptr, &extCount, instanceExts.data()), "instance-ext-list");
    auto hasInstanceExt = [&](const char* name) {
        return std::any_of(instanceExts.begin(), instanceExts.end(), [=](const auto& e) {
            return std::strcmp(e.extensionName, name) == 0;
        });
    };
    if (!hasInstanceExt(VK_KHR_SURFACE_EXTENSION_NAME) || !hasInstanceExt(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
        fail("required-surface-extension-missing");

    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    appInfo.pEngineName = "xcloud-ps4-bootstrap";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    VkInstance instance = VK_NULL_HANDLE;
    must(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");

    auto createHeadless = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
    if (!createHeadless) fail("vkCreateHeadlessSurfaceEXT-not-loaded");
    VkHeadlessSurfaceCreateInfoEXT surfaceInfo{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    must(createHeadless(instance, &surfaceInfo, nullptr, &surface), "vkCreateHeadlessSurfaceEXT");
    logLine("headless surface created");

    uint32_t physicalCount = 0;
    must(vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr), "physical-device-count");
    if (!physicalCount) fail("no-vulkan-physical-device");
    std::vector<VkPhysicalDevice> physicals(physicalCount);
    must(vkEnumeratePhysicalDevices(instance, &physicalCount, physicals.data()), "physical-device-list");
    VkPhysicalDevice physical = physicals[0];
    VkPhysicalDeviceProperties physicalProps{};
    vkGetPhysicalDeviceProperties(physical, &physicalProps);
    logLine("device=%s api=%u.%u.%u driver=0x%08x", physicalProps.deviceName,
            VK_VERSION_MAJOR(physicalProps.apiVersion), VK_VERSION_MINOR(physicalProps.apiVersion),
            VK_VERSION_PATCH(physicalProps.apiVersion), physicalProps.driverVersion);

    uint32_t deviceExtCount = 0;
    must(vkEnumerateDeviceExtensionProperties(physical, nullptr, &deviceExtCount, nullptr), "device-ext-count");
    std::vector<VkExtensionProperties> deviceExts(deviceExtCount);
    must(vkEnumerateDeviceExtensionProperties(physical, nullptr, &deviceExtCount, deviceExts.data()), "device-ext-list");
    const bool hasSwapchain = std::any_of(deviceExts.begin(), deviceExts.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
    });
    if (!hasSwapchain) fail("VK_KHR_swapchain-missing");

    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProps(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queueProps.data());
    uint32_t queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueCount; ++i) {
        VkBool32 presentSupport = VK_FALSE;
        must(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &presentSupport), "surface-support-query");
        if ((queueProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupport) {
            queueFamily = i;
            break;
        }
    }
    if (queueFamily == UINT32_MAX) fail("graphics-present-queue-not-found");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    VkDevice device = VK_NULL_HANDLE;
    must(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkSurfaceCapabilitiesKHR caps{};
    must(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface-capabilities");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) fail("surface-does-not-support-transfer-dst");
    uint32_t formatCount = 0;
    must(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr), "surface-format-count");
    if (!formatCount) fail("surface-has-no-formats");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    must(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data()), "surface-format-list");
    VkSurfaceFormatKHR selected = formats[0];
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(canvasWidth, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(canvasHeight, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    uint32_t imageCount = std::max(4u, caps.minImageCount);
    if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;
    logLine("surface extent=%ux%u format=%d transfer_dst=yes images=%u..%u selected=%u", extent.width, extent.height,
            static_cast<int>(selected.format), caps.minImageCount, caps.maxImageCount, imageCount);

    VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapInfo.surface = surface;
    swapInfo.minImageCount = imageCount;
    swapInfo.imageFormat = selected.format;
    swapInfo.imageColorSpace = selected.colorSpace;
    swapInfo.imageExtent = extent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is Vulkan's mandatory vsync mode. Keep it enabled and also cap the
    // CPU-side present loop so a WSI implementation that returns early cannot
    // spin the UI at hundreds of updates per second.
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    must(vkCreateSwapchainKHR(device, &swapInfo, nullptr, &swapchain), "vkCreateSwapchainKHR");

    uint32_t swapImageCount = 0;
    must(vkGetSwapchainImagesKHR(device, swapchain, &swapImageCount, nullptr), "swapchain-image-count");
    std::vector<VkImage> images(swapImageCount);
    must(vkGetSwapchainImagesKHR(device, swapchain, &swapImageCount, images.data()), "swapchain-image-list");
    if (!swapImageCount || swapImageCount > 8) fail("unsupported-swapchain-image-count");
    std::vector<VkImageLayout> layouts(swapImageCount, VK_IMAGE_LAYOUT_UNDEFINED);
    logLine("swapchain ready image_count=%u present_mode=FIFO vsync=on software_cap_fps=60",
            swapImageCount);

    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(extent.width) * extent.height * sizeof(uint32_t);
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = byteCount;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer upload = VK_NULL_HANDLE;
    must(vkCreateBuffer(device, &bufferInfo, nullptr, &upload), "vkCreateBuffer(upload)");
    VkMemoryRequirements memoryReq{};
    vkGetBufferMemoryRequirements(device, upload, &memoryReq);
    VkMemoryPropertyFlags memoryFlags = 0;
    const uint32_t memoryType = findMemoryType(physical, memoryReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                                memoryFlags);
    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = memoryReq.size;
    allocateInfo.memoryTypeIndex = memoryType;
    VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
    must(vkAllocateMemory(device, &allocateInfo, nullptr, &uploadMemory), "vkAllocateMemory(upload)");
    must(vkBindBufferMemory(device, upload, uploadMemory, 0), "vkBindBufferMemory(upload)");
    void* mapped = nullptr;
    must(vkMapMemory(device, uploadMemory, 0, byteCount, 0, &mapped), "vkMapMemory(upload)");
    makeCanvasPixels(rgba, canvasWidth, canvasHeight, extent.width, extent.height, static_cast<uint32_t*>(mapped));
    if (!(memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = uploadMemory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        must(vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges(upload)");
    }
    logLine("upload buffer ready bytes=%llu memory_type=%u coherent=%u cached=%u", (unsigned long long)byteCount,
            memoryType, !!(memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
            !!(memoryFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT));

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    must(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    must(vkAllocateCommandBuffers(device, &commandInfo, &command), "vkAllocateCommandBuffers");
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    must(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired), "vkCreateSemaphore(acquired)");

    using Clock = std::chrono::steady_clock;
    auto nextFrameDeadline = Clock::now();
    auto elapsedUs = [](Clock::time_point start) -> uint64_t {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    };
    auto maxInto = [](uint64_t value, uint64_t& target) { target = std::max(target, value); };
    uint64_t frame = 0;
    PresentationStats presentationStats{};
    auto statsWindowStart = Clock::now();
    uint64_t windowFrames = 0, windowNewVideoFrames = 0;
    uint64_t updateSumUs = 0, updateMaxUs = 0;
    uint64_t uploadSumUs = 0, uploadMaxUs = 0;
    uint64_t acquireSumUs = 0, acquireMaxUs = 0;
    uint64_t encodeSubmitSumUs = 0, encodeSubmitMaxUs = 0;
    uint64_t presentCallSumUs = 0, presentCallMaxUs = 0;
    uint64_t frameIntervalSumUs = 0, frameIntervalMaxUs = 0;
    uint64_t queueIdleSumUs = 0, queueIdleMaxUs = 0;
    uint64_t loopSumUs = 0, loopMaxUs = 0;
    uint64_t paceWaitSumUs = 0, paceWaitMaxUs = 0;
    auto lastFrameStart = Clock::time_point{};
    for (;;) {
        const auto paceStart = Clock::now();
        const auto beforeDeadline = Clock::now();
        if (beforeDeadline < nextFrameDeadline) {
            const auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(
                nextFrameDeadline - beforeDeadline).count();
            if (remainingUs > 0) sceKernelUsleep(static_cast<uint32_t>(remainingUs));
        }
        const auto loopStart = Clock::now();
        if (lastFrameStart.time_since_epoch().count() != 0) {
            const uint64_t frameIntervalUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                loopStart - lastFrameStart).count());
            frameIntervalSumUs += frameIntervalUs;
            maxInto(frameIntervalUs, frameIntervalMaxUs);
        }
        lastFrameStart = loopStart;
        const uint64_t paceWaitUs = elapsedUs(paceStart);
        paceWaitSumUs += paceWaitUs;
        maxInto(paceWaitUs, paceWaitMaxUs);
        // Pace by frame start. If rendering already took longer than 16.67 ms,
        // do not add another wait; the next iteration naturally catches up.
        const auto updateStart = Clock::now();
        CanvasFrameUpdateResult updateResult{};
        const uint64_t updateUs = elapsedUs(updateStart);
        updateSumUs += updateUs;
        maxInto(updateUs, updateMaxUs);
        if (updateResult.newVideoFrame) ++windowNewVideoFrames;
        uint64_t uploadUs = 0;
            const auto uploadStart = Clock::now();
            if (!(memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = uploadMemory;
                range.offset = 0;
                range.size = VK_WHOLE_SIZE;
                must(vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges(updated-upload)");
            }
            uploadUs = elapsedUs(uploadStart);
            uploadSumUs += uploadUs;
            maxInto(uploadUs, uploadMaxUs);
        }
        uint32_t imageIndex = 0;
        const auto acquireStart = Clock::now();
        const VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &imageIndex);
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) fail("vkAcquireNextImageKHR", acquire);
        const uint64_t acquireUs = elapsedUs(acquireStart);
        acquireSumUs += acquireUs;
        maxInto(acquireUs, acquireMaxUs);
        const auto encodeSubmitStart = Clock::now();
        must(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        must(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");

        must(vkEndCommandBuffer(command), "vkEndCommandBuffer");

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        const uint64_t encodeSubmitUs = elapsedUs(encodeSubmitStart);
        encodeSubmitSumUs += encodeSubmitUs;
        maxInto(encodeSubmitUs, encodeSubmitMaxUs);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        const auto presentCallStart = Clock::now();
        const VkResult presented = vkQueuePresentKHR(queue, &present);
        if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) fail("vkQueuePresentKHR", presented);
        const uint64_t presentCallUs = elapsedUs(presentCallStart);
        presentCallSumUs += presentCallUs;
        maxInto(presentCallUs, presentCallMaxUs);
        layouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        const uint64_t loopUs = elapsedUs(loopStart);
        loopSumUs += loopUs;
        maxInto(loopUs, loopMaxUs);
        ++windowFrames;
        ++frame;

        const uint32_t elapsedMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - statsWindowStart).count());
        if (elapsedMs >= 2000 && windowFrames) {
            presentationStats.windowMs = elapsedMs;
            presentationStats.presentFps = static_cast<uint32_t>(windowFrames * 1000u / elapsedMs);
            presentationStats.newVideoFps = static_cast<uint32_t>(windowNewVideoFrames * 1000u / elapsedMs);
            presentationStats.updateAvgUs = static_cast<uint32_t>(updateSumUs / windowFrames);
            presentationStats.updateMaxUs = static_cast<uint32_t>(updateMaxUs);
            presentationStats.uploadAvgUs = static_cast<uint32_t>(uploadSumUs / windowFrames);
            presentationStats.uploadMaxUs = static_cast<uint32_t>(uploadMaxUs);
            presentationStats.acquireAvgUs = static_cast<uint32_t>(acquireSumUs / windowFrames);
            presentationStats.acquireMaxUs = static_cast<uint32_t>(acquireMaxUs);
            presentationStats.encodeSubmitAvgUs = static_cast<uint32_t>(encodeSubmitSumUs / windowFrames);
            presentationStats.encodeSubmitMaxUs = static_cast<uint32_t>(encodeSubmitMaxUs);
            presentationStats.presentCallAvgUs = static_cast<uint32_t>(presentCallSumUs / windowFrames);
            presentationStats.presentCallMaxUs = static_cast<uint32_t>(presentCallMaxUs);
            presentationStats.frameIntervalAvgUs = static_cast<uint32_t>(frameIntervalSumUs / windowFrames);
            presentationStats.frameIntervalMaxUs = static_cast<uint32_t>(frameIntervalMaxUs);
            presentationStats.queueIdleAvgUs = static_cast<uint32_t>(queueIdleSumUs / windowFrames);
            presentationStats.queueIdleMaxUs = static_cast<uint32_t>(queueIdleMaxUs);
            presentationStats.loopAvgUs = static_cast<uint32_t>(loopSumUs / windowFrames);
            presentationStats.loopMaxUs = static_cast<uint32_t>(loopMaxUs);
            ++presentationStats.generation;
                    elapsedMs, static_cast<unsigned long long>(windowFrames), presentationStats.presentFps,
                    presentationStats.frameIntervalAvgUs / 1000.0, presentationStats.frameIntervalMaxUs / 1000.0);
                    presentationStats.updateAvgUs / 1000.0,
                    presentationStats.updateMaxUs / 1000.0, presentationStats.uploadAvgUs / 1000.0,
                    presentationStats.uploadMaxUs / 1000.0, presentationStats.acquireAvgUs / 1000.0,
                    presentationStats.acquireMaxUs / 1000.0, presentationStats.encodeSubmitAvgUs / 1000.0,
                    presentationStats.encodeSubmitMaxUs / 1000.0, presentationStats.presentCallAvgUs / 1000.0,
                    presentationStats.presentCallMaxUs / 1000.0, presentationStats.queueIdleAvgUs / 1000.0,
                    presentationStats.queueIdleMaxUs / 1000.0, paceWaitSumUs / windowFrames / 1000.0,
                    paceWaitMaxUs / 1000.0, presentationStats.loopAvgUs / 1000.0,
                    presentationStats.loopMaxUs / 1000.0);
            windowFrames = windowNewVideoFrames = 0;
            updateSumUs = updateMaxUs = uploadSumUs = uploadMaxUs = 0;
            acquireSumUs = acquireMaxUs = encodeSubmitSumUs = encodeSubmitMaxUs = 0;
            presentCallSumUs = presentCallMaxUs = queueIdleSumUs = queueIdleMaxUs = 0;
            frameIntervalSumUs = frameIntervalMaxUs = 0;
            paceWaitSumUs = paceWaitMaxUs = 0;
            loopSumUs = loopMaxUs = 0;
        }
        if (frame == 1) logLine("presentation loop started; present_loop_fps counts successful vkQueuePresentKHR calls, not direct TV scanout; rate and cost aggregates every ~2 s");
    }
}

} // namespace xc::ps4
