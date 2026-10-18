// SPDX-License-Identifier: MIT
// Experimental bounded synchronous readback. Automatic deployment is gated.
#include <string.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>
#include "render/vulkan.h"

bool vulkan_copy_buffer_init(struct wlr_vk_render_buffer *buffer,
        const struct wlr_dmabuf_attributes *dmabuf) {
    if (dmabuf->width <= 0 || dmabuf->height <= 0 ||
        (uint64_t)dmabuf->width * dmabuf->height > SIZE_MAX / 4) return false;
    if (dmabuf->format != DRM_FORMAT_XRGB8888 && dmabuf->format != DRM_FORMAT_ARGB8888) {
        return false;
    }
    struct wlr_vk_renderer *renderer = buffer->renderer;
    VkDevice dev = renderer->dev->dev;
    buffer->copy_target = true;
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {dmabuf->width, dmabuf->height, 1},
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = vkCreateImage(dev, &image, NULL, &buffer->image);
    if (res != VK_SUCCESS) goto error;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, buffer->image, &req);
    int type = vulkan_find_mem_type(renderer->dev, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.memoryTypeBits);
    if (type < 0) return false;
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = buffer->image,
    };
    VkMemoryAllocateInfo alloc = {
        .pNext = &dedicated,
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = type,
    };
    res = vkAllocateMemory(dev, &alloc, NULL, &buffer->memories[0]);
    if (res != VK_SUCCESS) goto error;
    buffer->mem_count = 1;
    res = vkBindImageMemory(dev, buffer->image, buffer->memories[0], 0);
    if (res != VK_SUCCESS) goto error;

    VkBufferCreateInfo staging = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)dmabuf->width * dmabuf->height * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    res = vkCreateBuffer(dev, &staging, NULL, &buffer->copy_staging);
    if (res != VK_SUCCESS) goto error;
    vkGetBufferMemoryRequirements(dev, buffer->copy_staging, &req);
    type = vulkan_find_mem_type(renderer->dev,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        req.memoryTypeBits);
    if (type < 0) type = vulkan_find_mem_type(renderer->dev,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, req.memoryTypeBits);
    if (type < 0) return false;
    dedicated.image = VK_NULL_HANDLE;
    dedicated.buffer = buffer->copy_staging;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    res = vkAllocateMemory(dev, &alloc, NULL, &buffer->copy_memory);
    if (res != VK_SUCCESS) goto error;
    res = vkBindBufferMemory(dev, buffer->copy_staging, buffer->copy_memory, 0);
    if (res != VK_SUCCESS) goto error;
    res = vkMapMemory(dev, buffer->copy_memory, 0, VK_WHOLE_SIZE, 0, &buffer->copy_mapping);
    if (res != VK_SUCCESS) goto error;
    wlr_log(WLR_INFO, "Vulkan presentation copy target allocated: %dx%d", dmabuf->width, dmabuf->height);
    return true;
error:
    wlr_vk_error("Vulkan presentation copy allocation", res);
    return false;
}

void vulkan_copy_buffer_record(struct wlr_vk_render_buffer *buffer, VkCommandBuffer cb) {
    VkImageMemoryBarrier image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = buffer->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image);
    VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {buffer->wlr_buffer->width, buffer->wlr_buffer->height, 1},
    };
    vkCmdCopyImageToBuffer(cb, buffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        buffer->copy_staging, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer->copy_staging, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0, NULL);
    image.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &image);
}

bool vulkan_copy_buffer_finish(struct wlr_vk_render_pass *pass) {
    struct wlr_vk_render_buffer *buffer = pass->render_buffer;
    const VkSemaphoreWaitInfoKHR wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR,
        .semaphoreCount = 1,
        .pSemaphores = &pass->renderer->timeline_semaphore,
        .pValues = &pass->command_buffer->timeline_point,
    };
    VkResult waited = pass->renderer->dev->api.vkWaitSemaphoresKHR(
        pass->renderer->dev->dev, &wait, 1000000000ULL);
    if (waited != VK_SUCCESS) {
        wlr_vk_error("Vulkan presentation copy completion (1 second limit)", waited);
        return false;
    }
    pass->completion_proven = true;
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = buffer->copy_memory, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    VkResult res = vkInvalidateMappedMemoryRanges(pass->renderer->dev->dev, 1, &range);
    if (res != VK_SUCCESS) {
        wlr_vk_error("Vulkan presentation copy readback", res);
        return false;
    }
    void *data;
    uint32_t format;
    size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(buffer->wlr_buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
            &data, &format, &stride)) return false;
    size_t row = (size_t)buffer->wlr_buffer->width * 4;
    bool ok = (format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888) && stride >= row;
    if (ok) {
        for (int y = 0; y < buffer->wlr_buffer->height; ++y) {
            memcpy((char *)data + y * stride, (char *)buffer->copy_mapping + y * row, row);
        }
    }
    wlr_buffer_end_data_ptr_access(buffer->wlr_buffer);
    return ok;
}
