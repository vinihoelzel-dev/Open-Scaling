#include "capture_x11.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/Xproto.h>
#include <X11/extensions/Xcomposite.h>   // <-- adiciona essa

static int cap_x_error(Display *dpy, XErrorEvent *e) {
    // BadMatch em XShmGetImage: deixa o XShmGetImage retornar False
    // em vez de o Xlib abortar o processo.
    char buf[256];
    XGetErrorText(dpy, e->error_code, buf, sizeof buf);
    fprintf(stderr, "[cap] X error ignorado: %s (req=%d minor=%d)\n",
            buf, e->request_code, e->minor_code);
    return 0;
}

bool capture_init(CaptureX11 *cap) {
    return capture_init_target(cap, 0);
}

static bool is_client_window(Display *dpy, Window window, Atom wm_state) {
    Atom type;
    int format;
    unsigned long items, bytes_after;
    unsigned char *data = NULL;
    int status = XGetWindowProperty(dpy, window, wm_state, 0, 2, False,
                                    wm_state, &type, &format, &items,
                                    &bytes_after, &data);
    if (data) XFree(data);
    return status == Success && type == wm_state;
}

static Window find_client_window(Display *dpy, Window window, Atom wm_state) {
    if (is_client_window(dpy, window, wm_state)) return window;

    Window root, parent, *children = NULL;
    unsigned int count = 0;
    if (!XQueryTree(dpy, window, &root, &parent, &children, &count)) return 0;
    Window client = 0;
    for (unsigned int i = 0; i < count && !client; i++)
        client = find_client_window(dpy, children[i], wm_state);
    if (children) XFree(children);
    return client;
}

Window capture_select_window(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[erro] XOpenDisplay falhou.\n");
        return 0;
    }

    Window root = DefaultRootWindow(dpy), ignored, parent, *children = NULL;
    unsigned int count = 0;
    if (!XQueryTree(dpy, root, &ignored, &parent, &children, &count)) {
        XCloseDisplay(dpy);
        return 0;
    }

    Window *choices = calloc(count, sizeof(*choices));
    unsigned int choices_count = 0;
    printf("Janelas disponiveis:\n");
    for (unsigned int i = 0; i < count; i++) {
        XWindowAttributes attrs;
        if (!XGetWindowAttributes(dpy, children[i], &attrs) ||
            attrs.map_state != IsViewable || attrs.override_redirect ||
            attrs.width < 64 || attrs.height < 64)
            continue;

        char *title = NULL;
        XFetchName(dpy, children[i], &title);
        printf("  [%u] 0x%08lx  %dx%d  %s\n", choices_count + 1,
               (unsigned long)children[i], attrs.width, attrs.height,
               title ? title : "(sem titulo)");
        if (title) XFree(title);
        choices[choices_count++] = children[i];
    }
    if (children) XFree(children);

    if (!choices_count) {
        fprintf(stderr, "[erro] nenhuma janela capturavel encontrada.\n");
        free(choices);
        XCloseDisplay(dpy);
        return 0;
    }

    printf("Escolha uma janela (1-%u, 0 cancela): ", choices_count);
    fflush(stdout);
    char line[32];
    unsigned long selected = 0;
    if (!fgets(line, sizeof(line), stdin) || sscanf(line, "%lu", &selected) != 1 ||
        selected == 0 || selected > choices_count) {
        fprintf(stderr, "[erro] selecao cancelada ou invalida.\n");
        free(choices);
        XCloseDisplay(dpy);
        return 0;
    }

    Window result = choices[selected - 1];
    free(choices);
    XCloseDisplay(dpy);
    return result;
}

bool capture_init_target(CaptureX11 *cap, Window target) {
    memset(cap, 0, sizeof(*cap));

    cap->dpy = XOpenDisplay(NULL);
    if (!cap->dpy) {
        XSetErrorHandler(cap_x_error);
        fprintf(stderr, "[erro] XOpenDisplay falhou. DISPLAY=%s\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(null)");
        return false;
    }

    cap->root = DefaultRootWindow(cap->dpy);
    cap->target = target ? target : cap->root;
    if (target) {
        Atom wm_state = XInternAtom(cap->dpy, "WM_STATE", False);
        Window client = find_client_window(cap->dpy, target, wm_state);
        if (client) cap->target = client;
    }

    int major, minor;
    Bool shm_ok = False;
    XShmQueryVersion(cap->dpy, &major, &minor, &shm_ok);
    if (!shm_ok) {
        fprintf(stderr, "[erro] MIT-SHM nao suportado.\n");
        return false;
    }

    XWindowAttributes attrs;
    if (!XGetWindowAttributes(cap->dpy, cap->target, &attrs)) {
        fprintf(stderr, "[erro] XGetWindowAttributes falhou.\n");
        return false;
    }
    cap->width  = attrs.width;
    cap->height = attrs.height;
    cap->depth  = attrs.depth;
    cap->visual = attrs.visual;

    printf("[cap] alvo=0x%08lx  %dx%d depth=%d\n",
           (unsigned long)cap->target, cap->width, cap->height, cap->depth);

    // Sem compositor, uma janela coberta nao tem pixels no framebuffer e
    // XShmGetImage devolve BadMatch. XCompositeRedirectWindow(Automatic)
    // faz o proprio servidor manter um pixmap offscreen atualizado, mesmo
    // com a janela ocluida. Nao precisa de compositor rodando.
    int ev, err;
    if (XCompositeQueryExtension(cap->dpy, &ev, &err)) {
        XCompositeRedirectWindow(cap->dpy, cap->target,
                                 CompositeRedirectAutomatic);
        cap->pixmap = XCompositeNameWindowPixmap(cap->dpy, cap->target);
        if (cap->pixmap) {
            cap->use_composite = true;
            printf("[cap] XComposite ativo, pixmap=0x%lx\n",
                   (unsigned long)cap->pixmap);
        } else {
            fprintf(stderr, "[cap] NameWindowPixmap falhou, usando janela direta\n");
        }
    } else {
        fprintf(stderr, "[cap] XComposite nao disponivel\n");
    }

    cap->img = XShmCreateImage(
        cap->dpy, cap->visual, cap->depth, ZPixmap,
        NULL, &cap->shminfo, cap->width, cap->height);
    if (!cap->img) {
        fprintf(stderr, "[erro] XShmCreateImage falhou.\n");
        return false;
    }

    size_t size = (size_t)cap->img->bytes_per_line * cap->img->height;
    cap->shminfo.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
    if (cap->shminfo.shmid < 0) { perror("[erro] shmget"); return false; }

    cap->shminfo.shmaddr = cap->img->data = shmat(cap->shminfo.shmid, NULL, 0);
    if (cap->shminfo.shmaddr == (char *)-1) { perror("[erro] shmat"); return false; }
    cap->shminfo.readOnly = False;

    if (!XShmAttach(cap->dpy, &cap->shminfo)) {
        fprintf(stderr, "[erro] XShmAttach falhou.\n");
        return false;
    }
    cap->shm_attached = true;
    XSync(cap->dpy, False);

    printf("[cap] buffer %.2f MB\n", (double)size / 1048576.0);
    return true;
}

bool capture_grab(CaptureX11 *cap) {
    if (!capture_grab_async(cap)) return false;
    return capture_grab_wait(cap);
}


bool capture_grab_async(CaptureX11 *cap) {
    // SDL reinstala o handler default do X durante SDL_CreateWindow /
    // SDL_Vulkan_CreateSurface. Reinstalamos o nosso a cada frame para
    // que BadMatch vire retorno False em vez de abortar o processo.
    XSetErrorHandler(cap_x_error);

    // XShmGetImage exige que o drawable tenha exatamente o mesmo
    // tamanho/depth/visual do XImage criado no init. Se a janela
    // redimensionou ou sumiu, avisamos e retornamos False.
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(cap->dpy, cap->target, &attrs)) {
        fprintf(stderr, "[cap] XGetWindowAttributes falhou antes do grab\n");
        return false;
    }
    if (attrs.map_state != IsViewable) {
        fprintf(stderr, "[cap] janela nao viewable (map_state=%d)\n",
                attrs.map_state);
        return false;
    }
    if (attrs.width != cap->width || attrs.height != cap->height) {
        fprintf(stderr, "[cap] janela redimensionou: %dx%d -> %dx%d\n",
                cap->width, cap->height, attrs.width, attrs.height);
        return false;
    }
    if (attrs.depth != cap->depth) {
        fprintf(stderr, "[cap] depth da janela mudou: %d -> %d\n",
                cap->depth, attrs.depth);
        return false;
    }

    Drawable src = cap->use_composite ? cap->pixmap : cap->target;
    return XShmGetImage(cap->dpy, src, cap->img, 0, 0, AllPlanes) == True;
}


bool capture_grab_wait(CaptureX11 *cap) {
    XEvent e;
    while (XPending(cap->dpy)) XNextEvent(cap->dpy, &e);
    return true;
}

void capture_shutdown(CaptureX11 *cap) {
    if (!cap->dpy) return;
    if (cap->shm_attached) XShmDetach(cap->dpy, &cap->shminfo);
    if (cap->img) XDestroyImage(cap->img);
    if (cap->shminfo.shmaddr) shmdt(cap->shminfo.shmaddr);
    if (cap->shminfo.shmid >= 0) shmctl(cap->shminfo.shmid, IPC_RMID, NULL);
    if (cap->pixmap) XFreePixmap(cap->dpy, cap->pixmap);
    XCloseDisplay(cap->dpy);
    memset(cap, 0, sizeof(*cap));
}