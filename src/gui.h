// src/gui.h — Launcher grafico (Dear ImGui) que replica o menu do start.sh
#ifndef OS_GUI_H
#define OS_GUI_H

#ifdef __cplusplus
extern "C" {
#endif

// Abre a janela do launcher (SDL2 + Vulkan + Dear ImGui). Bloqueia ate a
// janela ser fechada. Retorna 0 em sucesso.
int vk_gui_run(void);

#ifdef __cplusplus
}
#endif

#endif // OS_GUI_H
