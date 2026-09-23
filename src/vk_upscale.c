// src/vk_upscale.c — FSR 1 (EASU + RCAS) via Vulkan compute
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <X11/keysym.h>
#include "capture_x11.h"
#include "input_uinput.h"

#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "[vk] %s falhou: %d\n", #x, _r); exit(1); } } while(0)

#define MAX_FRAMES 2

typedef struct {
    SDL_Window       *window;
    VkInstance        instance;
    VkSurfaceKHR      surface;
    VkPhysicalDevice  physical;
    uint32_t          qfam;
    VkDevice          device;
    VkQueue           queue;

    VkSwapchainKHR    swapchain;
    VkFormat          fmt;
    VkExtent2D        extent;
    uint32_t          img_count;
    VkImage          *images;
    VkImageView      *views;
    VkFramebuffer    *fbs;

    VkRenderPass      present_rp;      // pass que só copia a textura pra swapchain
    VkPipelineLayout  present_layout;
    VkPipeline        present_pipe;
    VkDescriptorSetLayout present_dsl;
    VkDescriptorPool  present_pool;
    VkDescriptorSet   present_set;
    VkSampler         sampler;

    // Compute EASU
    VkDescriptorSetLayout easu_dsl;
    VkDescriptorPool      easu_pool;
    VkDescriptorSet       easu_set;
    VkPipelineLayout      easu_layout;
    VkPipeline            easu_pipe;

    // Compute RCAS
    VkDescriptorSetLayout rcas_dsl;
    VkDescriptorPool      rcas_pool;
    VkDescriptorSet       rcas_set;
    VkPipelineLayout      rcas_layout;
    VkPipeline            rcas_pipe;

    // Imagens de trabalho
    VkImage         input_img;    VkDeviceMemory input_mem;    VkImageView input_view;   // 1280x720
    VkImage         up_img;       VkDeviceMemory up_mem;       VkImageView up_view;      // 1920x1080
    VkImage         final_img;    VkDeviceMemory final_mem;    VkImageView final_view;   // 1920x1080

    // Staging buffer
    VkBuffer         stage_buf;
    VkDeviceMemory   stage_mem;
    void            *stage_map;
    size_t           stage_size;

    VkCommandPool    cmd_pool;
    VkCommandBuffer *cmds;
    VkSemaphore     *sem_avail;
    VkSemaphore     *sem_done;
    VkFence         *fences;
    uint32_t         frame;

    CaptureX11       cap;
    uint32_t         in_w, in_h;
    uint32_t         out_w, out_h;
    bool             mouse_locked;
    bool             suppress_lock_keyup;
    float            input_mouse_x, input_mouse_y;
    float            virtual_mouse_remainder_x, virtual_mouse_remainder_y;
    VirtualMouse     virtual_mouse;
} VkUp;

// ---------- Utilitários ----------
static uint32_t *read_file(const char *path, size_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nao abriu %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *b = malloc(sz); if (fread(b,1,sz,f) != (size_t)sz) exit(1);
    fclose(f); *out_sz = sz; return b;
}

static VkShaderModule mk_shader(VkDevice d, const char *p) {
    size_t sz; uint32_t *c = read_file(p, &sz);
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sz, .pCode = c,
    };
    VkShaderModule m; VK_CHECK(vkCreateShaderModule(d, &ci, NULL, &m));
    free(c); return m;
}

static uint32_t mem_type(VkPhysicalDevice p, uint32_t mask, VkMemoryPropertyFlags f) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mask & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    fprintf(stderr, "memoria nao encontrada\n"); exit(1);
}

static void mk_image(VkUp *a, uint32_t w, uint32_t h, VkFormat fmt,
                     VkImageUsageFlags usage,
                     VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(a->device, &ici, NULL, img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(a->device, *img, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(a->physical, mr.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VK_CHECK(vkAllocateMemory(a->device, &mai, NULL, mem));
    vkBindImageMemory(a->device, *img, *mem, 0);

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VK_CHECK(vkCreateImageView(a->device, &vci, NULL, view));
}

// ---------- Init ----------
static void init_instance(VkUp *a) {
    unsigned n = 0;
    SDL_Vulkan_GetInstanceExtensions(a->window, &n, NULL);
    const char **e = malloc(sizeof(char*) * n);
    SDL_Vulkan_GetInstanceExtensions(a->window, &n, e);
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Open Scaling FSR1", .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app, .enabledExtensionCount = n,
        .ppEnabledExtensionNames = e,
    };
    VK_CHECK(vkCreateInstance(&ci, NULL, &a->instance));
    free(e);
    if (!SDL_Vulkan_CreateSurface(a->window, a->instance, &a->surface)) {
        fprintf(stderr, "surface falhou\n"); exit(1);
    }
}

static void pick_device(VkUp *a) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(a->instance, &n, NULL);
    VkPhysicalDevice *d = malloc(sizeof(*d) * n);
    vkEnumeratePhysicalDevices(a->instance, &n, d);
    int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d[i], &p);
        printf("[vk] device %u: %s\n", i, p.deviceName);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { best = i; break; }
    }
    if (best < 0) best = 0;
    a->physical = d[best];
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(a->physical, &p);
    printf("[vk] usando: %s\n", p.deviceName);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(a->physical, &qn, NULL);
    VkQueueFamilyProperties *qp = malloc(sizeof(*qp) * qn);
    vkGetPhysicalDeviceQueueFamilyProperties(a->physical, &qn, qp);
    a->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if (!(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 pres = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(a->physical, i, a->surface, &pres);
        if (pres) { a->qfam = i; break; }
    }
    free(qp); free(d);
    if (a->qfam == UINT32_MAX) { fprintf(stderr, "sem queue\n"); exit(1); }
}

static void create_device(VkUp *a) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = a->qfam, .queueCount = 1, .pQueuePriorities = &prio,
    };
    const char *e[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = e,
    };
    VK_CHECK(vkCreateDevice(a->physical, &ci, NULL, &a->device));
    vkGetDeviceQueue(a->device, a->qfam, 0, &a->queue);
}

static void create_swapchain(VkUp *a) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(a->physical, a->surface, &caps);
    uint32_t fn;
    vkGetPhysicalDeviceSurfaceFormatsKHR(a->physical, a->surface, &fn, NULL);
    VkSurfaceFormatKHR *fmts = malloc(sizeof(*fmts) * fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(a->physical, a->surface, &fn, fmts);
    VkSurfaceFormatKHR ch = fmts[0];
    for (uint32_t i = 0; i < fn; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { ch = fmts[i]; break; }
    free(fmts);

    a->fmt = ch.format;
    a->extent = caps.currentExtent;
    if (a->extent.width == UINT32_MAX) {
        int w,h; SDL_Vulkan_GetDrawableSize(a->window, &w, &h);
        a->extent.width = w; a->extent.height = h;
    }
    uint32_t ic = caps.minImageCount + 1;
    if (caps.maxImageCount && ic > caps.maxImageCount) ic = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = a->surface, .minImageCount = ic,
        .imageFormat = a->fmt, .imageColorSpace = ch.colorSpace,
        .imageExtent = a->extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
    };
    VK_CHECK(vkCreateSwapchainKHR(a->device, &ci, NULL, &a->swapchain));
    vkGetSwapchainImagesKHR(a->device, a->swapchain, &a->img_count, NULL);
    a->images = malloc(sizeof(VkImage) * a->img_count);
    vkGetSwapchainImagesKHR(a->device, a->swapchain, &a->img_count, a->images);
    a->views = malloc(sizeof(VkImageView) * a->img_count);
    for (uint32_t i = 0; i < a->img_count; i++) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = a->images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = a->fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VK_CHECK(vkCreateImageView(a->device, &vci, NULL, &a->views[i]));
    }
    printf("[vk] swapchain %ux%u\n", a->extent.width, a->extent.height);
}

static void create_present_pass(VkUp *a) {
    // Render pass simples: 1 attachment color
    VkAttachmentDescription c = {
        .format = a->fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference r = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription s = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &r,
    };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &c,
        .subpassCount = 1, .pSubpasses = &s,
        .dependencyCount = 1, .pDependencies = &dep,
    };
    VK_CHECK(vkCreateRenderPass(a->device, &ci, NULL, &a->present_rp));

    a->fbs = malloc(sizeof(VkFramebuffer) * a->img_count);
    for (uint32_t i = 0; i < a->img_count; i++) {
        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = a->present_rp,
            .attachmentCount = 1, .pAttachments = &a->views[i],
            .width = a->extent.width, .height = a->extent.height, .layers = 1,
        };
        VK_CHECK(vkCreateFramebuffer(a->device, &fci, NULL, &a->fbs[i]));
    }

    // Pipeline de present (usa quad.vert/texture.frag reutilizados)
    VkShaderModule vs = mk_shader(a->device, "build/shaders/quad.vert.spv");
    VkShaderModule fs = mk_shader(a->device, "build/shaders/texture.frag.spv");
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage=VK_SHADER_STAGE_VERTEX_BIT, .module=vs, .pName="main" },
        { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage=VK_SHADER_STAGE_FRAGMENT_BIT, .module=fs, .pName="main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = {0,0,(float)a->extent.width,(float)a->extent.height,0,1};
    VkRect2D sc = {{0,0}, a->extent};
    VkPipelineViewportStateCreateInfo vps = { .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode=VK_POLYGON_MODE_FILL, .cullMode=VK_CULL_MODE_NONE, .frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth=1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask=0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount=1,.pAttachments=&cba };

    VkDescriptorSetLayoutBinding b = { .binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = { .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount=1, .pBindings=&b };
    VK_CHECK(vkCreateDescriptorSetLayout(a->device, &lci, NULL, &a->present_dsl));
    VkPipelineLayoutCreateInfo plci = { .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&a->present_dsl };
    VK_CHECK(vkCreatePipelineLayout(a->device, &plci, NULL, &a->present_layout));

    VkGraphicsPipelineCreateInfo pci = {
        .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=st,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,
        .pViewportState=&vps,.pRasterizationState=&rs,
        .pMultisampleState=&ms,.pColorBlendState=&cb,
        .layout=a->present_layout,.renderPass=a->present_rp,.subpass=0,
    };
    VK_CHECK(vkCreateGraphicsPipelines(a->device, VK_NULL_HANDLE, 1, &pci, NULL, &a->present_pipe));
    vkDestroyShaderModule(a->device, vs, NULL);
    vkDestroyShaderModule(a->device, fs, NULL);

    VkDescriptorPoolSize ps = { .type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount=1 };
    VkDescriptorPoolCreateInfo dpci = { .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets=1, .poolSizeCount=1, .pPoolSizes=&ps };
    VK_CHECK(vkCreateDescriptorPool(a->device, &dpci, NULL, &a->present_pool));
    VkDescriptorSetAllocateInfo ai = { .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool=a->present_pool, .descriptorSetCount=1, .pSetLayouts=&a->present_dsl };
    VK_CHECK(vkAllocateDescriptorSets(a->device, &ai, &a->present_set));

    VkSamplerCreateInfo sci = {
        .sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter=VK_FILTER_LINEAR, .minFilter=VK_FILTER_LINEAR,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VK_CHECK(vkCreateSampler(a->device, &sci, NULL, &a->sampler));
}

static void create_compute_pipeline(VkUp *a,
    const char *spv, uint32_t push_size,
    VkDescriptorSetLayout *dsl, VkDescriptorPool *pool, VkDescriptorSet *set,
    VkPipelineLayout *layout, VkPipeline *pipe)
{
    VkDescriptorSetLayoutBinding bs[2] = {
        { .binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding=1, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo lci = { .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount=2, .pBindings=bs };
    VK_CHECK(vkCreateDescriptorSetLayout(a->device, &lci, NULL, dsl));

    VkPushConstantRange pcr = { .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT, .offset=0, .size=push_size };
    VkPipelineLayoutCreateInfo plci = { .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=dsl, .pushConstantRangeCount=1, .pPushConstantRanges=&pcr };
    VK_CHECK(vkCreatePipelineLayout(a->device, &plci, NULL, layout));

    VkShaderModule sm = mk_shader(a->device, spv);
    VkComputePipelineCreateInfo cpci = {
        .sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage=VK_SHADER_STAGE_COMPUTE_BIT, .module=sm, .pName="main" },
        .layout=*layout,
    };
    VK_CHECK(vkCreateComputePipelines(a->device, VK_NULL_HANDLE, 1, &cpci, NULL, pipe));
    vkDestroyShaderModule(a->device, sm, NULL);

    VkDescriptorPoolSize ps[2] = {
        { .type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount=1 },
        { .type=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount=1 },
    };
    VkDescriptorPoolCreateInfo dpci = { .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets=1, .poolSizeCount=2, .pPoolSizes=ps };
    VK_CHECK(vkCreateDescriptorPool(a->device, &dpci, NULL, pool));
    VkDescriptorSetAllocateInfo ai = { .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool=*pool, .descriptorSetCount=1, .pSetLayouts=dsl };
    VK_CHECK(vkAllocateDescriptorSets(a->device, &ai, set));
}

static void write_storage_images(VkUp *a, VkDescriptorSet set, VkImageView in, VkImageView out) {
    VkDescriptorImageInfo i0 = { .sampler=a->sampler, .imageView=in,  .imageLayout=VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo i1 = { .imageView=out, .imageLayout=VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet ws[2] = {
        { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=set, .dstBinding=0, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo=&i0 },
        { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=set, .dstBinding=1, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .pImageInfo=&i1 },
    };
    vkUpdateDescriptorSets(a->device, 2, ws, 0, NULL);
}

static void create_resources(VkUp *a) {
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    mk_image(a, a->in_w, a->in_h, VK_FORMAT_R8G8B8A8_UNORM, u, &a->input_img, &a->input_mem, &a->input_view);
    mk_image(a, a->out_w, a->out_h, VK_FORMAT_R8G8B8A8_UNORM, u, &a->up_img,    &a->up_mem,    &a->up_view);
    mk_image(a, a->out_w, a->out_h, VK_FORMAT_R8G8B8A8_UNORM, u, &a->final_img, &a->final_mem, &a->final_view);

    // Staging buffer com os pixels nativos da janela capturada.
    a->stage_size = (size_t)a->in_w * a->in_h * 4;
    VkBufferCreateInfo bci = {
        .sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=a->stage_size,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode=VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(a->device, &bci, NULL, &a->stage_buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(a->device, a->stage_buf, &mr);
    VkMemoryAllocateInfo mai = {
        .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=mr.size,
        .memoryTypeIndex=mem_type(a->physical, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VK_CHECK(vkAllocateMemory(a->device, &mai, NULL, &a->stage_mem));
    vkBindBufferMemory(a->device, a->stage_buf, a->stage_mem, 0);
    VK_CHECK(vkMapMemory(a->device, a->stage_mem, 0, a->stage_size, 0, &a->stage_map));

    // Pipelines compute
    create_compute_pipeline(a, "build/shaders/fsr_easu.comp.spv", 64,
        &a->easu_dsl, &a->easu_pool, &a->easu_set, &a->easu_layout, &a->easu_pipe);
    create_compute_pipeline(a, "build/shaders/fsr_rcas.comp.spv", 16,
        &a->rcas_dsl, &a->rcas_pool, &a->rcas_set, &a->rcas_layout, &a->rcas_pipe);

    // Liga descriptors
    write_storage_images(a, a->easu_set, a->input_view, a->up_view);
    write_storage_images(a, a->rcas_set, a->up_view,    a->final_view);

    // Present descriptor
    VkDescriptorImageInfo ii = { .sampler=a->sampler, .imageView=a->final_view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=a->present_set, .dstBinding=0, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo=&ii };
    vkUpdateDescriptorSets(a->device, 1, &w, 0, NULL);
}

static void create_commands(VkUp *a) {
    VkCommandPoolCreateInfo pci = {
        .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex=a->qfam,
    };
    VK_CHECK(vkCreateCommandPool(a->device, &pci, NULL, &a->cmd_pool));
    a->cmds = malloc(sizeof(VkCommandBuffer) * MAX_FRAMES);
    VkCommandBufferAllocateInfo ai = { .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=a->cmd_pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=MAX_FRAMES };
    VK_CHECK(vkAllocateCommandBuffers(a->device, &ai, a->cmds));

    a->sem_avail = malloc(sizeof(VkSemaphore) * MAX_FRAMES);
    a->sem_done  = malloc(sizeof(VkSemaphore) * MAX_FRAMES);
    a->fences    = malloc(sizeof(VkFence) * MAX_FRAMES);
    VkSemaphoreCreateInfo sci = { .sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci = { .sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags=VK_FENCE_CREATE_SIGNALED_BIT };
    for (int i = 0; i < MAX_FRAMES; i++) {
        VK_CHECK(vkCreateSemaphore(a->device, &sci, NULL, &a->sem_avail[i]));
        VK_CHECK(vkCreateSemaphore(a->device, &sci, NULL, &a->sem_done[i]));
        VK_CHECK(vkCreateFence(a->device, &fci, NULL, &a->fences[i]));
    }
}

// ---------- FSR 1 constants ----------
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void fsr_easu_con(uint32_t c0[4], uint32_t c1[4], uint32_t c2[4], uint32_t c3[4],
                         float in_w, float in_h, float out_w, float out_h) {
    c0[0] = f2u(in_w/out_w);
    c0[1] = f2u(in_h/out_h);
    c0[2] = f2u(0.5f*in_w/out_w - 0.5f);
    c0[3] = f2u(0.5f*in_h/out_h - 0.5f);
    c1[0] = f2u(1.0f/in_w);
    c1[1] = f2u(1.0f/in_h);
    c1[2] = f2u( 1.0f/in_w);
    c1[3] = f2u(-1.0f/in_h);
    c2[0] = f2u(-1.0f/in_w);
    c2[1] = f2u( 2.0f/in_h);
    c2[2] = f2u( 1.0f/in_w);
    c2[3] = f2u( 2.0f/in_h);
    c3[0] = f2u( 0.0f/in_w);
    c3[1] = f2u( 4.0f/in_h);
    c3[2] = 0;
    c3[3] = 0;
}

// ---------- Upload + compute ----------
static void upload_input(VkUp *a) {
    if (!capture_grab(&a->cap)) return;
    unsigned char *src = capture_data(&a->cap);
    int src_stride = capture_stride(&a->cap);

    unsigned char *dst = a->stage_map;
    for (uint32_t y = 0; y < a->in_h; y++) {
        // XImage usa a primeira linha no topo; para o upload Vulkan, a
        // primeira linha do buffer precisa corresponder ao fim da imagem.
        unsigned char *srow = src + (size_t)(a->in_h - 1 - y) * src_stride;
        for (uint32_t x = 0; x < a->in_w; x++) {
            unsigned char *sp = srow + (size_t)x * 4;  // BGRA
            unsigned char *dp = dst + ((size_t)y * a->in_w + x) * 4;
            dp[0] = sp[2]; // R
            dp[1] = sp[1]; // G
            dp[2] = sp[0]; // B
            dp[3] = 255;
        }
    }
}

static void barrier(VkCommandBuffer cmd, VkImage img,
                    VkImageLayout oldL, VkImageLayout newL,
                    VkAccessFlags srcA, VkAccessFlags dstA,
                    VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
    VkImageMemoryBarrier b = {
        .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=oldL, .newLayout=newL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=img,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
        .srcAccessMask=srcA, .dstAccessMask=dstA,
    };
    vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0,NULL, 0,NULL, 1,&b);
}

static void draw(VkUp *a) {
    vkWaitForFences(a->device, 1, &a->fences[a->frame], VK_TRUE, UINT64_MAX);
    uint32_t ii;
    VkResult r = vkAcquireNextImageKHR(a->device, a->swapchain, UINT64_MAX,
        a->sem_avail[a->frame], VK_NULL_HANDLE, &ii);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return;

    upload_input(a);
    vkResetFences(a->device, 1, &a->fences[a->frame]);

    VkCommandBuffer cmd = a->cmds[a->frame];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);

    // 1) input_img: GENERAL -> TRANSFER_DST
    barrier(cmd, a->input_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { a->in_w, a->in_h, 1 },
    };
    vkCmdCopyBufferToImage(cmd, a->stage_buf, a->input_img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // 2) input_img: TRANSFER_DST -> GENERAL
    barrier(cmd, a->input_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // 3) up_img: UNDEFINED -> GENERAL
    barrier(cmd, a->up_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // 4) final_img: UNDEFINED -> GENERAL
    barrier(cmd, a->final_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // 5) EASU
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a->easu_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a->easu_layout, 0, 1, &a->easu_set, 0, NULL);
    uint32_t c0[4], c1[4], c2[4], c3[4];
    fsr_easu_con(c0, c1, c2, c3, (float)a->in_w, (float)a->in_h,
                 (float)a->out_w, (float)a->out_h);
    struct { uint32_t a[4], b[4], c[4], d[4]; } easu_pc = {
        {c0[0],c0[1],c0[2],c0[3]},
        {c1[0],c1[1],c1[2],c1[3]},
        {c2[0],c2[1],c2[2],c2[3]},
        {c3[0],c3[1],c3[2],c3[3]},
    };
    vkCmdPushConstants(cmd, a->easu_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, &easu_pc);
    vkCmdDispatch(cmd, (a->out_w + 63) / 64, a->out_h, 1);

    // 6) Barreira: up_img escrita -> lida pelo RCAS
    barrier(cmd, a->up_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // 7) RCAS
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a->rcas_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a->rcas_layout, 0, 1, &a->rcas_set, 0, NULL);
    // sharpness: 0 = max, 2 = nenhum. 0.2 = bom equilibrio.
    // con = exp2(-sharpness)
    float sharpness = 0.2f;
    float exp_val = 1.0f;
    // exp2f(-0.2) via libm
    extern float exp2f(float);
    exp_val = exp2f(-sharpness);
    uint32_t rcas_con = f2u(exp_val);
    vkCmdPushConstants(cmd, a->rcas_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &rcas_con);
    vkCmdDispatch(cmd, (a->out_w + 63) / 64, a->out_h, 1);

    // 8) final_img escrita -> lida pelo present
    barrier(cmd, a->final_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // 9) Present pass
    VkClearValue cv = {{{0,0,0,1}}};
    VkRenderPassBeginInfo rp = {
        .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass=a->present_rp, .framebuffer=a->fbs[ii],
        .renderArea={{0,0}, a->extent}, .clearValueCount=1, .pClearValues=&cv,
    };
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a->present_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a->present_layout, 0, 1, &a->present_set, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount=1, .pWaitSemaphores=&a->sem_avail[a->frame],
        .pWaitDstStageMask=&wait,
        .commandBufferCount=1, .pCommandBuffers=&cmd,
        .signalSemaphoreCount=1, .pSignalSemaphores=&a->sem_done[a->frame],
    };
    VK_CHECK(vkQueueSubmit(a->queue, 1, &si, a->fences[a->frame]));

    VkPresentInfoKHR pi = {
        .sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount=1, .pWaitSemaphores=&a->sem_done[a->frame],
        .swapchainCount=1, .pSwapchains=&a->swapchain, .pImageIndices=&ii,
    };
    vkQueuePresentKHR(a->queue, &pi);
    a->frame = (a->frame + 1) % MAX_FRAMES;
}

// ---------- Encaminhamento de entrada para a janela X11 capturada ----------
static void output_to_input(const VkUp *a, int out_x, int out_y, int *in_x, int *in_y) {
    if (out_x < 0) out_x = 0;
    if (out_y < 0) out_y = 0;
    if ((uint32_t)out_x >= a->out_w) out_x = (int)a->out_w - 1;
    if ((uint32_t)out_y >= a->out_h) out_y = (int)a->out_h - 1;
    *in_x = out_x * (int)a->in_w / (int)a->out_w;
    *in_y = out_y * (int)a->in_h / (int)a->out_h;
}

static unsigned int sdl_modifiers_to_x(SDL_Keymod mod) {
    unsigned int state = 0;
    if (mod & KMOD_SHIFT) state |= ShiftMask;
    if (mod & KMOD_CTRL)  state |= ControlMask;
    if (mod & KMOD_ALT)   state |= Mod1Mask;
    if (mod & KMOD_GUI)   state |= Mod4Mask;
    return state;
}

static KeySym sdl_to_x_keysym(SDL_Keycode key) {
    if (key >= SDLK_SPACE && key <= SDLK_z) return (KeySym)key;
    if (key >= SDLK_F1 && key <= SDLK_F12) return XK_F1 + (key - SDLK_F1);
    switch (key) {
    case SDLK_RETURN:    return XK_Return;
    case SDLK_TAB:       return XK_Tab;
    case SDLK_BACKSPACE: return XK_BackSpace;
    case SDLK_ESCAPE:    return XK_Escape;
    case SDLK_DELETE:    return XK_Delete;
    case SDLK_INSERT:    return XK_Insert;
    case SDLK_HOME:      return XK_Home;
    case SDLK_END:       return XK_End;
    case SDLK_PAGEUP:    return XK_Prior;
    case SDLK_PAGEDOWN:  return XK_Next;
    case SDLK_UP:        return XK_Up;
    case SDLK_DOWN:      return XK_Down;
    case SDLK_LEFT:      return XK_Left;
    case SDLK_RIGHT:     return XK_Right;
    default:              return NoSymbol;
    }
}

static void send_motion(VkUp *a, int x, int y, SDL_Keymod mod) {
    XEvent event = {0};
    event.xmotion.type = MotionNotify;
    event.xmotion.display = a->cap.dpy;
    event.xmotion.window = a->cap.target;
    event.xmotion.root = a->cap.root;
    event.xmotion.same_screen = True;
    event.xmotion.x = x;
    event.xmotion.y = y;
    event.xmotion.state = sdl_modifiers_to_x(mod);
    /* propate=True: muitos jogos (SDL2/GLFW/Unity) só aceitam eventos sinteticos
     * quando o flag de propagacao esta ligado; com False eles descartam em silencio. */
    XSendEvent(a->cap.dpy, a->cap.target, True, PointerMotionMask, &event);
    XFlush(a->cap.dpy);
}

static void forward_motion(VkUp *a, int out_x, int out_y, SDL_Keymod mod) {
    int x, y;
    output_to_input(a, out_x, out_y, &x, &y);
    send_motion(a, x, y, mod);
}

static void forward_relative_motion(VkUp *a, int delta_x, int delta_y,
                                    SDL_Keymod mod) {
    if (a->virtual_mouse.active) {
        a->virtual_mouse_remainder_x += (float)delta_x * a->in_w / a->out_w;
        a->virtual_mouse_remainder_y += (float)delta_y * a->in_h / a->out_h;
        int virtual_x = (int)a->virtual_mouse_remainder_x;
        int virtual_y = (int)a->virtual_mouse_remainder_y;
        a->virtual_mouse_remainder_x -= virtual_x;
        a->virtual_mouse_remainder_y -= virtual_y;
        virtual_mouse_move(&a->virtual_mouse, virtual_x, virtual_y);
        return;
    }
    a->input_mouse_x += (float)delta_x * a->in_w / a->out_w;
    a->input_mouse_y += (float)delta_y * a->in_h / a->out_h;
    if (a->input_mouse_x < 0.0f) a->input_mouse_x = 0.0f;
    if (a->input_mouse_y < 0.0f) a->input_mouse_y = 0.0f;
    if (a->input_mouse_x >= a->in_w) a->input_mouse_x = a->in_w - 1.0f;
    if (a->input_mouse_y >= a->in_h) a->input_mouse_y = a->in_h - 1.0f;
    send_motion(a, (int)(a->input_mouse_x + 0.5f),
                (int)(a->input_mouse_y + 0.5f), mod);
}

static void send_button(VkUp *a, int type, int x, int y,
                        unsigned int button, SDL_Keymod mod) {
    XEvent event = {0};
    event.xbutton.type = type;
    event.xbutton.display = a->cap.dpy;
    event.xbutton.window = a->cap.target;
    event.xbutton.root = a->cap.root;
    event.xbutton.same_screen = True;
    event.xbutton.x = x;
    event.xbutton.y = y;
    event.xbutton.button = button;
    event.xbutton.state = sdl_modifiers_to_x(mod);
    XSendEvent(a->cap.dpy, a->cap.target, False,
               type == ButtonPress ? ButtonPressMask : ButtonReleaseMask, &event);
    XFlush(a->cap.dpy);
}

static void forward_button(VkUp *a, int type, int out_x, int out_y,
                           unsigned int button, SDL_Keymod mod) {
    int x, y;
    output_to_input(a, out_x, out_y, &x, &y);
    send_button(a, type, x, y, button, mod);
}

static void forward_key(VkUp *a, int type, const SDL_KeyboardEvent *key) {
    KeySym sym = sdl_to_x_keysym(key->keysym.sym);
    KeyCode code = XKeysymToKeycode(a->cap.dpy, sym);
    if (sym == NoSymbol || !code) return;
    XEvent event = {0};
    event.xkey.type = type;
    event.xkey.display = a->cap.dpy;
    event.xkey.window = a->cap.target;
    event.xkey.root = a->cap.root;
    event.xkey.same_screen = True;
    event.xkey.keycode = code;
    event.xkey.state = sdl_modifiers_to_x((SDL_Keymod)key->keysym.mod);
    XSendEvent(a->cap.dpy, a->cap.target, False,
               type == KeyPress ? KeyPressMask : KeyReleaseMask, &event);
    XFlush(a->cap.dpy);
}

static void forward_input(VkUp *a, const SDL_Event *event) {
    switch (event->type) {
    case SDL_MOUSEMOTION:
        if (a->mouse_locked)
            forward_relative_motion(a, event->motion.xrel, event->motion.yrel,
                                    SDL_GetModState());
        else
            forward_motion(a, event->motion.x, event->motion.y,
                           SDL_GetModState());
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (a->mouse_locked && a->virtual_mouse.active)
            virtual_mouse_button(&a->virtual_mouse, event->button.button,
                                 event->type == SDL_MOUSEBUTTONDOWN);
        else if (a->mouse_locked)
            send_button(a, event->type == SDL_MOUSEBUTTONDOWN ? ButtonPress : ButtonRelease,
                        (int)(a->input_mouse_x + 0.5f),
                        (int)(a->input_mouse_y + 0.5f), event->button.button,
                        SDL_GetModState());
        else
            forward_button(a, event->type == SDL_MOUSEBUTTONDOWN ? ButtonPress : ButtonRelease,
                           event->button.x, event->button.y, event->button.button,
                           SDL_GetModState());
        break;
    case SDL_MOUSEWHEEL: {
        int x, y;
        SDL_GetMouseState(&x, &y);
        unsigned int button = event->wheel.y > 0 ? 4 : 5;
        if (event->wheel.y) {
            if (a->mouse_locked && a->virtual_mouse.active)
                virtual_mouse_scroll(&a->virtual_mouse, event->wheel.y);
            else {
                forward_button(a, ButtonPress, x, y, button, SDL_GetModState());
                forward_button(a, ButtonRelease, x, y, button, SDL_GetModState());
            }
        }
        break;
    }
    case SDL_KEYDOWN:
        if (event->key.keysym.sym != SDLK_ESCAPE)
            forward_key(a, KeyPress, &event->key);
        break;
    case SDL_KEYUP:
        if (event->key.keysym.sym != SDLK_ESCAPE)
            forward_key(a, KeyRelease, &event->key);
        break;
    }
}

static void toggle_mouse_lock(VkUp *a) {
    SDL_bool enable = a->mouse_locked ? SDL_FALSE : SDL_TRUE;
    if (SDL_SetRelativeMouseMode(enable) != 0) {
        fprintf(stderr, "[input] nao foi possivel travar o mouse: %s\n", SDL_GetError());
        return;
    }
    a->mouse_locked = !a->mouse_locked;
    a->input_mouse_x = a->in_w * 0.5f;
    a->input_mouse_y = a->in_h * 0.5f;
    a->virtual_mouse_remainder_x = 0.0f;
    a->virtual_mouse_remainder_y = 0.0f;
    printf("[input] mouse %s (AltGr+L para alternar)\n",
           a->mouse_locked ? "travado" : "liberado");
}

// Mantem uma cadencia de 60 Hz sem acumular atraso entre os frames.
static void wait_for_frame_deadline(uint64_t deadline, uint64_t frequency) {
    uint64_t now = SDL_GetPerformanceCounter();
    if (now >= deadline) return;

    uint64_t remaining_ms = (deadline - now) * 1000 / frequency;
    if (remaining_ms > 1) SDL_Delay((Uint32)(remaining_ms - 1));

    // SDL_Delay tem granularidade de milissegundos; a espera final curta
    // evita oscilações grandes no frametime.
    while (SDL_GetPerformanceCounter() < deadline) {}
}

int vk_upscale_run(Window target, uint32_t out_w, uint32_t out_h,
                   float scale, uint32_t max_w, uint32_t max_h) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    VkUp a = {0};
    a.virtual_mouse.fd = -1;
    if (!capture_init_target(&a.cap, target)) {
        fprintf(stderr, "capture_init_target falhou\n");
        SDL_Quit();
        return 1;
    }
    a.in_w = a.cap.width;
    a.in_h = a.cap.height;
    uint32_t screen_w = DisplayWidth(a.cap.dpy, DefaultScreen(a.cap.dpy));
    uint32_t screen_h = DisplayHeight(a.cap.dpy, DefaultScreen(a.cap.dpy));
    uint32_t limit_w = max_w < screen_w ? max_w : screen_w;
    uint32_t limit_h = max_h < screen_h ? max_h : screen_h;
    uint32_t wanted_w = out_w ? out_w : (uint32_t)(a.in_w * scale + 0.5f);
    uint32_t wanted_h = out_h ? out_h : (uint32_t)(a.in_h * scale + 0.5f);
    float fit = 1.0f;
    if (wanted_w > limit_w) fit = (float)limit_w / wanted_w;
    if (wanted_h * fit > limit_h) fit = (float)limit_h / wanted_h;
    a.out_w = (uint32_t)(wanted_w * fit + 0.5f);
    a.out_h = (uint32_t)(wanted_h * fit + 0.5f);
    printf("[fsr] entrada: %ux%u; perfil: %.1fx; teto: %ux%u; tela: %ux%u; saida: %ux%u\n",
           a.in_w, a.in_h, scale, max_w, max_h, screen_w, screen_h,
           a.out_w, a.out_h);
    a.window = SDL_CreateWindow("Open Scaling - FSR 1 (EASU+RCAS)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (int)a.out_w, (int)a.out_h, SDL_WINDOW_VULKAN);
    if (!a.window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        capture_shutdown(&a.cap);
        SDL_Quit();
        return 1;
    }

    init_instance(&a);
    pick_device(&a);
    create_device(&a);
    create_swapchain(&a);
    create_present_pass(&a);
    create_resources(&a);
    create_commands(&a);

    if (virtual_mouse_init(&a.virtual_mouse))
        printf("[input] mouse virtual uinput ativo para jogos.\n");
    else
        fprintf(stderr, "[input] mouse virtual indisponivel; usando fallback X11.\n");

    printf("[vk] limite: 60 FPS. ESC/F3/F4 pra sair; AltGr+L trava/libera o mouse.\n");
    bool run = true;
    uint64_t f = 0, t0 = SDL_GetTicks64();
    const uint64_t perf_freq = SDL_GetPerformanceFrequency();
    const uint64_t frame_interval = perf_freq / 60;
    uint64_t next_frame = SDL_GetPerformanceCounter();
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) run = false;
            if (e.type == SDL_KEYDOWN &&
                (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_F3 ||
                 e.key.keysym.sym == SDLK_F4)) {
                run = false;
                continue;
            }
            if (e.type == SDL_KEYDOWN && e.key.repeat == 0 &&
                e.key.keysym.sym == SDLK_l && (e.key.keysym.mod & KMOD_RALT)) {
                toggle_mouse_lock(&a);
                a.suppress_lock_keyup = true;
                continue;
            }
            if (e.type == SDL_KEYUP && a.suppress_lock_keyup &&
                e.key.keysym.sym == SDLK_l) {
                a.suppress_lock_keyup = false;
                continue;
            }
            forward_input(&a, &e);
        }
        draw(&a);
        f++;
        uint64_t now = SDL_GetTicks64();
        if (now - t0 >= 1000) {
            printf("[fps] %.1f\n", f * 1000.0 / (now - t0));
            f = 0; t0 = now;
        }

        next_frame += frame_interval;
        uint64_t perf_now = SDL_GetPerformanceCounter();
        if (perf_now > next_frame)
            next_frame = perf_now;
        else
            wait_for_frame_deadline(next_frame, perf_freq);
    }
    vkDeviceWaitIdle(a.device);
    if (a.mouse_locked) SDL_SetRelativeMouseMode(SDL_FALSE);
    virtual_mouse_shutdown(&a.virtual_mouse);
    capture_shutdown(&a.cap);
    SDL_DestroyWindow(a.window);
    SDL_Quit();
    return 0;
}
