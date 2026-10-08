#ifndef VK_FRAMEGEN_H
#define VK_FRAMEGEN_H

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdbool.h>
#include "rife_bridge.h"

typedef struct {
    // Pipeline de compute
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout descSetLayout;
    VkDescriptorSet descSet;
    VkDescriptorPool descPool;
    VkSampler sampler;

    // Histórico de frames (N-1 e N)
    VkImage frameHistory[2];
    VkDeviceMemory frameHistoryMem[2];
    VkImageView frameHistoryView[2];
    int currentHistoryIndex;

    // Frame gerado (N-0.5)
    VkImage outImage;
    VkDeviceMemory outMem;
    VkImageView outImageView;

    // Contexto de device/fila
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkCommandPool cmdPool;
    VkQueue queue_for_sync;
    bool initialized;
    uint32_t width;
    uint32_t height;

    // RIFE
    bool use_rife;
    RifeBridge rife_bridge;
    uint8_t *prev_rgb;

    // Staging pra saída (RIFE)
    VkBuffer stgOut;
    VkDeviceMemory stgOutMem;
    void *stgOutMapped;
} FramegenContext;

void framegen_init(FramegenContext* ctx, VkDevice device, VkPhysicalDevice physicalDevice,
                   uint32_t width, uint32_t height, VkCommandPool cmdPool, VkQueue queue);
void framegen_enable_rife(FramegenContext* ctx, const char* cli, const char* model, int gpuid);
void framegen_prepare_rife_cpu(FramegenContext* ctx, const uint8_t* rgba_current);
void framegen_dispatch(FramegenContext* ctx, VkCommandBuffer cmd,
                       VkImageView currFrameView, VkImage currFrameImage,
                       uint32_t width, uint32_t height);
void framegen_cleanup(FramegenContext* ctx, VkDevice device);

#endif