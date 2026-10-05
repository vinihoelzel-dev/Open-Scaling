// src/ui.c — Interface gráfica de configuração (estilo Lossless Scaling)
// Janela SDL2 + renderização imediata em software (pixels diretos no framebuffer).
// Sem dependências além de SDL2: funciona em qualquer máquina com X11.
#include "ui.h"
#include "capture_x11.h"

#include <SDL2/SDL.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define UI_W 560
#define UI_H 480

// ---------- Paleta (dark, estilo LS) ----------
typedef struct { uint32_t r, g, b; } Col;
static const Col C_BG      = { 24,  26,  32 };
static const Col C_PANEL   = { 37,  40,  49 };
static const Col C_PANEL2  = { 48,  52,  64 };
static const Col C_TEXT    = { 225, 228, 234 };
static const Col C_MUTED   = { 140, 146, 160 };
static const Col C_ACCENT  = { 0,   178, 120 };  // verde Open-Scaling
static const Col C_RED     = { 220,  70,  70 };
static const Col C_BORDER  = { 62,   66,  80 };

static uint32_t pack(Col c) { return (c.r << 16) | (c.g << 8) | c.b; }

// ---------- Framebuffer ----------
static uint32_t *fb;

static void px(int x, int y, Col c) {
    if ((unsigned)x < UI_W && (unsigned)y < UI_H) fb[y * UI_W + x] = pack(c);
}
static void rect(int x, int y, int w, int h, Col c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px(x + i, y + j, c);
}
static void rrect(int x, int y, int w, int h, int rad, Col c) {
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    for (int j = 0; j < h; j++) {
        int inset = 0;
        int dy = (j < rad) ? (rad - 1 - j) : (j >= h - rad ? j - (h - rad) : -1);
        if (dy >= 0) {
            int dx = rad - (int)sqrtf((float)(rad * rad - dy * dy) + 0.5f);
            if (dx < 0) dx = 0;
            inset = dx;
        }
        for (int i = inset; i < w - inset; i++) px(x + i, y + j, c);
    }
}

static void border(int x, int y, int w, int h, Col c) {
    for (int i = 0; i < w; i++) { px(x+i, y, c); px(x+i, y+h-1, c); }
    for (int j = 0; j < h; j++) { px(x, y+j, c); px(x+w-1, y+j, c); }
}

// ---------- Fonte bitmap embutida (5x7, ASCII 32..126) ----------
// Cada caractere: 7 linhas de bits em colunas de 5 bits (bit 4 = coluna mais à esquerda).
static const uint8_t FONT[95][7] = {
{0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x04,0x04,0x04,0x04,0x00,0x00,0x04},
{0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
{0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},{0x00,0x11,0x12,0x04,0x09,0x11,0x00},
{0x06,0x09,0x06,0x0B,0x0D,0x0B,0x06},{0x08,0x08,0x00,0x00,0x00,0x00,0x00},
{0x02,0x04,0x08,0x08,0x08,0x04,0x02},{0x08,0x04,0x02,0x02,0x02,0x04,0x08},
{0x00,0x04,0x15,0x0E,0x15,0x04,0x00},{0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
{0x00,0x00,0x00,0x00,0x00,0x04,0x08},{0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x04},{0x00,0x01,0x02,0x04,0x08,0x10,0x00},
{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x1F},
{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
{0x06,0x09,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x1C},
{0x00,0x04,0x04,0x00,0x04,0x04,0x00},{0x00,0x04,0x04,0x00,0x04,0x04,0x08},
{0x02,0x04,0x08,0x10,0x08,0x04,0x02},{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
{0x08,0x04,0x02,0x01,0x02,0x04,0x08},{0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
{0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},{0x04,0x0A,0x11,0x1F,0x11,0x11,0x11},
{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},{0x07,0x08,0x10,0x17,0x11,0x11,0x17},
{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
{0x07,0x02,0x02,0x02,0x02,0x12,0x0C},{0x10,0x12,0x14,0x18,0x1C,0x12,0x11},
{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},{0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
{0x11,0x19,0x19,0x15,0x13,0x13,0x11},{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},{0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},{0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
{0x1F,0x02,0x04,0x08,0x10,0x08,0x1F},{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
{0x10,0x08,0x04,0x02,0x01,0x02,0x04},{0x1F,0x01,0x01,0x01,0x01,0x01,0x01},
{0x04,0x0A,0x11,0x00,0x00,0x00,0x00},{0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
{0x08,0x04,0x02,0x00,0x00,0x00,0x00},{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
{0x10,0x10,0x16,0x19,0x11,0x11,0x1E},{0x00,0x00,0x0E,0x11,0x10,0x11,0x0E},
{0x01,0x01,0x0D,0x13,0x11,0x11,0x0F},{0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
{0x06,0x09,0x08,0x1C,0x08,0x08,0x08},{0x00,0x00,0x0F,0x11,0x11,0x0F,0x01},
{0x10,0x10,0x16,0x19,0x11,0x11,0x11},{0x04,0x00,0x04,0x04,0x04,0x04,0x0E},
{0x02,0x00,0x06,0x02,0x02,0x12,0x0C},{0x10,0x10,0x12,0x14,0x18,0x1C,0x12},
{0x04,0x04,0x04,0x04,0x04,0x04,0x0E},{0x00,0x00,0x1A,0x15,0x15,0x15,0x15},
{0x00,0x00,0x16,0x19,0x11,0x11,0x11},{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
{0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},{0x00,0x00,0x0E,0x11,0x11,0x0F,0x01},
{0x00,0x00,0x16,0x19,0x10,0x10,0x10},{0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},
{0x00,0x08,0x1C,0x08,0x08,0x09,0x06},{0x00,0x00,0x11,0x11,0x11,0x11,0x0F},
{0x00,0x00,0x11,0x11,0x11,0x0A,0x04},{0x00,0x00,0x11,0x15,0x15,0x1F,0x00},
{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},{0x00,0x00,0x11,0x11,0x0F,0x01,0x1E},
{0x00,0x00,0x1F,0x04,0x08,0x10,0x1F},{0x06,0x08,0x08,0x10,0x08,0x08,0x06},
{0x04,0x04,0x04,0x00,0x04,0x04,0x04},{0x0C,0x10,0x10,0x02,0x10,0x10,0x0C},
{0x00,0x00,0x05,0x0A,0x14,0x0A,0x05},
};

static void draw_char(int x, int y, char c, Col col, int scale) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = FONT[c - 32];
    for (int row = 0; row < 7; row++) {
        for (int bit = 0; bit < 5; bit++) {
            if (g[row] & (0x10 >> bit)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        px(x + bit*scale + sx, y + row*scale + sy, col);
            }
        }
    }
}
static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale - scale; }
static void text(int x, int y, const char *s, Col col, int scale) {
    for (int i = 0; s[i]; i++) {
        draw_char(x + i * 6 * scale, y, s[i], col, scale);
    }
}
static void text_center(int cx, int y, const char *s, Col col, int scale) {
    text(cx - text_w(s, scale) / 2, y, s, col, scale);
}

// Acentuação: desenha sem diacríticos quando necessário — usamos strings
// simples em PT sem acentos na UI para manter a fonte enxuta.

// ---------- Estado dos widgets ----------
typedef struct { int x, y, w, h; } Rect;
static bool in_rect(Rect r, int mx, int my) {
    return mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h;
}

// Lista de janelas X11
typedef struct { Window id; char title[80]; int w, h; } WinInfo;
static WinInfo wins[64];
static int win_count = 0;
static int win_scroll = 0;      // primeira janela visível na lista
#define WIN_VISIBLE 3           // linhas de janela exibidas

static void refresh_windows(void) {
    win_count = 0;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy), ret_root, parent, *children = NULL;
    unsigned int count = 0;
    if (XQueryTree(dpy, root, &ret_root, &parent, &children, &count)) {
        Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
        Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        for (unsigned int i = 0; i < count && win_count < 64; i++) {
            XWindowAttributes attrs;
            if (!XGetWindowAttributes(dpy, children[i], &attrs) ||
                attrs.map_state != IsViewable || attrs.override_redirect ||
                attrs.width < 100 || attrs.height < 100)
                continue;
            char *title = NULL;
            Atom type; int fmt; unsigned long nitems, bytes_after; unsigned char *data = NULL;
            if (XGetWindowProperty(dpy, children[i], net_wm_name, 0, 256, False, utf8,
                                   &type, &fmt, &nitems, &bytes_after, &data) == Success && data) {
                title = (char *)data;
            } else {
                XFetchName(dpy, children[i], &title);
            }
            WinInfo *wi = &wins[win_count++];
            wi->id = children[i];
            wi->w = attrs.width; wi->h = attrs.height;
            snprintf(wi->title, sizeof(wi->title), "%s", title && *title ? title : "(sem titulo)");
            if (data) XFree(data);
            else if (title) XFree(title);
        }
        if (children) XFree(children);
    }
    XCloseDisplay(dpy);
}

// ---------- Configuração viva ----------
static UiConfig cfg;
static int sel_win = 0;          // índice em wins[]

// IDs de widgets clicáveis
enum {
    W_NONE = 0,
    W_WIN_FIRST,       // "Auto (primeira janela)"
    W_WIN_BASE = 100,  // 100+i = janela i
    W_SCALE_BASE = 200,// 200+i = escala i
    W_SHARP_BASE = 300,
    W_VSYNC, W_FULLSCREEN, W_REFRESH_DOWN, W_REFRESH_UP,
    W_REFRESH_BASE = 400, // hit area do slider
    W_APPLY, W_CANCEL, W_REFRESH_LIST, W_WIN_SCROLL_UP, W_WIN_SCROLL_DOWN,
};

static const float SCALE_OPTS[]  = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
static const char *SCALE_LBL[]   = { "OFF", "1.5x", "2x", "3x", "4x" };
#define N_SCALE 5
static const char *SHARP_LBL[]   = { "OFF", "BAIXA", "MEDIA", "ALTA" };
static const float SHARP_VAL[]    = { 0.0f, 0.33f, 0.66f, 1.0f };
#define N_SHARP 4

static const int REFRESH_MIN = 30, REFRESH_MAX = 240;


static Rect hit_rects[96]; static int hit_ids[96]; static int hit_n;
static void add_hit(int id, Rect r) {
    if (hit_n < 96) { hit_ids[hit_n] = id; hit_rects[hit_n] = r; hit_n++; }
}

static void draw_switch(int x, int y, bool on, const char *label, int id) {
    Rect sw = { x, y, 44, 22 };
    rrect(sw.x, sw.y, sw.w, sw.h, 11, on ? C_ACCENT : C_PANEL2);
    border(sw.x, sw.y, sw.w, sw.h, on ? C_ACCENT : C_BORDER);
    int knob_cx = on ? (sw.x + sw.w - 12) : (sw.x + 12);
    int knob_cy = sw.y + 11;
    for (int j = -9; j <= 9; j++)
        for (int i = -9; i <= 9; i++)
            if (i*i + j*j <= 81) px(knob_cx + i, knob_cy + j, (Col){240,240,240});
    text(x + 54, y + 4, label, C_TEXT, 2);
    add_hit(id, (Rect){ x - 4, y - 4, 60 + (int)strlen(label) * 12 + 8, 30 });
}

static void draw_segmented(int x, int y, const char **labels, int n, int sel, int seg_w, int id_base) {
    Rect box = { x, y, seg_w * n, 30 };
    rrect(box.x, box.y, box.w, box.h, 6, C_PANEL2);
    // fundo da fatia selecionada primeiro, depois rótulos por cima
    if (sel >= 0 && sel < n) {
        Rect r = { x + sel * seg_w, y, seg_w, 30 };
        rrect(r.x, r.y, r.w, r.h, 6, C_ACCENT);
        if (sel != 0)     rect(r.x, r.y, 7, r.h, C_ACCENT);
        if (sel != n - 1) rect(r.x + r.w - 7, r.y, 7, r.h, C_ACCENT);
    }
    for (int i = 0; i < n; i++) {
        Rect r = { x + i * seg_w, y, seg_w, 30 };
        text_center(r.x + r.w / 2, r.y + 8, labels[i], i == sel ? (Col){255,255,255} : C_MUTED, 2);
        if (i != n - 1 && i != sel && i + 1 != sel)
            for (int j = 4; j < 26; j++) px(r.x + r.w, r.y + j, C_BORDER);
        add_hit(id_base + i, r);
    }
    border(box.x, box.y, box.w, box.h, C_BORDER);
}

static void draw_slider(int x, int y, int w, int val_0_100, int id, const char *fmt_left, const char *fmt_right) {
    // trilha
    Rect track = { x, y + 8, w, 6 };
    rrect(track.x, track.y, track.w, track.h, 3, C_PANEL2);
    int fill = (int)((w - 16) * (val_0_100 / 100.0f)) + 8;
    for (int j = 0; j < 6; j++)
        for (int i = 0; i < fill - 8; i++) px(x + 8 + i, y + 8 + j, C_ACCENT);
    // thumb
    int tx = x + fill;
    for (int j = -9; j <= 9; j++)
        for (int i = -9; i <= 9; i++)
            if (i*i + j*j <= 81) px(tx + i, y + 11 + j, (Col){235,238,242});
    Rect hitr = { x - 10, y - 10, w + 20, 42 };
    add_hit(id, hitr);
    (void)fmt_left; (void)fmt_right;
}

// ---------- Desenho principal ----------
static void draw_ui(int mx, int my) {
    hit_n = 0;
    rect(0, 0, UI_W, UI_H, C_BG);

    // Header
    rect(0, 0, UI_W, 56, C_PANEL);
    // logo: monitorzinho
    rrect(18, 14, 28, 20, 4, C_ACCENT);
    rect(21, 17, 22, 14, C_BG);
    rect(28, 34, 8, 4, C_ACCENT);
    rect(22, 38, 20, 3, C_ACCENT);
    text(58, 12, "OPEN SCALING", C_TEXT, 3);
    text(58, 36, "Upscaler FSR 1.0  -  X11 / Vulkan", C_MUTED, 1);

    int y = 68;

    // ---- Seção: Janela alvo ----
    text(20, y, "JANELA DO JOGO", C_MUTED, 2); y += 24;
    Rect rf = { UI_W - 130, y - 2, 110, 26 };
    rrect(rf.x, rf.y, rf.w, rf.h, 6, C_PANEL2);
    border(rf.x, rf.y, rf.w, rf.h, C_BORDER);
    text_center(rf.x + rf.w/2, rf.y + 6, "ATUALIZAR", C_TEXT, 1);
    add_hit(W_REFRESH_LIST, rf);

    Rect auto_r = { 20, y, UI_W - 40 - 130, 26 };
    bool auto_sel = (sel_win < 0);
    rrect(auto_r.x, auto_r.y, auto_r.w, auto_r.h, 6, auto_sel ? C_ACCENT : C_PANEL);
    border(auto_r.x, auto_r.y, auto_r.w, auto_r.h, auto_sel ? C_ACCENT : C_BORDER);
    text(auto_r.x + 10, auto_r.y + 6, "AUTO - primeira janela encontrada", auto_sel ? (Col){255,255,255} : C_MUTED, 1);
    add_hit(W_WIN_FIRST, auto_r);
    y += 32;

    if (win_scroll + WIN_VISIBLE > win_count) win_scroll = win_count > WIN_VISIBLE ? win_count - WIN_VISIBLE : 0;
    for (int k = 0; k < WIN_VISIBLE && win_scroll + k < win_count; k++) {
        int i = win_scroll + k;
        WinInfo *wi = &wins[i];
        char line[96];
        snprintf(line, sizeof(line), "%.38s  %dx%d", wi->title, wi->w, wi->h);
        Rect r = { 20, y, UI_W - 40, 26 };
        bool s = (sel_win == i);
        rrect(r.x, r.y, r.w, r.h, 6, s ? C_ACCENT : C_PANEL);
        border(r.x, r.y, r.w, r.h, s ? C_ACCENT : C_BORDER);
        text(r.x + 10, r.y + 6, line, s ? (Col){255,255,255} : C_TEXT, 1);
        char hexid[24]; snprintf(hexid, sizeof(hexid), "0x%lx", (unsigned long)wi->id);
        text(r.x + r.w - (int)strlen(hexid) * 6 - 10, r.y + 6, hexid, s ? (Col){220,255,235} : C_MUTED, 1);
        add_hit(W_WIN_BASE + i, r);
        y += 30;
    }
    if (win_count == 0) {
        text(20, y, "nenhuma janela encontrada - clique ATUALIZAR", C_RED, 1);
        y += 20;
    }
    if (win_count > WIN_VISIBLE) {
        char pos[48];
        snprintf(pos, sizeof(pos), "%d-%d de %d   [-] role [+]",
                 win_scroll + 1,
                 win_scroll + WIN_VISIBLE < win_count ? win_scroll + WIN_VISIBLE : win_count,
                 win_count);
        text(20, y, pos, C_MUTED, 1);
        Rect dn = { 150, y - 4, 26, 20 }, up = { 230, y - 4, 26, 20 };
        rrect(dn.x, dn.y, dn.w, dn.h, 4, C_PANEL2); border(dn.x, dn.y, dn.w, dn.h, C_BORDER);
        text_center(dn.x + dn.w/2, dn.y + 7, "-", C_TEXT, 1);
        rrect(up.x, up.y, up.w, up.h, 4, C_PANEL2); border(up.x, up.y, up.w, up.h, C_BORDER);
        text_center(up.x + up.w/2, up.y + 7, "+", C_TEXT, 1);
        add_hit(W_WIN_SCROLL_DOWN, dn);
        add_hit(W_WIN_SCROLL_UP, up);
        y += 22;
    }
    y += 6;

    // ---- Seção: Escala (Hangovers como LS: "Scale") ----
    text(20, y, "ESCALA (FSR)", C_MUTED, 2);
    char cur[32]; snprintf(cur, sizeof(cur), "%s", SCALE_LBL[cfg.scale_idx]);
    text(UI_W - 20 - text_w(cur, 2), y, cur, C_ACCENT, 2);
    y += 26;
    draw_segmented(20, y, SCALE_LBL, N_SCALE, cfg.scale_idx, (UI_W - 40) / N_SCALE, W_SCALE_BASE);
    y += 44;

    // ---- Sharpness ----
    text(20, y, "SHARPNESS (RCAS)", C_MUTED, 2);
    y += 26;
    draw_segmented(20, y, SHARP_LBL, N_SHARP, cfg.sharp_idx, (UI_W - 40) / N_SHARP, W_SHARP_BASE);
    y += 44;

    // ---- Refresh rate ----
    text(20, y, "TAXA DE ATUALIZACAO", C_MUTED, 2);
    char hz[32];
    if (cfg.vsync) snprintf(hz, sizeof(hz), "SYNC");
    else snprintf(hz, sizeof(hz), "%d HZ", cfg.refresh_hz);
    text(UI_W - 20 - text_w(hz, 2), y, hz, cfg.vsync ? C_ACCENT : C_TEXT, 2);
    y += 26;
    int pct = cfg.vsync ? 100 : (cfg.refresh_hz - REFRESH_MIN) * 100 / (REFRESH_MAX - REFRESH_MIN);
    draw_slider(20, y, UI_W - 40, pct, W_REFRESH_BASE, NULL, NULL);
    y += 40;

    // ---- Switches ----
    draw_switch(20, y, cfg.vsync, "V-SYNC", W_VSYNC);
    draw_switch(UI_W / 2 + 10, y, cfg.fullscreen, "TELA CHEIA", W_FULLSCREEN);
    y += 44;

    // ---- Botões ----
    Rect ap = { 20, y, UI_W - 40, 46 };
    rrect(ap.x, ap.y, ap.w, ap.h, 8, C_ACCENT);
    text_center(ap.x + ap.w / 2, ap.y + 15, "APLICAR  (ENTER)", (Col){255,255,255}, 2);
    add_hit(W_APPLY, ap);

    // cursor "pointer" sobre widgets clicaveis
    bool over_widget = false;
    for (int i = 0; i < hit_n; i++) {
        if (in_rect(hit_rects[i], mx, my)) { over_widget = true; break; }
    }
        static SDL_Cursor *cur_hand = NULL, *cur_arrow = NULL;
    if (!cur_hand)  cur_hand  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    if (!cur_arrow) cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    SDL_SetCursor(over_widget ? cur_hand : cur_arrow);
}

// ---------- Interação ----------
static void click(int id, int mx, int my) {
    if (id >= W_WIN_BASE && id < W_WIN_BASE + 64) sel_win = id - W_WIN_BASE;
    else if (id == W_WIN_FIRST) sel_win = -1;
    else if (id >= W_SCALE_BASE && id < W_SCALE_BASE + N_SCALE) cfg.scale_idx = id - W_SCALE_BASE;
    else if (id >= W_SHARP_BASE && id < W_SHARP_BASE + N_SHARP) cfg.sharp_idx = id - W_SHARP_BASE;
    else if (id == W_VSYNC) cfg.vsync = !cfg.vsync;
    else if (id == W_FULLSCREEN) cfg.fullscreen = !cfg.fullscreen;
    else if (id == W_REFRESH_LIST) { refresh_windows(); if (sel_win >= win_count) sel_win = -1; win_scroll = 0; }
    else if (id == W_WIN_SCROLL_UP)   { if (win_scroll + WIN_VISIBLE < win_count) win_scroll++; }
    else if (id == W_WIN_SCROLL_DOWN) { if (win_scroll > 0) win_scroll--; }
    else if (id >= W_REFRESH_BASE && id < W_REFRESH_BASE + 8) {
        // arraste do slider tratado em motion; clique posiciona
        int x0 = 20, w = UI_W - 40;
        int rel = mx - (x0 + 8);
        if (rel < 0) rel = 0;
        if (rel > w - 16) rel = w - 16;
        cfg.vsync = false;
        cfg.refresh_hz = REFRESH_MIN + rel * (REFRESH_MAX - REFRESH_MIN) / (w - 16);
    }
}


// ---------- Conversão UI -> pipeline ----------
float ui_scale_factor(int scale_idx) {
    if (scale_idx < 0) scale_idx = 0;
    if (scale_idx >= N_SCALE) scale_idx = N_SCALE - 1;
    return SCALE_OPTS[scale_idx];
}

float ui_sharpness(int sharp_idx) {
    if (sharp_idx < 0) sharp_idx = 0;
    if (sharp_idx >= N_SHARP) sharp_idx = N_SHARP - 1;
    // SHARP_VAL: 0 = OFF (sem RCAS), maior = mais nitido.
    // O pipeline usa "suavizacao" no estilo FidelityFX: exp2(-2*sharpness).
    return SHARP_VAL[sharp_idx];
}

int ui_config_run(UiConfig *out) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.scale_idx = 1;   // 1.5x default
    cfg.sharp_idx = 2;   // media
    cfg.vsync = true;
    cfg.refresh_hz = 60;
    sel_win = -1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[ui] SDL: %s\n", SDL_GetError());
        return -1;
    }
    SDL_Window *win = SDL_CreateWindow("Open Scaling",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, UI_W, UI_H, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, UI_W, UI_H);
    fb = malloc(sizeof(uint32_t) * UI_W * UI_H);

    refresh_windows();

    bool run = true; int result = -1;
    int mx = 0, my = 0;
    bool dragging_slider = false;

    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: run = false; break;
            case SDL_MOUSEMOTION: mx = e.motion.x; my = e.motion.y;
                if (dragging_slider) click(W_REFRESH_BASE, mx, my);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    mx = e.button.x; my = e.button.y;
                    // resolve hit no frame atual
                    for (int i = 0; i < hit_n; i++) {
                        if (in_rect(hit_rects[i], mx, my)) {
                            int hit_id = hit_ids[i];
                            if (hit_id >= W_REFRESH_BASE) dragging_slider = true;
                            click(hit_id, mx, my);
                            break;
                        }
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP: dragging_slider = false; break;
            case SDL_KEYDOWN: {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { run = false; result = 0; }
                else if (k == SDLK_ESCAPE) { run = false; result = -1; }
                else if (k == SDLK_r) { refresh_windows(); win_scroll = 0; }
                else if (k == SDLK_PAGEDOWN) { if (win_scroll + WIN_VISIBLE < win_count) win_scroll++; }
                else if (k == SDLK_PAGEUP)   { if (win_scroll > 0) win_scroll--; }
                else if (k == SDLK_v) { cfg.vsync = !cfg.vsync; }
                else if (k == SDLK_f) { cfg.fullscreen = !cfg.fullscreen; }
                else if (k == SDLK_LEFT)  { if (cfg.scale_idx > 0) cfg.scale_idx--; }
                else if (k == SDLK_RIGHT) { if (cfg.scale_idx < N_SCALE - 1) cfg.scale_idx++; }
                else if (k == SDLK_UP)    { if (!cfg.vsync && cfg.refresh_hz < REFRESH_MAX) cfg.refresh_hz++; }
                else if (k == SDLK_DOWN)  { if (!cfg.vsync && cfg.refresh_hz > REFRESH_MIN) cfg.refresh_hz--; }
                break;
            }
            }
        }

        draw_ui(mx, my);
        SDL_UpdateTexture(tex, NULL, fb, UI_W * sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    if (result == 0) {
        // monta a saída
        if (sel_win >= 0 && sel_win < win_count) out->window_id = (uint64_t)wins[sel_win].id;
        else out->window_id = 0;
        out->scale_idx = cfg.scale_idx;
        out->sharp_idx = cfg.sharp_idx;
        out->vsync = cfg.vsync;
        out->fullscreen = cfg.fullscreen;
        out->refresh_hz = cfg.vsync ? 0 : cfg.refresh_hz;
    }

    free(fb);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return result;
}
