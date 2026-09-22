#ifndef CAPTURE_X11_H
#define CAPTURE_X11_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <stdbool.h>

typedef struct {
    Display         *dpy;
    Window           root;
    Window           target;
    int              width;
    int              height;
    int              depth;
    Visual          *visual;
    XShmSegmentInfo  shminfo;
    XImage          *img;
    bool             shm_attached;
} CaptureX11;

bool capture_init(CaptureX11 *cap);
bool capture_init_target(CaptureX11 *cap, Window target);
Window capture_select_window(void);
bool capture_grab(CaptureX11 *cap);
void capture_shutdown(CaptureX11 *cap);

static inline unsigned char *capture_data(CaptureX11 *cap) {
    return (unsigned char *)cap->img->data;
}
static inline int capture_stride(CaptureX11 *cap) {
    return cap->img->bytes_per_line;
}

#endif
