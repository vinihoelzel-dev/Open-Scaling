// tools/inspect.c — lê um PPM e imprime pixels em posições-chave
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s arquivo.ppm\n", argv[0]); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 1; }

    char magic[3] = {0};
    int w, h, maxv;
    if (fscanf(fp, "%2s %d %d %d", magic, &w, &h, &maxv) != 4) {
        fprintf(stderr, "erro lendo header\n"); return 1;
    }
    fgetc(fp); // consome o \n
    if (magic[0] != 'P' || magic[1] != '6') {
        fprintf(stderr, "não é P6\n"); return 1;
    }

    printf("PPM %dx%d max=%d\n", w, h, maxv);

    int points[][2] = {
        {10, 10},
        {w/2, h/2},
        {w-10, h-10},
        {w/2, 5},
    };
    for (int i = 0; i < 4; i++) {
        int x = points[i][0], y = points[i][1];
        fseek(fp, 15L + (long)y * w * 3 + (long)x * 3, SEEK_SET);
        unsigned char rgb[3];
        if (fread(rgb, 1, 3, fp) != 3) break;
        printf("(%4d,%4d) -> R=%3d G=%3d B=%3d\n", x, y, rgb[0], rgb[1], rgb[2]);
    }
    fclose(fp);
    return 0;
}
