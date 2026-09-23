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

static const unsigned short keyboard_keys[] = {
    KEY_ESC, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_0, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB,
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
    KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_ENTER, KEY_LEFTCTRL,
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
    KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_LEFTSHIFT, KEY_BACKSLASH,
    KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT,
    KEY_SLASH, KEY_RIGHTSHIFT, KEY_KPASTERISK, KEY_LEFTALT, KEY_SPACE,
    KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_SYSRQ,
    KEY_SCROLLLOCK, KEY_PAUSE, KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE,
    KEY_END, KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    KEY_NUMLOCK, KEY_KPSLASH, KEY_KPMINUS, KEY_KPPLUS, KEY_KPENTER,
    KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6,
    KEY_KP7, KEY_KP8, KEY_KP9, KEY_KPDOT, KEY_RIGHTCTRL, KEY_RIGHTALT,
    KEY_LEFTMETA, KEY_RIGHTMETA, KEY_COMPOSE, KEY_MENU,
};

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

    for (size_t i = 0; i < sizeof(keyboard_keys) / sizeof(keyboard_keys[0]); i++) {
        if (ioctl(mouse->fd, UI_SET_KEYBIT, keyboard_keys[i]) < 0) {
            close(mouse->fd);
            mouse->fd = -1;
            return false;
        }
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

bool virtual_keyboard_key(VirtualMouse *mouse, unsigned short keycode,
                          bool pressed) {
    if (!mouse->active) return false;
    return emit_event(mouse, EV_KEY, keycode, pressed ? 1 : 0) &&
           sync_events(mouse);
}
