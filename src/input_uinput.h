#ifndef INPUT_UINPUT_H
#define INPUT_UINPUT_H

#include <stdbool.h>

typedef struct {
    int fd;
    bool active;
} VirtualMouse;

bool virtual_mouse_init(VirtualMouse *mouse);
void virtual_mouse_shutdown(VirtualMouse *mouse);
bool virtual_mouse_move(VirtualMouse *mouse, int dx, int dy);
bool virtual_mouse_button(VirtualMouse *mouse, unsigned int button, bool pressed);
bool virtual_mouse_scroll(VirtualMouse *mouse, int amount);

#endif
