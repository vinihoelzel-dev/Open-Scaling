#include "input_uinput.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool emit_event(VirtualMouse *mouse, unsigned short type,
                       unsigned short code, int value) {
    struct input_event event = { .type = type, .code = code, .value = value };
    return write(mouse->fd, &event, sizeof(event)) == (ssize_t)sizeof(event);
}

static bool sync_events(VirtualMouse *mouse) {
    return emit_event(mouse, EV_SYN, SYN_REPORT, 0);
}

bool virtual_mouse_init(VirtualMouse *mouse) {
    memset(mouse, 0, sizeof(*mouse));
    mouse->fd = -1;
    mouse->fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (mouse->fd < 0) return false;

    if (ioctl(mouse->fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(mouse->fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(mouse->fd, UI_SET_RELBIT, REL_X) < 0 ||
        ioctl(mouse->fd, UI_SET_RELBIT, REL_Y) < 0 ||
        ioctl(mouse->fd, UI_SET_RELBIT, REL_WHEEL) < 0 ||
        ioctl(mouse->fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
        ioctl(mouse->fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0 ||
        ioctl(mouse->fd, UI_SET_KEYBIT, BTN_RIGHT) < 0) {
        close(mouse->fd);
        mouse->fd = -1;
        return false;
    }

    struct uinput_setup setup = { .id = { BUS_USB, 0x1, 0x1 } };
    strncpy(setup.name, "Open Scaling Virtual Mouse", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(mouse->fd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(mouse->fd, UI_DEV_CREATE) < 0) {
        close(mouse->fd);
        mouse->fd = -1;
        return false;
    }
    mouse->active = true;
    return true;
}

void virtual_mouse_shutdown(VirtualMouse *mouse) {
    if (mouse->active) ioctl(mouse->fd, UI_DEV_DESTROY);
    if (mouse->fd >= 0) close(mouse->fd);
    mouse->fd = -1;
    mouse->active = false;
}

bool virtual_mouse_move(VirtualMouse *mouse, int dx, int dy) {
    if (!mouse->active) return false;
    if (dx && !emit_event(mouse, EV_REL, REL_X, dx)) return false;
    if (dy && !emit_event(mouse, EV_REL, REL_Y, dy)) return false;
    return sync_events(mouse);
}

bool virtual_mouse_button(VirtualMouse *mouse, unsigned int button, bool pressed) {
    static const unsigned short button_map[] = { 0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT };
    if (!mouse->active || button >= sizeof(button_map) / sizeof(button_map[0])) return false;
    return emit_event(mouse, EV_KEY, button_map[button], pressed) && sync_events(mouse);
}

bool virtual_mouse_scroll(VirtualMouse *mouse, int amount) {
    return mouse->active && emit_event(mouse, EV_REL, REL_WHEEL, amount) && sync_events(mouse);
}
