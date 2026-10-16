// SPDX-License-Identifier: GPL-3.0-or-later
// Small Vulkan upload/present path for the PS4 homebrew bootstrap.
#include "gpu_wsi.h"
#include "native_thread_cpu.h"
#include "gpu_video_shaders.h"
#include "util/log.h"
#include "ui/canvas.h"
#include <orbis/VideoOut.h>
#include <atomic>
#include <array>
#include <mutex>

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

// Observe the handle used by Mesa without opening a second VideoOut port or
// changing the driver's buffer indices, flip arguments, or scheduling.
namespace {
std::atomic<int32_t> gVideoOutHandle{-1};
struct FlipVideoTag { int32_t handle=-1; int64_t arg=0; uint64_t sequence=0; bool active=false, known=false; };
std::array<FlipVideoTag,128> gFlipVideoTags{};
size_t gFlipTagNext=0;
std::mutex gFlipTagMutex;
// Only tag synchronous native submissions made within this present call.
// If a backend submits on another thread, report unknown rather than guess.
thread_local FlipVideoTag gPresentTag{};
}
extern "C" int32_t __real_sceVideoOutSubmitFlip(int32_t, int32_t, uint32_t, int64_t);
extern "C" int32_t __wrap_sceVideoOutSubmitFlip(int32_t handle, int32_t buffer,
                                               uint32_t mode, int64_t argument) {
    const int32_t result = __real_sceVideoOutSubmitFlip(handle, buffer, mode, argument);
    if (result >= 0) {
        gVideoOutHandle.store(handle, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(gFlipTagMutex);
        auto tag=gPresentTag; tag.handle=handle; tag.arg=argument;
        gFlipVideoTags[gFlipTagNext++ % gFlipVideoTags.size()]=tag;
    }
    return result;
}

namespace xc::ps4 {
namespace {
std::atomic<uint32_t> gPresentationStage{0};
std::atomic<uint64_t> gPresentationAtMs{0};
void markPresentation(uint32_t stage) {
    gPresentationAtMs.store(sceKernelGetProcessTime()/1000u);
    gPresentationStage.store(stage);
}

void logLine(const char* fmt, ...) {
    char body[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[900];
    std::snprintf(line, sizeof(line), "[%s] %s\n", kBuild, body);
    XC_LOGI("%s", line);
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

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct GpuVideoRenderer {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat outputFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImage yImage = VK_NULL_HANDLE;
    VkDeviceMemory yMemory = VK_NULL_HANDLE;
    VkImageView yView = VK_NULL_HANDLE;
    VkImage uvImage = VK_NULL_HANDLE;
    VkDeviceMemory uvMemory = VK_NULL_HANDLE;
    VkImageView uvView = VK_NULL_HANDLE;
    VkImage overlayImage = VK_NULL_HANDLE;
    VkDeviceMemory overlayMemory = VK_NULL_HANDLE;
    VkImageView overlayView = VK_NULL_HANDLE;
    VkImage renderImage = VK_NULL_HANDLE;
    VkDeviceMemory renderMemory = VK_NULL_HANDLE;
    VkImageView renderView = VK_NULL_HANDLE;
    VkDeviceMemory videoUploadMemory = VK_NULL_HANDLE;
    VkBuffer videoUpload = VK_NULL_HANDLE;
    uint8_t* videoMapped = nullptr;
    VkDeviceSize videoCapacity = 0;
    VkDeviceSize videoUvOffset = 0;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkImage scaledImage = VK_NULL_HANDLE;
    VkDeviceMemory scaledMemory = VK_NULL_HANDLE;
    VkImageView scaledView = VK_NULL_HANDLE;
    VkFramebuffer scaledFramebuffer = VK_NULL_HANDLE;
    VkImageLayout scaledLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDescriptorSet rcasSet = VK_NULL_HANDLE;
    VkPipeline rcasPipeline = VK_NULL_HANDLE;
    VkPipeline uiPipeline = VK_NULL_HANDLE;
    VkPipelineLayout uiLayout = VK_NULL_HANDLE;
    VkImageLayout yLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout uvLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout overlayLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout renderLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool initialized = false;
    VkMemoryPropertyFlags videoMemoryFlags = 0;
};

bool hasFormatFeatures(VkPhysicalDevice physical, VkFormat format, VkFormatFeatureFlags required) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &props);
    return (props.optimalTilingFeatures & required) == required;
}

bool createGpuImage(VkPhysicalDevice physical, VkDevice device, VkFormat format,
                    VkExtent2D extent, VkImageUsageFlags usage,
                    VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if (requirements.memoryTypeBits & (1u << i)) { memoryType = i; break; }
        }
    }
    if (memoryType == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
        vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) return false;
    return true;
}

bool initGpuVideoRenderer(VkPhysicalDevice physical, VkDevice device, VkFormat outputFormat,
                          VkExtent2D extent, GpuVideoRenderer& gpu) {
    const VkFormatFeatureFlags sampledTransfer = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                  VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const VkFormatFeatureFlags colorTransfer = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if (!hasFormatFeatures(physical, VK_FORMAT_R8_UNORM, sampledTransfer) ||
        !hasFormatFeatures(physical, VK_FORMAT_R8G8_UNORM, sampledTransfer) ||
        !hasFormatFeatures(physical, VK_FORMAT_R8G8B8A8_UNORM, sampledTransfer) ||
        !hasFormatFeatures(physical, outputFormat, colorTransfer | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    gpu.device = device;
    gpu.outputFormat = outputFormat;
    gpu.extent = extent;
    constexpr uint32_t maxWidth = 1920, maxHeight = 1088;
    if (!createGpuImage(physical, device, VK_FORMAT_R8_UNORM, {maxWidth, maxHeight},
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        gpu.yImage, gpu.yMemory, gpu.yView) ||
        !createGpuImage(physical, device, VK_FORMAT_R8G8_UNORM, {(maxWidth + 1) / 2, (maxHeight + 1) / 2},
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        gpu.uvImage, gpu.uvMemory, gpu.uvView) ||
        !createGpuImage(physical, device, VK_FORMAT_R8G8B8A8_UNORM, extent,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        gpu.overlayImage, gpu.overlayMemory, gpu.overlayView) ||
        !createGpuImage(physical, device, outputFormat, extent,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        gpu.renderImage, gpu.renderMemory, gpu.renderView) ||
        !createGpuImage(physical, device, outputFormat, extent,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        gpu.scaledImage, gpu.scaledMemory, gpu.scaledView)) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }

    gpu.videoCapacity = static_cast<VkDeviceSize>(maxWidth) * maxHeight * 3 / 2;
    gpu.videoUvOffset = static_cast<VkDeviceSize>(maxWidth) * maxHeight;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = gpu.videoCapacity;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &gpu.videoUpload) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    VkMemoryRequirements bufferReq{};
    vkGetBufferMemoryRequirements(device, gpu.videoUpload, &bufferReq);
    VkPhysicalDeviceMemoryProperties memoryProps{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProps);
    uint32_t hostType = UINT32_MAX;
    for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i) {
        const auto flags = memoryProps.memoryTypes[i].propertyFlags;
        if ((bufferReq.memoryTypeBits & (1u << i)) &&
            (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            hostType = i; gpu.videoMemoryFlags = flags; break;
        }
    }
    if (hostType == UINT32_MAX) {
        for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i) {
            const auto flags = memoryProps.memoryTypes[i].propertyFlags;
            if ((bufferReq.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                hostType = i; gpu.videoMemoryFlags = flags; break;
            }
        }
    }
    if (hostType == UINT32_MAX) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    VkMemoryAllocateInfo videoAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    videoAlloc.allocationSize = bufferReq.size;
    videoAlloc.memoryTypeIndex = hostType;
    if (vkAllocateMemory(device, &videoAlloc, nullptr, &gpu.videoUploadMemory) != VK_SUCCESS ||
        vkBindBufferMemory(device, gpu.videoUpload, gpu.videoUploadMemory, 0) != VK_SUCCESS ||
        vkMapMemory(device, gpu.videoUploadMemory, 0, gpu.videoCapacity, 0,
                    reinterpret_cast<void**>(&gpu.videoMapped)) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &gpu.sampler) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorLayoutInfo.bindingCount = 3;
    descriptorLayoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &descriptorLayoutInfo, nullptr, &gpu.descriptorLayout) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &gpu.descriptorPool) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = gpu.descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &gpu.descriptorLayout;
    if (vkAllocateDescriptorSets(device, &setInfo, &gpu.descriptorSet) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    VkDescriptorImageInfo imageInfos[3]{{gpu.sampler, gpu.yView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {gpu.sampler, gpu.uvView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {gpu.sampler, gpu.overlayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = gpu.descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    if (vkAllocateDescriptorSets(device, &setInfo, &gpu.rcasSet) != VK_SUCCESS) return false;
    VkDescriptorImageInfo rcasImages[3] = {
        {gpu.sampler, gpu.scaledView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {gpu.sampler, gpu.uvView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {gpu.sampler, gpu.overlayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    for (uint32_t i=0; i<3; ++i) {
        writes[i].dstSet = gpu.rcasSet;
        writes[i].pImageInfo = &rcasImages[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);


    VkAttachmentDescription attachment{};
    attachment.format = outputFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo renderInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderInfo.attachmentCount = 1; renderInfo.pAttachments = &attachment;
    renderInfo.subpassCount = 1; renderInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(device, &renderInfo, nullptr, &gpu.renderPass) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferInfo.renderPass = gpu.renderPass;
    framebufferInfo.attachmentCount = 1; framebufferInfo.pAttachments = &gpu.renderView;
    framebufferInfo.width = extent.width; framebufferInfo.height = extent.height; framebufferInfo.layers = 1;
    if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &gpu.framebuffer) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }

    framebufferInfo.pAttachments = &gpu.scaledView;
    if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &gpu.scaledFramebuffer) != VK_SUCCESS) return false;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = 48;
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1; pipelineLayoutInfo.pSetLayouts = &gpu.descriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1; pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &gpu.pipelineLayout) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = sizeof(kNv12VideoVertSpv); shaderInfo.pCode = kNv12VideoVertSpv;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &shaderInfo, nullptr, &vert) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    shaderInfo.codeSize = sizeof(kNv12VideoFragSpv); shaderInfo.pCode = kNv12VideoFragSpv;
    if (vkCreateShaderModule(device, &shaderInfo, nullptr, &frag) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1; viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1; blend.pAttachments = &blendAttachment;
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2; pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = gpu.pipelineLayout;
    pipelineInfo.renderPass = gpu.renderPass;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu.pipeline) != VK_SUCCESS) { logLine("gpu_video_init failed source_line=%d cpu_fallback=1", __LINE__); return false; }
    VkShaderModule rcasFrag = VK_NULL_HANDLE;
    shaderInfo.codeSize = sizeof(kRcasFragSpv); shaderInfo.pCode = kRcasFragSpv;
    if (vkCreateShaderModule(device, &shaderInfo, nullptr, &rcasFrag) != VK_SUCCESS) return false;
    stages[1].module = rcasFrag;
    const VkResult rcasResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu.rcasPipeline);
    vkDestroyShaderModule(device, rcasFrag, nullptr);
    if (rcasResult != VK_SUCCESS) return false;
    // HUD geometry and text blending use GPU quads. Font masks are uploaded
    // only when the atlas grows, rather than rasterizing a 1080p overlay.
    VkPushConstantRange uiRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    pipelineLayoutInfo.pPushConstantRanges = &uiRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &gpu.uiLayout) != VK_SUCCESS) return false;
    VkShaderModule uiVert = VK_NULL_HANDLE, uiFrag = VK_NULL_HANDLE;
    shaderInfo.codeSize = sizeof(kUiVertSpv); shaderInfo.pCode = kUiVertSpv;
    if (vkCreateShaderModule(device, &shaderInfo, nullptr, &uiVert) != VK_SUCCESS) return false;
    shaderInfo.codeSize = sizeof(kUiFragSpv); shaderInfo.pCode = kUiFragSpv;
    if (vkCreateShaderModule(device, &shaderInfo, nullptr, &uiFrag) != VK_SUCCESS) return false;
    stages[0].module = uiVert; stages[1].module = uiFrag;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    pipelineInfo.layout = gpu.uiLayout;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu.uiPipeline) != VK_SUCCESS) return false;
    vkDestroyShaderModule(device, uiFrag, nullptr);
    vkDestroyShaderModule(device, uiVert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    vkDestroyShaderModule(device, vert, nullptr);
    gpu.initialized = true;
    return true;
}

void transitionImage(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout,
                     VkImageLayout newLayout, VkPipelineStageFlags sourceStage,
                     VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess,
                     VkAccessFlags destinationAccess) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}
} // namespace

PresentationHeartbeat presentationHeartbeat() {
    const uint64_t now = sceKernelGetProcessTime()/1000u;
    const uint64_t at = gPresentationAtMs.load();
    return {gPresentationStage.load(), at && now >= at ? now-at : 0};
}

[[noreturn]] void presentCanvas(uint32_t* rgba, uint32_t canvasWidth, uint32_t canvasHeight,
                                CanvasFrameUpdate update, Nv12FrameUpload uploadVideo,
                                void* userData) {
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
    for (const auto& format : formats)
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM) { selected = format; break; }
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

    GpuVideoRenderer gpuVideo{};
    const bool gpuVideoEnabled = initGpuVideoRenderer(physical, device, selected.format, extent, gpuVideo);
    logLine("gpu_video=%s path=NV12-sample+YUV-convert+scale%s",
            gpuVideoEnabled ? "enabled" : "disabled",
            gpuVideoEnabled ? "" : " (using CPU fallback)");

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
    VkSemaphore acquired = VK_NULL_HANDLE;
    must(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired), "vkCreateSemaphore(acquired)");
    std::vector<VkSemaphore> rendered(swapImageCount, VK_NULL_HANDLE);
    for (auto& semaphore : rendered)
        must(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore(rendered)");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence frameFence = VK_NULL_HANDLE;
    must(vkCreateFence(device, &fenceInfo, nullptr, &frameFence), "vkCreateFence(frame)");
    bool overlayNeedsInitialUpload = gpuVideoEnabled;
    bool gpuOverlayInitialized = false;
    bool gpuYuvInitialized = false;
    bool gpuVideoHasFrame = false;
    uint32_t lastVideoWidth = 0, lastVideoHeight = 0;

    using Clock = std::chrono::steady_clock;
    uint32_t targetFps = 60;
    auto nextFrameDeadline = Clock::now();
    auto elapsedUs = [](Clock::time_point start) -> uint64_t {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    };
    auto maxInto = [](uint64_t value, uint64_t& target) { target = std::max(target, value); };
    NativeThreadCpuSampler<logLine> renderCpuSampler("vulkan_renderer");
    renderCpuSampler.start();
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
    uint64_t submittedVideoSequence = 0, watchedFlipNum = 0, watchedSequence = 0;
    uint64_t confirmedUniqueVideoFrames = 0, confirmedRepeatedVideoFlips = 0;
    uint64_t previousWindowUniqueFrames = 0;
    int32_t watchedHandle = -1;
    bool wasGpuUi = false;
    uint64_t atlasGeneration = 0;
    bool flipBaselineValid = false;
    int32_t flipBaselineHandle = -1;
    uint64_t previousFlipCount = 0;
    auto flipBaselineTime = Clock::now();
    for (;;) {
        renderCpuSampler.sample();
        const auto fenceStart = Clock::now();
        markPresentation(1); // frame fence
        must(vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX), "vkWaitForFences(frame)");
        const uint64_t fenceUs = elapsedUs(fenceStart);
        queueIdleSumUs += fenceUs;
        maxInto(fenceUs, queueIdleMaxUs);
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
        const auto frameInterval = std::chrono::microseconds(targetFps == 30 ? 33333 : 16667);
        // Keep an absolute cadence: scheduler oversleep must not accumulate
        // another delay on every frame. Skip missed slots rather than burst.
        if (loopStart > nextFrameDeadline + frameInterval)
            nextFrameDeadline = loopStart + frameInterval;
        else
            nextFrameDeadline += frameInterval;
        const auto updateStart = Clock::now();
        CanvasFrameUpdateResult updateResult{};
        markPresentation(2); // UI callback
        if (update) updateResult = update(rgba, userData, presentationStats, gpuVideoEnabled);
        const uint32_t requestedFps = updateResult.targetFps == 30 ? 30 : 60;
        if (requestedFps != targetFps) {
            targetFps = requestedFps;
            nextFrameDeadline = loopStart + std::chrono::microseconds(targetFps == 30 ? 33333 : 16667);
        }
        const auto* uiCanvas = updateResult.gpuUi;
        const bool useGpuUi = gpuVideoEnabled && uiCanvas && uiCanvas->gpuUiEnabled();
        const bool atlasChanged = useGpuUi && (uiCanvas->gpuAtlasGeneration() != atlasGeneration || !wasGpuUi);
        const bool overlayChanged = useGpuUi ? atlasChanged : (updateResult.canvasChanged || wasGpuUi);
        wasGpuUi = useGpuUi;
        const uint64_t updateUs = elapsedUs(updateStart);
        updateSumUs += updateUs;
        maxInto(updateUs, updateMaxUs);
        uint32_t videoWidth = 0, videoHeight = 0;
        bool uploadedVideoFrame = false;
        if (gpuVideoEnabled && updateResult.videoActive && uploadVideo) {
            markPresentation(3); // NV12 upload copy
            uploadedVideoFrame = uploadVideo(userData, gpuVideo.videoMapped,
                                              static_cast<size_t>(gpuVideo.videoCapacity),
            if (uploadedVideoFrame) {
                if (!videoWidth || !videoHeight || videoWidth > 1920 || videoHeight > 1088 ||
                    (videoWidth & 1u) || (videoHeight & 1u))
                    fail("invalid-gpu-video-frame-dimensions");
                const VkDeviceSize yBytes = static_cast<VkDeviceSize>(videoWidth) * videoHeight;
                const VkDeviceSize uvBytes = static_cast<VkDeviceSize>(videoWidth) * (videoHeight / 2u);
                const VkDeviceSize alignedUvOffset = (yBytes + 3u) & ~VkDeviceSize(3u);
                if (alignedUvOffset + uvBytes > gpuVideo.videoCapacity) fail("gpu-video-upload-overflow");
                if (alignedUvOffset != yBytes)
                    std::memmove(gpuVideo.videoMapped + alignedUvOffset,
                                 gpuVideo.videoMapped + yBytes, static_cast<size_t>(uvBytes));
                gpuVideo.videoUvOffset = alignedUvOffset;
                lastVideoWidth = videoWidth;
                lastVideoHeight = videoHeight;
                gpuVideoHasFrame = true;
                if (!(gpuVideo.videoMemoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                    range.memory = gpuVideo.videoUploadMemory;
                    range.offset = 0;
                    range.size = VK_WHOLE_SIZE;
                    must(vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges(video-upload)");
                }
            }
        }
        if (!updateResult.videoActive) gpuVideoHasFrame = false;
        updateResult.newVideoFrame = uploadedVideoFrame || (!gpuVideoEnabled && updateResult.newVideoFrame);
        if (updateResult.newVideoFrame) ++windowNewVideoFrames;
        uint64_t uploadUs = 0;
        if (overlayChanged || overlayNeedsInitialUpload) {
            const auto uploadStart = Clock::now();
            const uint32_t* overlay = useGpuUi ? uiCanvas->gpuAtlas() : rgba;
            if (overlay) makeCanvasPixels(overlay, canvasWidth, canvasHeight, extent.width, extent.height,
                                          static_cast<uint32_t*>(mapped));
            else std::memset(mapped, 0, static_cast<size_t>(byteCount));
            if (useGpuUi) atlasGeneration = uiCanvas->gpuAtlasGeneration();
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
            overlayNeedsInitialUpload = false;
        }
        uint32_t imageIndex = 0;
        const auto acquireStart = Clock::now();
        markPresentation(4); // swapchain acquire
        const VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &imageIndex);
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) fail("vkAcquireNextImageKHR", acquire);
        const uint64_t acquireUs = elapsedUs(acquireStart);
        acquireSumUs += acquireUs;
        maxInto(acquireUs, acquireMaxUs);
        const auto encodeSubmitStart = Clock::now();
        must(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        must(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");

        if (gpuVideoEnabled) {
            if (!gpuYuvInitialized && !uploadedVideoFrame) {
                transitionImage(command, gpuVideo.yImage, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                transitionImage(command, gpuVideo.uvImage, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkClearColorValue black{};
                VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(command, gpuVideo.yImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &black, 1, &colorRange);
                vkCmdClearColorImage(command, gpuVideo.uvImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &black, 1, &colorRange);
                transitionImage(command, gpuVideo.yImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT);
                transitionImage(command, gpuVideo.uvImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT);
                gpuVideo.yLayout = gpuVideo.uvLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                gpuYuvInitialized = true;
            }
            if (uploadedVideoFrame) {
                const auto oldY = gpuVideo.yLayout;
                const auto oldUv = gpuVideo.uvLayout;
                const VkPipelineStageFlags oldStage = gpuYuvInitialized
                    ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                const VkAccessFlags oldAccess = gpuYuvInitialized ? VK_ACCESS_SHADER_READ_BIT : 0;
                transitionImage(command, gpuVideo.yImage, oldY, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                oldStage, VK_PIPELINE_STAGE_TRANSFER_BIT, oldAccess, VK_ACCESS_TRANSFER_WRITE_BIT);
                transitionImage(command, gpuVideo.uvImage, oldUv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                oldStage, VK_PIPELINE_STAGE_TRANSFER_BIT, oldAccess, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkBufferImageCopy yCopy{};
                yCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                yCopy.imageExtent = {videoWidth, videoHeight, 1};
                vkCmdCopyBufferToImage(command, gpuVideo.videoUpload, gpuVideo.yImage,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &yCopy);
                VkBufferImageCopy uvCopy{};
                uvCopy.bufferOffset = gpuVideo.videoUvOffset;
                uvCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                uvCopy.imageExtent = {videoWidth / 2u, videoHeight / 2u, 1};
                vkCmdCopyBufferToImage(command, gpuVideo.videoUpload, gpuVideo.uvImage,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uvCopy);
                transitionImage(command, gpuVideo.yImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT);
                transitionImage(command, gpuVideo.uvImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT);
                gpuVideo.yLayout = gpuVideo.uvLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                gpuYuvInitialized = true;
            }
            if (overlayChanged || !gpuOverlayInitialized) {
                const auto oldLayout = gpuVideo.overlayLayout;
                const VkPipelineStageFlags oldStage = gpuOverlayInitialized
                    ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                const VkAccessFlags oldAccess = gpuOverlayInitialized ? VK_ACCESS_SHADER_READ_BIT : 0;
                transitionImage(command, gpuVideo.overlayImage, oldLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, oldStage,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, oldAccess, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkBufferImageCopy overlayCopy{};
                overlayCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                overlayCopy.imageExtent = {extent.width, extent.height, 1};
                vkCmdCopyBufferToImage(command, upload, gpuVideo.overlayImage,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &overlayCopy);
                transitionImage(command, gpuVideo.overlayImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT);
                gpuVideo.overlayLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                gpuOverlayInitialized = true;
            }

            const bool doRcas = gpuVideoHasFrame && updateResult.videoActive && updateResult.upscaler == 1 &&
            if (doRcas) {
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            }
            transitionImage(command, gpuVideo.renderImage, gpuVideo.renderLayout,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            gpuVideo.renderLayout == VK_IMAGE_LAYOUT_UNDEFINED
                                ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            gpuVideo.renderLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderPassBeginInfo renderBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            renderBegin.renderPass = gpuVideo.renderPass;
            vkCmdBeginRenderPass(command, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport viewport{};
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuVideo.pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuVideo.pipelineLayout,
                                    0, 1, &gpuVideo.descriptorSet, 0, nullptr);
            struct VideoPushConstants { float rect[4]; int32_t width, height, enabled, gpuUi; int32_t targetWidth, targetHeight, upscaler, padding; } params{};
            params.gpuUi = (useGpuUi || doRcas) ? 1 : 0;
            params.upscaler = updateResult.upscaler == 1 ? 1 : 0;
            params.width = static_cast<int32_t>(lastVideoWidth);
            params.height = static_cast<int32_t>(lastVideoHeight);
            params.enabled = gpuVideoHasFrame && updateResult.videoActive ? 1 : 0;
            if (params.enabled) {
                double rectWidth = 1.0, rectHeight = 1.0, x = 0.0, y = 0.0;
                    y = (1.0 - rectHeight) * 0.5;
                } else {
                }
                params.rect[0] = static_cast<float>(x);
                params.rect[1] = static_cast<float>(y);
                params.rect[2] = static_cast<float>(x + rectWidth);
                params.rect[3] = static_cast<float>(y + rectHeight);
            } else {
                params.rect[2] = params.rect[3] = 1.0f;
            }
            const bool easuActive = params.enabled && params.upscaler == 1 &&
            bool scalerChanged = false;
            if (scalerChanged) {
            }
            vkCmdPushConstants(command, gpuVideo.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(params), &params);
            vkCmdDraw(command, 3, 1, 0, 0);
            if (doRcas) {
                vkCmdEndRenderPass(command);
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
                renderBegin.framebuffer = gpuVideo.framebuffer;
                vkCmdBeginRenderPass(command, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuVideo.rcasPipeline);
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuVideo.pipelineLayout,
                params.gpuUi = useGpuUi ? 1 : 0;
                vkCmdPushConstants(command, gpuVideo.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
                vkCmdDraw(command, 3, 1, 0, 0);
            }
            if (useGpuUi) {
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuVideo.uiPipeline);
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuVideo.uiLayout,
                                        0, 1, &gpuVideo.descriptorSet, 0, nullptr);
                for (const auto& draw : uiCanvas->gpuCommands()) {
                    const int left = std::max(0, draw.clip.x), top = std::max(0, draw.clip.y);
                    const int right = std::min(static_cast<int>(extent.width), draw.clip.x + draw.clip.w);
                    const int bottom = std::min(static_cast<int>(extent.height), draw.clip.y + draw.clip.h);
                    if (right <= left || bottom <= top || draw.rect.w <= 0 || draw.rect.h <= 0) continue;
                    VkRect2D clip{{left, top}, {static_cast<uint32_t>(right-left), static_cast<uint32_t>(bottom-top)}};
                    vkCmdSetScissor(command, 0, 1, &clip);
                    struct UiPush { float rect[4], uv[4], color[4], shape[4]; } u{};
                    u.rect[0]=draw.rect.x; u.rect[1]=draw.rect.y; u.rect[2]=draw.rect.w; u.rect[3]=draw.rect.h;
                    u.uv[0]=(draw.atlas.x+0.0f)/canvasWidth; u.uv[1]=(draw.atlas.y+0.0f)/canvasHeight;
                    u.uv[2]=(draw.atlas.x+draw.atlas.w+0.0f)/canvasWidth;
                    u.uv[3]=(draw.atlas.y+draw.atlas.h+0.0f)/canvasHeight;
                    for (uint32_t i=0; i<4; ++i) u.color[i]=((draw.color>>(8*i))&255u)/255.0f;
                    u.shape[0]=draw.radius; u.shape[1]=draw.stroke; u.shape[2]=draw.mask ? 1.0f : 0.0f;
                    vkCmdPushConstants(command, gpuVideo.uiLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(u), &u);
                    vkCmdDraw(command, 6, 1, 0, 0);
                }
            }
            vkCmdEndRenderPass(command);
            gpuVideo.renderLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

            transitionImage(command, images[imageIndex], layouts[imageIndex],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            layouts[imageIndex] == VK_IMAGE_LAYOUT_UNDEFINED
                                ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy renderCopy{};
            renderCopy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            renderCopy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            renderCopy.extent = {extent.width, extent.height, 1};
            vkCmdCopyImage(command, gpuVideo.renderImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &renderCopy);
            transitionImage(command, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        } else {
            VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransfer.srcAccessMask = 0;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = layouts[imageIndex];
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = images[imageIndex];
            toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &toTransfer);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyBufferToImage(command, upload, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            transitionImage(command, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        }
        must(vkEndCommandBuffer(command), "vkEndCommandBuffer");

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        VkSemaphore renderedSemaphore = rendered[imageIndex];
        submit.pSignalSemaphores = &renderedSemaphore;
        must(vkResetFences(device, 1, &frameFence), "vkResetFences(frame)");
        markPresentation(5); // GPU submit
        must(vkQueueSubmit(queue, 1, &submit, frameFence), "vkQueueSubmit");
        const uint64_t encodeSubmitUs = elapsedUs(encodeSubmitStart);
        encodeSubmitSumUs += encodeSubmitUs;
        maxInto(encodeSubmitUs, encodeSubmitMaxUs);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderedSemaphore;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        const auto presentCallStart = Clock::now();
        if (updateResult.newVideoFrame) ++submittedVideoSequence;
        gPresentTag.sequence=submittedVideoSequence;
        gPresentTag.active=updateResult.videoActive && submittedVideoSequence!=0;
        gPresentTag.known=true;
        markPresentation(6); // Vulkan/VideoOut present
        const VkResult presented = vkQueuePresentKHR(queue, &present);
        gPresentTag.known=false;
        if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) fail("vkQueuePresentKHR", presented);
        const uint64_t presentCallUs = elapsedUs(presentCallStart);
        presentCallSumUs += presentCallUs;
        maxInto(presentCallUs, presentCallMaxUs);
        // Poll completed flips, not the count of Vulkan calls. Correlate the
        // driver's unmodified flipArg with the tagged video upload sequence.
        OrbisVideoOutFlipStatus completed{};
        const int32_t watchedVideoHandle=gVideoOutHandle.load(std::memory_order_relaxed);
        if (watchedVideoHandle>=0 && sceVideoOutGetFlipStatus(watchedVideoHandle,&completed)>=0 &&
            (watchedVideoHandle!=watchedHandle || completed.num!=watchedFlipNum)) {
            FlipVideoTag tag{};
            {
                std::lock_guard<std::mutex> lock(gFlipTagMutex);
                for (const auto& candidate:gFlipVideoTags)
                    if(candidate.handle==watchedVideoHandle && candidate.arg==completed.flipArg) { tag=candidate; break; }
            }
            const bool sameHandle=watchedVideoHandle==watchedHandle;
            watchedHandle=watchedVideoHandle; watchedFlipNum=completed.num;
            presentationStats.confirmedVideoKnown=tag.known && tag.active;
            presentationStats.confirmedVideoSequence=presentationStats.confirmedVideoKnown ? tag.sequence : 0;
            if (presentationStats.confirmedVideoKnown) {
                const bool repeated = sameHandle && watchedSequence == tag.sequence;
                presentationStats.repeatedVideoFlips = repeated ? presentationStats.repeatedVideoFlips + 1 : 1;
                if (repeated) ++confirmedRepeatedVideoFlips;
                else ++confirmedUniqueVideoFrames;
                presentationStats.confirmedUniqueVideoFrames = confirmedUniqueVideoFrames;
                presentationStats.confirmedRepeatedVideoFlips = confirmedRepeatedVideoFlips;
                watchedSequence=tag.sequence;
            } else {
                watchedSequence=0; presentationStats.repeatedVideoFlips=0;
            }
        }
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
            presentationStats.confirmedUniqueVideoFps = static_cast<uint32_t>(
                (confirmedUniqueVideoFrames - previousWindowUniqueFrames) * 1000u / elapsedMs);
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
            OrbisVideoOutFlipStatus flip{};
            const int32_t videoHandle = gVideoOutHandle.load(std::memory_order_relaxed);
            const int32_t flipResult = videoHandle >= 0
                ? sceVideoOutGetFlipStatus(videoHandle, &flip) : -1;
            const auto flipNow = Clock::now();
            presentationStats.scanoutValid = false;
            presentationStats.scanoutFps = 0;
            if (flipResult >= 0) {
                if (flipBaselineValid && flipBaselineHandle == videoHandle && flip.num >= previousFlipCount) {
                    const auto flipMs = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(
                        flipNow - flipBaselineTime).count());
                    presentationStats.scanoutFps = static_cast<uint32_t>((flip.num - previousFlipCount) * 1000u / flipMs);
                    presentationStats.scanoutValid = true;
                }
                previousFlipCount = flip.num;
                flipBaselineTime = flipNow;
                flipBaselineHandle = videoHandle;
                flipBaselineValid = true;
            } else {
                flipBaselineValid = false;
            }
            logLine("videoout_scanout valid=%u status_rc=0x%08x handle=%d confirmed_flip_fps=%u total_flips=%llu confirmed_unique_video_frames=%llu repeated_video_flips_total=%llu pending=%d gpu_pending=%d buffer=%d flip_arg=%lld source=sceVideoOutGetFlipStatus includes_repeated_video=1",
                    presentationStats.scanoutValid ? 1u : 0u, static_cast<unsigned>(flipResult), videoHandle,
                    presentationStats.scanoutFps, static_cast<unsigned long long>(flip.num),
                    static_cast<unsigned long long>(presentationStats.confirmedUniqueVideoFrames),
                    static_cast<unsigned long long>(presentationStats.confirmedRepeatedVideoFlips),
                    flip.numFlipPending, flip.numGpuFlipPending, flip.currentBuffer, static_cast<long long>(flip.flipArg));
            logLine("video_flip_identity known=%u sequence=%llu consecutive_same=%u confirmed_unique_video_frames=%llu repeated_video_flips_total=%llu source=VideoOut_flipArg identity_only=1",
                    presentationStats.confirmedVideoKnown ? 1u : 0u,
                    static_cast<unsigned long long>(presentationStats.confirmedVideoSequence), presentationStats.repeatedVideoFlips,
                    static_cast<unsigned long long>(presentationStats.confirmedUniqueVideoFrames),
                    static_cast<unsigned long long>(presentationStats.confirmedRepeatedVideoFlips));
            ++presentationStats.generation;
            if (useGpuUi) logLine("gpu_ui quads=%zu atlas_generation=%llu atlas_overflow=%u target_fps=%u raster=GPU",
                    uiCanvas->gpuCommands().size(), static_cast<unsigned long long>(atlasGeneration),
                    uiCanvas->gpuAtlasOverflow() ? 1u : 0u, targetFps);
            logLine("present_rate window_ms=%u frames=%llu present_loop_fps=%u uploaded_video_fps=%u confirmed_unique_video_fps=%u frame_interval_ms=%.3f/%.3f",
                    elapsedMs, static_cast<unsigned long long>(windowFrames), presentationStats.presentFps,
                    presentationStats.newVideoFps, presentationStats.confirmedUniqueVideoFps,
                    presentationStats.frameIntervalAvgUs / 1000.0, presentationStats.frameIntervalMaxUs / 1000.0);
            logLine("present_cost callback_ms=%.3f/%.3f upload_ms=%.3f/%.3f acquire_ms=%.3f/%.3f encode_submit_ms=%.3f/%.3f present_call_ms=%.3f/%.3f gpu_fence_wait_ms=%.3f/%.3f pace_wait_ms=%.3f/%.3f loop_ms=%.3f/%.3f",
                    presentationStats.updateAvgUs / 1000.0,
                    presentationStats.updateMaxUs / 1000.0, presentationStats.uploadAvgUs / 1000.0,
                    presentationStats.uploadMaxUs / 1000.0, presentationStats.acquireAvgUs / 1000.0,
                    presentationStats.acquireMaxUs / 1000.0, presentationStats.encodeSubmitAvgUs / 1000.0,
                    presentationStats.encodeSubmitMaxUs / 1000.0, presentationStats.presentCallAvgUs / 1000.0,
                    presentationStats.presentCallMaxUs / 1000.0, presentationStats.queueIdleAvgUs / 1000.0,
                    presentationStats.queueIdleMaxUs / 1000.0, paceWaitSumUs / windowFrames / 1000.0,
                    paceWaitMaxUs / 1000.0, presentationStats.loopAvgUs / 1000.0,
                    presentationStats.loopMaxUs / 1000.0);
            // Keep all time between windows in the denominator, including
            // diagnostics. Otherwise present FPS hides logging stalls.
            statsWindowStart += std::chrono::milliseconds(elapsedMs);
            previousWindowUniqueFrames = confirmedUniqueVideoFrames;
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
