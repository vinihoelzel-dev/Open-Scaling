#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "XOpenDisplay falhou\n"); return 1; }

    Window root = DefaultRootWindow(dpy);
    Window r, p, *children;
    unsigned int n;

    if (!XQueryTree(dpy, root, &r, &p, &children, &n)) return 1;

    printf("%-12s %-12s %-10s %s\n", "WINDOW", "TAMANHO", "POSICAO", "TITULO");
    printf("---------------------------------------------------------------\n");

    for (unsigned int i = 0; i < n; i++) {
        Window w = children[i];
        XWindowAttributes a;
        if (!XGetWindowAttributes(dpy, w, &a)) continue;
        if (a.map_state != IsViewable) continue;
        if (a.override_redirect) continue;
        if (a.width < 100 || a.height < 100) continue;

        char *name = NULL;
        XFetchName(dpy, w, &name);
        printf("0x%08lx   %4dx%-6d   +%d,+%-6d %s\n",
               w, a.width, a.height, a.x, a.y,
               name ? name : "(sem titulo)");
        if (name) XFree(name);
    }
    XFree(children);
    XCloseDisplay(dpy);
    return 0;
}
