#ifndef OVERLAY_INPUT_H
#define OVERLAY_INPUT_H
#include <SDL2/SDL.h>
#include <stdbool.h>

// Torna a janela SDL invisivel ao input (cliques/teclas passam pra baixo).
bool overlay_input_set_through(SDL_Window *sdl_win);

// Registra um hotkey global (funciona sem foco na nossa janela).
bool overlay_grab_key(int sdl_keycode);

// Le um hotkey pressionado (nao bloqueante). Retorna SDLK_UNKNOWN se nada.
int overlay_poll_hotkey(void);

void overlay_release_all(void);

#endif