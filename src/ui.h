#ifndef OPENSCALING_UI_H
#define OPENSCALING_UI_H

#include <stdint.h>
#include <stdbool.h>

// Configuração escolhida na interface gráfica (estilo Lossless Scaling).
typedef struct {
    uint64_t window_id;   // 0 = primeira janela visível encontrada automaticamente
    int      scale_idx;   // índice interno dos botões segmentados de escala
    int      sharp_idx;   // índice interno dos botões segmentados de sharpness
    bool     vsync;
    bool     fullscreen;
    int      refresh_hz;  // ignorado quando vsync = true
} UiConfig;

// Converte os índices da UI em valores usados pelo pipeline FSR.
float ui_scale_factor(int scale_idx);   // 1.0 (sem upscale) .. 4.0
float ui_sharpness(int sharp_idx);      // 0.0 .. 1.0 (maior = menos suavização RCAS)

// Abre a janela de configuração, espera o usuário clicar em APPLY e retorna 0.
// Retorna -1 se o usuário fechar/cancelar sem aplicar.
int ui_config_run(UiConfig *out);

#endif // OPENSCALING_UI_H
