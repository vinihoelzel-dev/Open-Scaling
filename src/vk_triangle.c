// src/vk_triangle.c — Etapa 1: janela Vulkan + triangulo
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "[vk] %s falhou: %d\n", #x, _r); exit(1); } } while(0)

#define MAX_FRAMES 2

typedef struct {
    SDL_Window      *window;
    VkInstance       instance;
    VkSurfaceKHR     surface;
    VkPhysicalDevice physical;
    uint32_t         qfam;
    VkDevice         device;
    VkQueue          queue;
    VkSwapchainKHR   swapchain;
    VkFormat         fmt;
    VkExtent2D       extent;
    uint32_t         img_count;
    VkImage         *images;
    VkImageView     *views;
    VkRenderPass     renderpass;
    VkPipelineLayout pipe_layout;
    VkPipeline       pipeline;
    VkFramebuffer   *fbs;
    VkCommandPool    cmd_pool;
    VkCommandBuffer *cmds;
    VkSemaphore     *sem_avail;
    VkSemaphore     *sem_done;
    VkFence         *fences;
    uint32_t         frame;
} VkApp;

static uint32_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nao abriu %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        fprintf(stderr, "leitura incompleta de %s\n", path);
        exit(1);
    }
    fclose(f);
    *out_size = sz;
    return buf;
}

static VkShaderModule make_shader(VkDevice dev, const char *path) {
    size_t sz;
    uint32_t *code = read_file(path, &sz);
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sz,
        .pCode = code,
    };
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
    free(code);
    return m;
}

static void init_instance(VkApp *a) {
    unsigned ext_count = 0;
    SDL_Vulkan_GetInstanceExtensions(a->window, &ext_count, NULL);
    const char **exts = malloc(sizeof(char*) * ext_count);
    SDL_Vulkan_GetInstanceExtensions(a->window, &ext_count, exts);

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Open Scaling",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = exts,
    };
    VK_CHECK(vkCreateInstance(&ci, NULL, &a->instance));
    free(exts);

    if (!SDL_Vulkan_CreateSurface(a->window, a->instance, &a->surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface falhou: %s\n", SDL_GetError());
        exit(1);
    }
}

static void pick_device(VkApp *a) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(a->instance, &n, NULL);
    if (!n) { fprintf(stderr, "nenhuma GPU Vulkan\n"); exit(1); }
    VkPhysicalDevice *devs = malloc(sizeof(*devs) * n);
    vkEnumeratePhysicalDevices(a->instance, &n, devs);

    int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        printf("[vk] device %u: %s (type=%d)\n", i, p.deviceName, p.deviceType);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { best = i; break; }
        if (best < 0 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) best = i;
    }
    if (best < 0) best = 0;
    a->physical = devs[best];

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(a->physical, &p);
    printf("[vk] usando: %s\n", p.deviceName);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(a->physical, &qn, NULL);
    VkQueueFamilyProperties *qprops = malloc(sizeof(*qprops) * qn);
    vkGetPhysicalDeviceQueueFamilyProperties(a->physical, &qn, qprops);

    a->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if (!(qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(a->physical, i, a->surface, &present);
        if (present) { a->qfam = i; break; }
    }
    free(qprops);
    if (a->qfam == UINT32_MAX) { fprintf(stderr, "sem queue graphics+present\n"); exit(1); }
    free(devs);
}

static void create_device(VkApp *a) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = a->qfam,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    const char *exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = exts,
    };
    VK_CHECK(vkCreateDevice(a->physical, &ci, NULL, &a->device));
    vkGetDeviceQueue(a->device, a->qfam, 0, &a->queue);
}

static void create_swapchain(VkApp *a) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(a->physical, a->surface, &caps);

    uint32_t fmt_n;
    vkGetPhysicalDeviceSurfaceFormatsKHR(a->physical, a->surface, &fmt_n, NULL);
    VkSurfaceFormatKHR *fmts = malloc(sizeof(*fmts) * fmt_n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(a->physical, a->surface, &fmt_n, fmts);

    VkSurfaceFormatKHR chosen = fmts[0];
    for (uint32_t i = 0; i < fmt_n; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = fmts[i]; break;
        }
    }
    free(fmts);

    a->fmt = chosen.format;
    a->extent = caps.currentExtent;
    if (a->extent.width == UINT32_MAX) {
        int w, h; SDL_Vulkan_GetDrawableSize(a->window, &w, &h);
        a->extent.width  = w;
        a->extent.height = h;
    }

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount && img_count > caps.maxImageCount)
        img_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = a->surface,
        .minImageCount = img_count,
        .imageFormat = a->fmt,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = a->extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VK_CHECK(vkCreateSwapchainKHR(a->device, &ci, NULL, &a->swapchain));

    vkGetSwapchainImagesKHR(a->device, a->swapchain, &a->img_count, NULL);
    a->images = malloc(sizeof(VkImage) * a->img_count);
    vkGetSwapchainImagesKHR(a->device, a->swapchain, &a->img_count, a->images);

    a->views = malloc(sizeof(VkImageView) * a->img_count);
    for (uint32_t i = 0; i < a->img_count; i++) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = a->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = a->fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VK_CHECK(vkCreateImageView(a->device, &vci, NULL, &a->views[i]));
    }
    printf("[vk] swapchain: %ux%u, %u imagens\n",
           a->extent.width, a->extent.height, a->img_count);
}

static void create_renderpass(VkApp *a) {
    VkAttachmentDescription color = {
        .format = a->fmt,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color,
        .subpassCount = 1,
        .pSubpasses = &sub,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    VK_CHECK(vkCreateRenderPass(a->device, &ci, NULL, &a->renderpass));
}

static void create_pipeline(VkApp *a) {
    VkShaderModule vs = make_shader(a->device, "build/shaders/triangle.vert.spv");
    VkShaderModule fs = make_shader(a->device, "build/shaders/triangle.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };

    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = { 0, 0, (float)a->extent.width, (float)a->extent.height, 0, 1 };
    VkRect2D sc = { {0,0}, a->extent };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount = 1,  .pScissors = &sc,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .colorWriteMask = 0xF,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba,
    };

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    VK_CHECK(vkCreatePipelineLayout(a->device, &plci, NULL, &a->pipe_layout));

    VkGraphicsPipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .layout = a->pipe_layout,
        .renderPass = a->renderpass,
        .subpass = 0,
    };
    VK_CHECK(vkCreateGraphicsPipelines(a->device, VK_NULL_HANDLE, 1, &ci, NULL, &a->pipeline));

    vkDestroyShaderModule(a->device, vs, NULL);
    vkDestroyShaderModule(a->device, fs, NULL);
}

static void create_framebuffers(VkApp *a) {
    a->fbs = malloc(sizeof(VkFramebuffer) * a->img_count);
    for (uint32_t i = 0; i < a->img_count; i++) {
        VkFramebufferCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = a->renderpass,
            .attachmentCount = 1,
            .pAttachments = &a->views[i],
            .width = a->extent.width,
            .height = a->extent.height,
            .layers = 1,
        };
        VK_CHECK(vkCreateFramebuffer(a->device, &ci, NULL, &a->fbs[i]));
    }
}

static void create_commands(VkApp *a) {
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = a->qfam,
    };
    VK_CHECK(vkCreateCommandPool(a->device, &pci, NULL, &a->cmd_pool));

    a->cmds = malloc(sizeof(VkCommandBuffer) * MAX_FRAMES);
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = a->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MAX_FRAMES,
    };
    VK_CHECK(vkAllocateCommandBuffers(a->device, &ai, a->cmds));

    a->sem_avail = malloc(sizeof(VkSemaphore) * MAX_FRAMES);
    a->sem_done  = malloc(sizeof(VkSemaphore) * MAX_FRAMES);
    a->fences    = malloc(sizeof(VkFence) * MAX_FRAMES);
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (int i = 0; i < MAX_FRAMES; i++) {
        VK_CHECK(vkCreateSemaphore(a->device, &sci, NULL, &a->sem_avail[i]));
        VK_CHECK(vkCreateSemaphore(a->device, &sci, NULL, &a->sem_done[i]));
        VK_CHECK(vkCreateFence(a->device, &fci, NULL, &a->fences[i]));
    }
}

static void record_cmds(VkApp *a, uint32_t i, uint32_t img_idx) {
    VkCommandBuffer cmd = a->cmds[i];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    VkClearValue clear = {{{0.05f, 0.05f, 0.08f, 1.0f}}};
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = a->renderpass,
        .framebuffer = a->fbs[img_idx],
        .renderArea = { {0,0}, a->extent },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a->pipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));
}

static void draw(VkApp *a) {
    vkWaitForFences(a->device, 1, &a->fences[a->frame], VK_TRUE, UINT64_MAX);

    uint32_t img_idx;
    VkResult r = vkAcquireNextImageKHR(a->device, a->swapchain, UINT64_MAX,
        a->sem_avail[a->frame], VK_NULL_HANDLE, &img_idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return;

    vkResetFences(a->device, 1, &a->fences[a->frame]);
    record_cmds(a, a->frame, img_idx);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &a->sem_avail[a->frame],
        .pWaitDstStageMask = &wait,
        .commandBufferCount = 1, .pCommandBuffers = &a->cmds[a->frame],
        .signalSemaphoreCount = 1, .pSignalSemaphores = &a->sem_done[a->frame],
    };
    VK_CHECK(vkQueueSubmit(a->queue, 1, &si, a->fences[a->frame]));

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &a->sem_done[a->frame],
        .swapchainCount = 1, .pSwapchains = &a->swapchain,
        .pImageIndices = &img_idx,
    };
    vkQueuePresentKHR(a->queue, &pi);

    a->frame = (a->frame + 1) % MAX_FRAMES;
}

int vk_triangle_run(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }

    VkApp a = {0};
    a.window = SDL_CreateWindow("Open Scaling - Vulkan",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    init_instance(&a);
    pick_device(&a);
    create_device(&a);
    create_swapchain(&a);
    create_renderpass(&a);
    create_pipeline(&a);
    create_framebuffers(&a);
    create_commands(&a);

    printf("[vk] loop principal. feche a janela pra sair.\n");
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }
        draw(&a);
    }

    vkDeviceWaitIdle(a.device);
    SDL_DestroyWindow(a.window);
    SDL_Quit();
    return 0;
}
