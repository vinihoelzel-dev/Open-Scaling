#ifndef VK_UPSCALE_H
#define VK_UPSCALE_H
#include <stdint.h>
#include <stdbool.h>
#include <X11/Xlib.h>
// sharpness: 0.0 (sem nitidez extra) .. 1.0 (maxima). vsync: bloquear em 60 Hz.
int vk_upscale_run(Window target, uint32_t out_w, uint32_t out_h,
                   float scale, uint32_t max_w, uint32_t max_h,
                   float sharpness, bool vsync);
#endif
