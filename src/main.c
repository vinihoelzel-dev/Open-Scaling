#include "capture_x11.h"
#include "timer.h"
#include "vk_triangle.h"
#include "vk_preview.h"
#include "vk_upscale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool parse_resolution(const char *text, uint32_t *width, uint32_t *height) {
    unsigned w, h;
    if (sscanf(text, "%ux%u", &w, &h) != 2 || !w || !h) return false;
    *width = w;
    *height = h;
    return true;
}

static bool parse_profile(const char *text, float *scale) {
    if (strcmp(text, "ultra") == 0)       { *scale = 1.3f; return true; }
    if (strcmp(text, "quality") == 0 || strcmp(text, "qualidade") == 0)
                                            { *scale = 1.5f; return true; }
    if (strcmp(text, "balanced") == 0 || strcmp(text, "balanceado") == 0)
                                            { *scale = 1.7f; return true; }
    if (strcmp(text, "performance") == 0 || strcmp(text, "desempenho") == 0)
                                            { *scale = 2.0f; return true; }
    return false;
}

static int cmd_fsr(int argc, char **argv) {
    Window target = 0;
    uint32_t out_w = 0, out_h = 0;
    uint32_t max_w = 1920, max_h = 1080;
    float scale = 1.5f; // quality

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--max") == 0) {
            if (++i == argc || !parse_resolution(argv[i], &max_w, &max_h)) {
                fprintf(stderr, "[erro] use --max WxH.\n");
                return 1;
            }
        } else if (parse_resolution(argv[i], &out_w, &out_h)) {
            if (i + 1 < argc && strcmp(argv[i + 1], "--max") != 0) {
                fprintf(stderr, "[erro] so uma resolucao de saida e permitida.\n");
                return 1;
            }
        } else if (parse_profile(argv[i], &scale)) {
            continue;
        } else if (!target) {
            char *end = NULL;
            unsigned long id = strtoul(argv[i], &end, 0);
            if (!id || !end || *end) {
                fprintf(stderr, "[erro] perfil ou ID de janela invalido: %s\n", argv[i]);
                return 1;
            }
            target = (Window)id;
        } else {
            fprintf(stderr, "[erro] argumento desconhecido: %s\n", argv[i]);
            return 1;
        }
    }
    if (!target) target = capture_select_window();
    if (!target) return 1;
    return vk_upscale_run(target, out_w, out_h, scale, max_w, max_h);
}

static void salvar_ppm(const char *path, CaptureX11 *cap) {
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror("fopen"); return; }
    fprintf(fp, "P6\n%d %d\n255\n", cap->width, cap->height);

    unsigned char *base = capture_data(cap);
    int stride = capture_stride(cap);
    for (int y = 0; y < cap->height; y++) {
        unsigned char *row = base + y * stride;
        for (int x = 0; x < cap->width; x++) {
            unsigned char *px = row + x * 4;
            fputc(px[2], fp);
            fputc(px[1], fp);
            fputc(px[0], fp);
        }
    }
    fclose(fp);
}

static int cmd_snap(int argc, char **argv) {
    const char *out = (argc > 0) ? argv[0] : "snap.ppm";
    CaptureX11 cap;
    if (!capture_init(&cap)) return 1;

    uint64_t t0 = timer_now_ns();
    if (!capture_grab(&cap)) {
        fprintf(stderr, "[erro] grab falhou.\n");
        capture_shutdown(&cap);
        return 1;
    }
    uint64_t t1 = timer_now_ns();
    printf("[cap] 1 frame em %.2f ms\n", timer_elapsed_ms(t0, t1));
    salvar_ppm(out, &cap);
    printf("[cap] salvo: %s\n", out);
    capture_shutdown(&cap);
    return 0;
}

static int cmd_bench(int argc, char **argv) {
    int segundos = (argc > 0) ? atoi(argv[0]) : 5;
    CaptureX11 cap;
    if (!capture_init(&cap)) return 1;

    for (int i = 0; i < 10; i++) capture_grab(&cap);

    uint64_t start = timer_now_ns();
    uint64_t end   = start + (uint64_t)segundos * 1000000000ULL;
    uint64_t frames = 0;
    uint64_t t_min = UINT64_MAX, t_max = 0;

    printf("[bench] Capturando por %d s...\n", segundos);
    uint64_t last = start;
    while (timer_now_ns() < end) {
        uint64_t t0 = timer_now_ns();
        if (!capture_grab(&cap)) break;
        uint64_t t1 = timer_now_ns();
        uint64_t dt = t1 - t0;
        if (dt < t_min) t_min = dt;
        if (dt > t_max) t_max = dt;
        frames++;
        if (t1 - last >= 1000000000ULL) {
            printf("[bench] %.1f FPS\n", (double)frames / ((t1 - start) / 1e9));
            last = t1;
        }
    }
    uint64_t total = timer_now_ns() - start;
    printf("\n=== Resultado ===\n");
    printf("Frames:        %lu\n", frames);
    printf("Tempo total:   %.3f s\n", total / 1e9);
    printf("FPS medio:     %.1f\n", (double)frames / (total / 1e9));
    printf("Lat. minima:   %.3f ms\n", t_min / 1e6);
    printf("Lat. maxima:   %.3f ms\n", t_max / 1e6);
    printf("Lat. media:    %.3f ms\n", (double)total / frames / 1e6);
    printf("Resolucao:     %dx%d\n", cap.width, cap.height);
    capture_shutdown(&cap);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Uso: %s <comando> [args]\n"
        "Comandos:\n"
        "  snap [arquivo.ppm]     captura 1 frame\n"
        "  bench [segundos]       benchmark de captura (padrao: 5s)\n"
        "  vk                     janela Vulkan + triangulo (teste)\n"
        "  preview                preview da tela via Vulkan\n"
        "  fsr [janela] [perfil] [WxH] [--max WxH]\n"
        "                         FSR 1; perfis: ultra (1.3x), quality (1.5x),\n"
        "                         balanced (1.7x), performance (2x).\n"
        "                         Teto padrao: 1920x1080 e resolucao da tela.\n",
        prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *cmd = argv[1];
    if (strcmp(cmd, "snap")    == 0) return cmd_snap(argc - 2, argv + 2);
    if (strcmp(cmd, "bench")   == 0) return cmd_bench(argc - 2, argv + 2);
    if (strcmp(cmd, "vk")      == 0) return vk_triangle_run();
    if (strcmp(cmd, "preview") == 0) return vk_preview_run();
    if (strcmp(cmd, "fsr")     == 0) return cmd_fsr(argc - 2, argv + 2);
    usage(argv[0]);
    return 1;
}
