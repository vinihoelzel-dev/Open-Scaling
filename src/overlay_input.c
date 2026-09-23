#include "overlay_input.h"
#include <X11/Xlib.h>
#include <SDL2/SDL_syswm.h>
#include <X11/keysym.h>
#include <X11/extensions/shape.h>
#include <stdio.h>
#include <string.h>

static Display *s_dpy = NULL;
static Window   s_win = 0;

// Tabela pra traduzir SDL_Keycode -> KeySym do X11 (so o que a gente grava).
static KeySym sdl_to_xkeysym(int sdl)
{
    switch (sdl) {
    case SDLK_ESCAPE: return XK_Escape;
    case SDLK_F3:     return XK_F3;
    case SDLK_F4:     return XK_F4;
    case SDLK_PAUSE:  return XK_Pause;
    case SDLK_F11:    return XK_F11;
    case SDLK_TAB:    return XK_Tab;
    default:          return NoSymbol;
    }
}

bool overlay_input_set_through(SDL_Window *sdl_win)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(sdl_win, &info)) {
        fprintf(stderr, "[overlay] GetWindowWMInfo falhou: %s\n", SDL_GetError());
        return false;
    }
    if (info.subsystem != SDL_SYSWM_X11) {
        fprintf(stderr, "[overlay] nao esta em X11\n");
        return false;
    }
    s_dpy = info.info.x11.display;
    s_win = info.info.x11.window;

    // Regiao de input vazia = janela nao recebe nada. Todos os cliques/teclas
    // vao pra janela que estiver embaixo (o jogo).
    XShapeCombineRectangles(s_dpy, s_win, ShapeInput,
                            0, 0, NULL, 0, ShapeSet, Unsorted);
    XFlush(s_dpy);
    fprintf(stderr, "[overlay] janela agora e input-transparente (0x%lx)\n",
            (unsigned long)s_win);
    return true;
}

bool overlay_grab_key(int sdl_keycode)
{
    if (!s_dpy) return false;
    KeySym sym = sdl_to_xkeysym(sdl_keycode);
    if (sym == NoSymbol) return false;
    KeyCode code = XKeysymToKeycode(s_dpy, sym);
    if (!code) return false;

    Window root = DefaultRootWindow(s_dpy);
    // NumLock e CapsLock alteram o modifier mask que o X espera, entao grava
    // com todas as combinacoes relevantes de Lock/Mod2.
    unsigned int locks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    for (size_t i = 0; i < sizeof(locks)/sizeof(locks[0]); i++) {
        XGrabKey(s_dpy, code, locks[i], root, True,
                 GrabModeAsync, GrabModeAsync);
    }
    XFlush(s_dpy);
    return true;
}

int overlay_poll_hotkey(void)
{
    if (!s_dpy) return SDLK_UNKNOWN;
    while (XPending(s_dpy)) {
        XEvent e;
        XNextEvent(s_dpy, &e);
        if (e.type == KeyPress) {
            KeySym sym = XLookupKeysym(&e.xkey, 0);
            switch (sym) {
            case XK_Escape: return SDLK_ESCAPE;
            case XK_F3:     return SDLK_F3;
            case XK_F4:     return SDLK_F4;
            case XK_Pause:  return SDLK_PAUSE;
            case XK_F11:    return SDLK_F11;
            case XK_Tab:    return SDLK_TAB;
            default:        break;
            }
        }
    }
    return SDLK_UNKNOWN;
}

void overlay_release_all(void)
{
    if (s_dpy) {
        XUngrabKey(s_dpy, AnyKey, AnyModifier, DefaultRootWindow(s_dpy));
        XFlush(s_dpy);
    }
}