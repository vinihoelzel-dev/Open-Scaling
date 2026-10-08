#include "vk_framegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "[framegen] %s falhou: %d\n", #x, _r); exit(1); } } while(0)

static uint32_t find_mem_type(VkPhysicalDevice physical, uint32_t mask, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((mask & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    fprintf(stderr, "[framegen] tipo de memória não encontrado\n");
    exit(1);
}

static void create_image(VkDevice device, VkPhysicalDevice physical, uint32_t w, uint32_t h,
                         VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, // Começa indefinido
    };
    VK_CHECK(vkCreateImage(device, &ici, NULL, img));
    
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, *img, &mr);
    
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = find_mem_type(physical, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VK_CHECK(vkAllocateMemory(device, &mai, NULL, mem));
    VK_CHECK(vkBindImageMemory(device, *img, *mem, 0));
    
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VK_CHECK(vkCreateImageView(device, &vci, NULL, view));
}

// --- NOVO: Função para transicionar layout (UNDEFINED -> GENERAL) ---
static void transition_layout(VkDevice device, VkCommandPool cmdPool, VkQueue queue, VkImage image) {
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
    
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, // O shader precisa disso!
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VK_CHECK(vkEndCommandBuffer(cmd));
    
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue)); // Espera terminar antes de continuar
    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

static uint32_t* read_file(const char *path, size_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[framegen] ERRO CRITICO: não abriu %s. Verifique se o shader foi compilado!\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *b = malloc(sz);
    if (fread(b, 1, sz, f) != (size_t)sz) exit(1);
    fclose(f);
    *out_sz = sz;
    return b;
}

static VkShaderModule mk_shader(VkDevice d, const char *p) {
    size_t sz;
    uint32_t *c = read_file(p, &sz);
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = c };
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(d, &ci, NULL, &m));
    free(c);
    return m;
}

void framegen_init(FramegenContext* ctx, VkDevice device, VkPhysicalDevice physicalDevice,
                   uint32_t width, uint32_t height, VkCommandPool cmdPool, VkQueue queue) {
    ctx->device = device;
    ctx->physicalDevice = physicalDevice;
    ctx->queue_for_sync = queue;
    ctx->use_rife = false;
    ctx->prev_rgb = NULL;
    ctx->stgOut = VK_NULL_HANDLE;
    ctx->stgOutMem = VK_NULL_HANDLE;
    ctx->stgOutMapped = NULL;
    ctx->cmdPool = cmdPool;
    ctx->currentHistoryIndex = 0;
    ctx->width = width;
    ctx->height = height;
    
    printf("[framegen] Inicializando para %dx%d\n", width, height);
    
    // 1. Criar imagens
    VkImageUsageFlags histUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int i = 0; i < 2; i++) {
        create_image(device, physicalDevice, width, height, histUsage,
                     &ctx->frameHistory[i], &ctx->frameHistoryMem[i], &ctx->frameHistoryView[i]);
        // TRANSICIONA LAYOUT: Isso deve evitar o Sinal 11!
        transition_layout(device, cmdPool, queue, ctx->frameHistory[i]);
    }
    
    // Cria sampler uma única vez, reaproveitado em todos os frames.
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VK_CHECK(vkCreateSampler(device, &sci, NULL, &ctx->sampler));

    VkImageUsageFlags outUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    create_image(device, physicalDevice, width, height, outUsage,
                 &ctx->outImage, &ctx->outMem, &ctx->outImageView);
    transition_layout(device, cmdPool, queue, ctx->outImage);
    
    // 2. Criar Pipeline (igual antes)
    VkDescriptorSetLayoutBinding bindings[3] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bindings };
    VK_CHECK(vkCreateDescriptorSetLayout(device, &lci, NULL, &ctx->descSetLayout));
    
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &ctx->descSetLayout };
    VK_CHECK(vkCreatePipelineLayout(device, &plci, NULL, &ctx->layout));
    
    VkShaderModule sm = mk_shader(device, "build/shaders/flow_warp.comp.spv");
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
        .layout = ctx->layout,
    };
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &ctx->pipeline));
    vkDestroyShaderModule(device, sm, NULL);
    
    VkDescriptorPoolSize poolSizes[2] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2 },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1 },
    };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = poolSizes };
    VK_CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &ctx->descPool));
    
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = ctx->descPool, .descriptorSetCount = 1, .pSetLayouts = &ctx->descSetLayout };
    VK_CHECK(vkAllocateDescriptorSets(device, &ai, &ctx->descSet));
    
    ctx->initialized = true;
    printf("[framegen] ✅ Inicializado e layouts transicionados!\n");
}

void framegen_enable_rife(FramegenContext* ctx, const char* cli, const char* model, int gpuid) {
    (void)cli;
    if (!rife_bridge_init(&ctx->rife_bridge, model, gpuid)) {
        fprintf(stderr, "[framegen] RIFE init falhou\n");
        return;
    }
    ctx->use_rife = true;

    VkDeviceSize sz = (VkDeviceSize)ctx->width * ctx->height * 4;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sz,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(ctx->device, &bci, NULL, &ctx->stgOut));

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(ctx->device, ctx->stgOut, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = find_mem_type(ctx->physicalDevice, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VK_CHECK(vkAllocateMemory(ctx->device, &mai, NULL, &ctx->stgOutMem));
    VK_CHECK(vkBindBufferMemory(ctx->device, ctx->stgOut, ctx->stgOutMem, 0));
    VK_CHECK(vkMapMemory(ctx->device, ctx->stgOutMem, 0, sz, 0, &ctx->stgOutMapped));

    printf("[framegen] RIFE pronto (%ux%u, staging %.1f MB)\n",
           ctx->width, ctx->height, sz / 1024.0 / 1024.0);
}

void framegen_prepare_rife_cpu(FramegenContext* ctx, const uint8_t* rgba_current) {
    if (!ctx->use_rife || !ctx->initialized) return;

    uint32_t w = ctx->width, h = ctx->height;
    uint32_t n = w * h;

    // Converte RGBA -> RGB (in place nao da, aloca)
    uint8_t* cur_rgb = malloc(n * 3);
    for (uint32_t i = 0; i < n; i++) {
        cur_rgb[i*3+0] = rgba_current[i*4+0];
        cur_rgb[i*3+1] = rgba_current[i*4+1];
        cur_rgb[i*3+2] = rgba_current[i*4+2];
    }

    uint8_t* out_rgb = cur_rgb;  // fallback: primeiro frame

    if (ctx->prev_rgb) {
        // Temos 2 frames: roda RIFE
        out_rgb = malloc(n * 3);
        bool ok = rife_bridge_interpolate(&ctx->rife_bridge,
                                          ctx->prev_rgb, cur_rgb,
                                          w, h, out_rgb);
        if (!ok) {
            fprintf(stderr, "[framegen] RIFE falhou, usando current\n");
            memcpy(out_rgb, cur_rgb, n * 3);
        }
    }

    // Salva o atual para o proximo frame
    if (!ctx->prev_rgb) ctx->prev_rgb = malloc(n * 3);
    memcpy(ctx->prev_rgb, cur_rgb, n * 3);

    // RGB -> RGBA no staging de saida
    uint8_t* rgbaOut = (uint8_t*)ctx->stgOutMapped;
    for (uint32_t i = 0; i < n; i++) {
        rgbaOut[i*4+0] = out_rgb[i*3+0];
        rgbaOut[i*4+1] = out_rgb[i*3+1];
        rgbaOut[i*4+2] = out_rgb[i*3+2];
        rgbaOut[i*4+3] = 255;
    }

    if (out_rgb != cur_rgb) free(out_rgb);
    free(cur_rgb);

    // Upload: staging -> outImage
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer tmp;
    VK_CHECK(vkAllocateCommandBuffers(ctx->device, &cbai, &tmp));

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(tmp, &bi));

    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { w, h, 1 },
    };
    vkCmdCopyBufferToImage(tmp, ctx->stgOut, ctx->outImage,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    VK_CHECK(vkEndCommandBuffer(tmp));

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &tmp,
    };
    VK_CHECK(vkQueueSubmit(ctx->queue_for_sync, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx->queue_for_sync));
    vkFreeCommandBuffers(ctx->device, ctx->cmdPool, 1, &tmp);
}

void framegen_dispatch(FramegenContext* ctx, VkCommandBuffer cmd, VkImageView currFrameView, VkImage currFrameImage, uint32_t width, uint32_t height) {
    if (!ctx->initialized) {
        fprintf(stderr, "[framegen] Dispatch: context nao inicializado!\n");
        return;
    }
    printf("[framegen] Dispatch: width=%u, height=%u, prevIdx=%d\n", width, height, ctx->currentHistoryIndex);

    int prevIndex = ctx->currentHistoryIndex;
    ctx->currentHistoryIndex = (ctx->currentHistoryIndex + 1) % 2;

    if (ctx->use_rife) {
        return;  // <<< AQUI
    }

    VkDescriptorImageInfo imgInfo[3] = {
        { .sampler = ctx->sampler, .imageView = ctx->frameHistoryView[prevIndex], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .sampler = ctx->sampler, .imageView = currFrameView,                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        {                          .imageView = ctx->outImageView,                .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
    };

    VkWriteDescriptorSet writes[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ctx->descSet, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &imgInfo[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ctx->descSet, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &imgInfo[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ctx->descSet, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .pImageInfo = &imgInfo[2] },
    };
    vkUpdateDescriptorSets(ctx->device, 3, writes, 0, NULL);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->layout, 0, 1, &ctx->descSet, 0, NULL);
    vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);

    VkImageCopy region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { width, height, 1 },
    };
    vkCmdCopyImage(cmd,
                   currFrameImage,               VK_IMAGE_LAYOUT_GENERAL,
                   ctx->frameHistory[prevIndex], VK_IMAGE_LAYOUT_GENERAL,
                   1, &region);
}

void framegen_cleanup(FramegenContext* ctx, VkDevice device) {
    if (!ctx->initialized) return;
    if (ctx->sampler) vkDestroySampler(device, ctx->sampler, NULL);
    for (int i = 0; i < 2; i++) {
        if (ctx->frameHistoryView[i]) vkDestroyImageView(device, ctx->frameHistoryView[i], NULL);
        if (ctx->frameHistory[i]) vkDestroyImage(device, ctx->frameHistory[i], NULL);
        if (ctx->frameHistoryMem[i]) vkFreeMemory(device, ctx->frameHistoryMem[i], NULL);
    }
    if (ctx->outImageView) vkDestroyImageView(device, ctx->outImageView, NULL);
    if (ctx->outImage) vkDestroyImage(device, ctx->outImage, NULL);
    if (ctx->outMem) vkFreeMemory(device, ctx->outMem, NULL);
    if (ctx->pipeline) vkDestroyPipeline(device, ctx->pipeline, NULL);
    if (ctx->layout) vkDestroyPipelineLayout(device, ctx->layout, NULL);
    if (ctx->descSetLayout) vkDestroyDescriptorSetLayout(device, ctx->descSetLayout, NULL);
    if (ctx->descPool) vkDestroyDescriptorPool(device, ctx->descPool, NULL);
    ctx->initialized = false;
}