#ifndef VK_UPSCALE_H
#define VK_UPSCALE_H
#include <stdint.h>
#include <X11/Xlib.h>
int vk_upscale_run(Window target, uint32_t out_w, uint32_t out_h,
                   float scale, uint32_t max_w, uint32_t max_h);
#endif
