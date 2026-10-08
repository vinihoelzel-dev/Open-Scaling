#ifndef CAPTURE_X11_H
#define CAPTURE_X11_H

#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

typedef struct {
    Display   *dpy;
    Window     root;
    Window     target;
    int        width;
    int        height;
    int        depth;
    Visual    *visual;

    Pixmap     pixmap;
    bool       use_composite;

    XImage    *img;
    XShmSegmentInfo shminfo;
    bool       shm_attached;
} CaptureX11;

bool   capture_init(CaptureX11 *cap);
bool   capture_init_target(CaptureX11 *cap, Window target);
Window capture_select_window(void);
bool   capture_grab(CaptureX11 *cap);
bool   capture_grab_async(CaptureX11 *cap);
bool   capture_grab_wait(CaptureX11 *cap);
void   capture_shutdown(CaptureX11 *cap);

#define capture_data(cap)    ((unsigned char *)((cap)->img->data))
#define capture_stride(cap)  ((cap)->img->bytes_per_line)

#endif