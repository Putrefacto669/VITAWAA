#include "ui.h"
#include "tex.h"
#include "touch.h"
#include "net.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>

vita2d_font *F_REG = NULL, *F_BOLD = NULL, *F_XB = NULL;

unsigned int C_ACC, C_ACC_SOFT, C_MINE, C_MINE_SUB, C_MINE_Q;
unsigned int TH[T_COUNT];
int ui_dark = 0;
static int cur_accent = 0;

static const unsigned int ACC[ACCENT_COUNT] = {0x1E6FD9, 0x1F9D63, 0xE07A1F, 0x7B4FD6};

void ui_set_accent(int idx) {
    // claro: burbujas pastel · noche: burbujas profundas con texto claro
    static const unsigned int soft[2][ACCENT_COUNT] = {{0xE6F0FD, 0xE3F5EB, 0xFDEEDF, 0xEEE8FB}, {0x173A66, 0x14452E, 0x4A3016, 0x2F2557}};
    static const unsigned int mine[2][ACCENT_COUNT] = {{0xD6E8FF, 0xD4F0E1, 0xFCE3CC, 0xE6DCFA}, {0x1D4E8F, 0x16613F, 0x7A4516, 0x4B3591}};
    static const unsigned int msub[2][ACCENT_COUNT] = {{0x4C6A8C, 0x3C7357, 0x8A5A2E, 0x5E4A8A}, {0xBBD5F5, 0xB5E6CB, 0xF7D3AE, 0xD6C8F7}};
    static const unsigned int mq[2][ACCENT_COUNT]   = {{0xC2DBFA, 0xBFE6D0, 0xF7D0AE, 0xD6C7F5}, {0x2A5FA3, 0x237A50, 0x8F5A28, 0x5E45A8}};
    if (idx < 0 || idx >= ACCENT_COUNT) idx = 0;
    cur_accent = idx;
    int d = ui_dark ? 1 : 0;
    C_ACC = HEX(ACC[idx]);
    C_ACC_SOFT = HEX(soft[d][idx]);
    C_MINE = HEX(mine[d][idx]);
    C_MINE_SUB = HEX(msub[d][idx]);
    C_MINE_Q = HEX(mq[d][idx]);
}

void ui_set_theme(int dark) {
    // 0 = Aero claro (cielo, vidrio blanco) · 1 = Aero noche (azul marino → verde botella)
    static const unsigned int t[2][T_COUNT] = {
        { 0xEEF2F6, 0xFFFFFF, 0xFFFFFF, 0xF7F9FB, 0xD6DEE7, 0xE6EBF1, 0xEEF2F6, 0xE4EAF1, 0xE1E7EE, 0xFFFFFF,
          0xDCE3EB, 0xDCE3EB, 0x1E2A36, 0x1E2A36, 0x5B6876, 0x3D4A57, 0x6F7C89, 0xFFF4E0, 0x8A5216,
          0xE4F1FF, 0xDDF2EC, 0xF4F9FF, 0xFFFFFF },
        { 0x0E2240, 0x12284A, 0x15294A, 0x112440, 0x2A4468, 0x1F3556, 0x1A2F52, 0x223A5E, 0x1C3254, 0x18304F,
          0x2A4468, 0x223A5E, 0x000000, 0xEAF2FB, 0xA9BBD0, 0xC9D6E4, 0x8A9DB4, 0x3A2C14, 0xF5C77E,
          0x0A1F44, 0x06332A, 0x1B3963, 0xFFFFFF },
    };
    ui_dark = dark ? 1 : 0;
    for (int i = 0; i < T_COUNT; i++) TH[i] = HEX(t[ui_dark][i]);
    TH[T_DIM] = ui_dark ? HEXA(0x000000, 0x99) : HEXA(0x1E2A36, 0x66);
    ui_set_accent(cur_accent);
}

unsigned int accent_preview(int idx) { return HEX(ACC[(idx % ACCENT_COUNT + ACCENT_COUNT) % ACCENT_COUNT]); }

int ui_init(void) {
    ui_set_theme(0);
    ui_set_accent(0);
    F_REG = vita2d_load_font_file("app0:res/fonts/MPLUS1p-Regular.ttf");
    F_BOLD = vita2d_load_font_file("app0:res/fonts/MPLUS1p-Bold.ttf");
    F_XB = vita2d_load_font_file("app0:res/fonts/MPLUS1p-ExtraBold.ttf");
    if (!F_BOLD) F_BOLD = F_REG;
    if (!F_XB) F_XB = F_BOLD;
    return F_REG != NULL;
}

void ui_term(void) {
    if (F_XB && F_XB != F_BOLD) vita2d_free_font(F_XB);
    if (F_BOLD && F_BOLD != F_REG) vita2d_free_font(F_BOLD);
    if (F_REG) vita2d_free_font(F_REG);
}

// nombres de remitentes en grupos (legibles sobre blanco)
unsigned int name_color(int idx) {
    static const unsigned int c[8] = {0xC2410C, 0x1E6FD9, 0xBE2D7A, 0x15803D, 0x7B4FD6, 0xA16207, 0xDC2626, 0x0E7490};
    return HEX(c[(idx % 8 + 8) % 8]);
}
unsigned int pastel_bg(int idx) {
    static const unsigned int c[8] = {0xF6D7E0, 0xDCEBFA, 0xD4F0E1, 0xE2DAF5, 0xF9E3C8, 0xE3EED0, 0xD3ECEF, 0xF3DCEF};
    return HEX(c[(idx % 8 + 8) % 8]);
}
unsigned int pastel_fg(int idx) {
    static const unsigned int c[8] = {0x8A2448, 0x1E5A94, 0x1F6B45, 0x5B3E9A, 0x8A5216, 0x4F6B1E, 0x1F6670, 0x7A2E6B};
    return HEX(c[(idx % 8 + 8) % 8]);
}

int hash_color(const char *s) {
    unsigned int h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return (int)(h % 8);
}

// ---------- formas ----------
// ---------- figuras con pocos vertices ----------
// vita2d_draw_fill_circle usa 100 segmentos (~1.8 KB del pool por circulo). Con muchas
// burbujas y anillos se llenaba el pool de vita2d y la app se caia. Aqui todo se dibuja
// con abanicos/tiras de triangulos de pocos segmentos y se revisa que haya espacio.
static vita2d_color_vertex *verts(int n) {
    if (vita2d_pool_free_space() < (unsigned int)(n * sizeof(vita2d_color_vertex) + 1024)) return NULL;
    return (vita2d_color_vertex *)vita2d_pool_memalign(n * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
}
static inline void vset(vita2d_color_vertex *v, float x, float y, unsigned int c) {
    v->x = x; v->y = y; v->z = 0.5f; v->color = c;
}
static int segs_for(float r) {
    int n = (int)(r * 0.8f) + 8;
    return n < 8 ? 8 : n > 48 ? 48 : n;
}

void rrect(float x, float y, float w, float h, float r, unsigned int c) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1.5f) {
        vita2d_draw_rectangle(x, y, w, h, c);
        return;
    }
    // un solo poligono convexo: centro + contorno con las 4 esquinas redondeadas
    int k = segs_for(r) / 4 + 2;                 // segmentos por esquina
    int n = 4 * (k + 1) + 2;
    vita2d_color_vertex *v = verts(n);
    if (!v) return;
    vset(&v[0], x + w / 2, y + h / 2, c);
    const float cxs[4] = {x + w - r, x + r, x + r, x + w - r};
    const float cys[4] = {y + r, y + r, y + h - r, y + h - r};
    int i = 1;
    for (int q = 0; q < 4; q++) {
        float a0 = -1.5707963f * (q + 1) + 1.5707963f;   // 0, -90, -180, -270 grados
        for (int j = 0; j <= k; j++) {
            float a = a0 - 1.5707963f * j / k;
            vset(&v[i++], cxs[q] + cosf(a) * r, cys[q] + sinf(a) * r, c);
        }
    }
    v[i] = v[1];
    i++;
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, i);
}

void card(float x, float y, float w, float h, float r, unsigned int fill, unsigned int border) {
    rrect(x, y, w, h, r, border);
    rrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, fill);
    if (h >= 30 && w >= 60) gloss(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0);
}

void focus_card(float x, float y, float w, float h, float r) {
    // halo suave + borde de acento + relleno + brillo de vidrio
    unsigned int glow = (C_ACC & 0x00FFFFFF) | 0x22000000u;
    rrect(x - 4, y - 3, w + 8, h + 7, r + 4, glow);
    rrect(x, y, w, h, r, C_ACC);
    rrect(x + 2, y + 2, w - 4, h - 4, r > 2 ? r - 2 : 0, C_ACC_SOFT);
    gloss(x + 2, y + 2, w - 4, h - 4, r > 2 ? r - 2 : 0);
}

void shadow(float x, float y, float w, float h, float r) {
    unsigned int base = ui_dark ? 0x000000 : 0x1E2A36;
    rrect(x - 3, y + 3, w + 6, h + 8, r + 4, HEXA(base, ui_dark ? 0x30 : 0x0C));
    rrect(x - 2, y + 2, w + 4, h + 5, r + 3, HEXA(base, ui_dark ? 0x38 : 0x10));
    rrect(x - 1, y + 1, w + 2, h + 3, r + 1, HEXA(base, ui_dark ? 0x40 : 0x14));
}

// ---------- Aero: degradados y vidrio ----------
static unsigned int lerpc(unsigned int a, unsigned int b, float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    unsigned int o = 0;
    for (int sft = 0; sft < 32; sft += 8) {
        float ca = (a >> sft) & 0xFF, cb = (b >> sft) & 0xFF;
        o |= ((unsigned int)(ca + (cb - ca) * t) & 0xFF) << sft;
    }
    return o;
}

void grad_rect(float x, float y, float w, float h, unsigned int top, unsigned int bot) {
    if (w <= 0 || h <= 0) return;
    vita2d_color_vertex *v = verts(4);
    if (!v) return;
    vset(&v[0], x, y, top); vset(&v[1], x + w, y, top);
    vset(&v[2], x, y + h, bot); vset(&v[3], x + w, y + h, bot);
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void rrect_grad(float x, float y, float w, float h, float r, unsigned int top, unsigned int bot) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1.5f) { grad_rect(x, y, w, h, top, bot); return; }
    int k = segs_for(r) / 4 + 2;
    int n = 4 * (k + 1) + 2;
    vita2d_color_vertex *v = verts(n);
    if (!v) return;
    vset(&v[0], x + w / 2, y + h / 2, lerpc(top, bot, 0.5f));
    const float cxs[4] = {x + w - r, x + r, x + r, x + w - r};
    const float cys[4] = {y + r, y + r, y + h - r, y + h - r};
    int i = 1;
    for (int q = 0; q < 4; q++) {
        float a0 = -1.5707963f * (q + 1) + 1.5707963f;
        for (int j = 0; j <= k; j++) {
            float a = a0 - 1.5707963f * j / k;
            float px = cxs[q] + cosf(a) * r, py = cys[q] + sinf(a) * r;
            vset(&v[i++], px, py, lerpc(top, bot, (py - y) / h));
        }
    }
    v[i] = v[1];
    i++;
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, i);
}

// El sello Frutiger Aero: un reflejo blanco que se desvanece en la mitad de arriba
void gloss(float x, float y, float w, float h, float r) {
    if (w < 8 || h < 8) return;
    float gh = h * 0.48f;
    if (gh > 34) gh = 34;
    int a = ui_dark ? 0x24 : 0x9A;
    rrect_grad(x + 2, y + 1, w - 4, gh, r > 3 ? r - 2 : r, HEXA(0xFFFFFF, a), HEXA(0xFFFFFF, 0));
}

// Fondo: degradado + burbujas que flotan + onda de luz (como el wallpaper de la LiveArea)
void ui_background(int fr) {
    float t = fr / 60.0f;
    grad_rect(0, 0, SCREEN_W, SCREEN_H, TH[T_BG_TOP], TH[T_BG_BOT]);
    for (int i = 0; i < 6; i++) {
        float bx = fmodf(150.0f * i + t * (5 + i * 2), SCREEN_W + 300) - 150;
        float by = 430 - fmodf(t * (4 + i) + i * 97, 540) + sinf(t * 0.4f + i) * 12;
        float br = 50 + (i * 37) % 80;
        unsigned int c = ui_dark ? (i % 2 ? 0x6FE3B0 : 0x6FB8FF) : (i % 2 ? 0x9FE8CF : 0xA9D2FF);
        circle(bx, by, br, HEXA(c, ui_dark ? 0x10 : 0x2A));
        circle(bx - br * 0.3f, by - br * 0.35f, br * 0.33f, HEXA(0xFFFFFF, ui_dark ? 0x0A : 0x30));
    }
    vita2d_color_vertex *v = verts(48);
    if (v) {
        for (int i = 0; i < 24; i++) {
            float x = i * (SCREEN_W / 23.0f);
            float y = 300 + sinf(x * 0.008f + t * 0.35f) * 40 - x * 0.12f;
            vset(&v[i * 2], x, y, HEXA(0xFFFFFF, 0));
            vset(&v[i * 2 + 1], x, y + 70 + sinf(x * 0.01f - t * 0.5f) * 18, HEXA(ui_dark ? 0xBFFFE8 : 0xFFFFFF, ui_dark ? 0x16 : 0x55));
        }
        vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 48);
    }
}

void typing_dots(float x, float cy, unsigned int c) {
    extern int frame;
    for (int i = 0; i < 3; i++) {
        float k = sinf(frame * 0.12f - i * 0.9f);
        float yy = cy - (k > 0 ? k * 3.5f : 0);
        circle(x + i * 8, yy, 2.6f, (c & 0x00FFFFFF) | ((unsigned int)(150 + (k > 0 ? k * 105 : 0)) << 24));
    }
}

void circle(float cx, float cy, float r, unsigned int c) {
    if (r <= 0) return;
    int n = segs_for(r);
    vita2d_color_vertex *v = verts(n + 2);
    if (!v) return;
    vset(&v[0], cx, cy, c);
    for (int i = 0; i <= n; i++) {
        float a = 6.2831853f * i / n;
        vset(&v[i + 1], cx + cosf(a) * r, cy + sinf(a) * r, c);
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, n + 2);
}

void ring(float cx, float cy, float r, float t, unsigned int c, unsigned int inner) {
    circle(cx, cy, r, c);
    circle(cx, cy, r - t, inner);
}

void thick_line(float x0, float y0, float x1, float y1, float t, unsigned int c) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    vita2d_color_vertex *v = verts(4);
    if (!v) return;
    vset(&v[0], x0 + nx, y0 + ny, c);
    vset(&v[1], x0 - nx, y0 - ny, c);
    vset(&v[2], x1 + nx, y1 + ny, c);
    vset(&v[3], x1 - nx, y1 - ny, c);
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
    if (t >= 2) {
        circle(x0, y0, t / 2, c);
        circle(x1, y1, t / 2, c);
    }
}

void fill_tri(float x0, float y0, float x1, float y1, float x2, float y2, unsigned int c) {
    vita2d_color_vertex *v = verts(3);
    if (!v) return;
    vset(&v[0], x0, y0, c);
    vset(&v[1], x1, y1, c);
    vset(&v[2], x2, y2, c);
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
}

void arc_dots(float cx, float cy, float r, float a0, float a1, float t, unsigned int c) {
    // arco grueso como una tira de triangulos (antes eran decenas de circulitos)
    float span = a1 - a0;
    if (span <= 0) return;
    int n = (int)(span * r / 4) + 2;
    if (n > 96) n = 96;
    vita2d_color_vertex *v = verts(2 * (n + 1));
    if (!v) return;
    float ri = r - t / 2, ro = r + t / 2;
    for (int i = 0; i <= n; i++) {
        float a = a0 + span * i / n, ca = cosf(a), sa = sinf(a);
        vset(&v[2 * i], cx + ca * ro, cy + sa * ro, c);
        vset(&v[2 * i + 1], cx + ca * ri, cy + sa * ri, c);
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 2 * (n + 1));
    if (span < 6.2f && t >= 2) {   // puntas redondas
        circle(cx + cosf(a0) * r, cy + sinf(a0) * r, t / 2, c);
        circle(cx + cosf(a1) * r, cy + sinf(a1) * r, t / 2, c);
    }
}

void spinner(float cx, float cy, int frame) {
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f - 1.5708f;
        int age = ((frame / 6) - i) & 7;
        unsigned int col = (C_ACC & 0x00FFFFFF) | ((unsigned int)(255 - age * 28) << 24);
        circle(cx + cosf(a) * 20, cy + sinf(a) * 20, 4.5f, col);
    }
}

// ---------- texto ----------
static vita2d_texture *emoji_tex(const char *name, int n) {
    char path[96], nm[64];
    if (n >= (int)sizeof(nm)) n = sizeof(nm) - 1;
    memcpy(nm, name, n);
    nm[n] = '\0';
    snprintf(path, sizeof(path), "/v1/emoji?n=%s&s=40", nm);
    return tex_get(path);
}

static int emoji_box(int size) { return (int)(size * 1.2f); }

static int walk(vita2d_font *f, int x, int y, unsigned int color, int size, const char *s, int len, int draw) {
    if (!s) return 0;
    if (len < 0) len = (int)strlen(s);
    char buf[256];
    int pen = x, i = 0;
    while (i < len) {
        if (s[i] == '\x1E') {
            int j = i + 1;
            while (j < len && s[j] != '\x1F') j++;
            int box = emoji_box(size);
            if (draw) {
                vita2d_texture *t = emoji_tex(s + i + 1, j - i - 1);
                if (t) {
                    float sc = (float)box / vita2d_texture_get_width(t);
                    vita2d_draw_texture_scale(t, pen + 1, y - size * 0.98f, sc, sc);
                }
            }
            pen += box + 2;
            i = j < len ? j + 1 : j;
            continue;
        }
        int j = i;
        while (j < len && s[j] != '\x1E' && s[j] != '\n' && j - i < (int)sizeof(buf) - 4) j++;
        if (j < len && j - i >= (int)sizeof(buf) - 4)
            while (j > i && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
        int n = j - i;
        memcpy(buf, s + i, n);
        buf[n] = '\0';
        if (n > 0) {
            if (draw) pen += vita2d_font_draw_text(f, pen, y, color, size, buf);
            else pen += vita2d_font_text_width(f, size, buf);
        }
        i = j;
        if (i < len && s[i] == '\n') i++;
    }
    return pen - x;
}

int txt_w(vita2d_font *f, int size, const char *s, int len) { return walk(f, 0, 0, 0, size, s, len, 0); }
int txt(vita2d_font *f, int x, int y, unsigned int color, int size, const char *s, int len) { return walk(f, x, y, color, size, s, len, 1); }

void txt_fit(vita2d_font *f, int x, int y, unsigned int color, int size, const char *s, int maxw) {
    int len = (int)strlen(s);
    if (txt_w(f, size, s, len) <= maxw) {
        txt(f, x, y, color, size, s, len);
        return;
    }
    int ell = vita2d_font_text_width(f, size, "…");
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
        int k = len;
        while (k > 0 && s[k - 1] != '\x1F' && s[k - 1] != '\x1E') k--;
        if (k > 0 && s[k - 1] == '\x1E') len = k - 1;
        if (txt_w(f, size, s, len) + ell <= maxw) break;
    }
    int w = txt(f, x, y, color, size, s, len);
    vita2d_font_draw_text(f, x + w, y, color, size, "…");
}

int txt_fit_w(vita2d_font *f, int size, const char *s, int maxw) {
    int w = txt_w(f, size, s, -1);
    return w < maxw ? w : maxw;
}

void txt_center(vita2d_font *f, int cx, int y, unsigned int color, int size, const char *s) {
    int w = txt_w(f, size, s, -1);
    txt(f, cx - w / 2, y, color, size, s, -1);
}

void txt_right(vita2d_font *f, int rx, int y, unsigned int color, int size, const char *s) {
    int w = txt_w(f, size, s, -1);
    txt(f, rx - w, y, color, size, s, -1);
}

static int utf8_next(const char *s, int i, int len) {
    if (s[i] == '\x1E') {
        while (i < len && s[i] != '\x1F') i++;
        return i < len ? i + 1 : len;
    }
    i++;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

int wrap(vita2d_font *f, int size, const char *s, int maxw, short *off, short *len, int max_lines) {
    int L = (int)strlen(s);
    int n = 0, i = 0;
    while (i < L && n < max_lines) {
        int start = i, j = i;
        while (j < L && s[j] != '\n') {
            int k;
            if (s[j] == ' ') k = j + 1;
            else if (s[j] == '\x1E') k = utf8_next(s, j, L);
            else {
                k = j;
                while (k < L && s[k] != ' ' && s[k] != '\n' && s[k] != '\x1E') k++;
            }
            if (txt_w(f, size, s + start, k - start) <= maxw) {
                j = k;
                continue;
            }
            if (j == start) {
                int c = start;
                for (;;) {
                    int nx = utf8_next(s, c, L);
                    if (nx > k || txt_w(f, size, s + start, nx - start) > maxw) break;
                    c = nx;
                }
                j = c > start ? c : utf8_next(s, start, L);
            }
            break;
        }
        int e = j;
        while (e > start && s[e - 1] == ' ') e--;
        off[n] = (short)start;
        len[n] = (short)(e - start);
        n++;
        if (j < L && s[j] == '\n') j++;
        else while (j < L && s[j] == ' ') j++;
        i = j;
    }
    if (n == 0) {
        off[0] = 0;
        len[0] = 0;
        n = 1;
    }
    return n;
}

void first_glyph(const char *s, char *out, int cap) {
    while (*s == ' ' || *s == '~' || *s == '.') s++;
    if (*s == '+') {
        // numeros sin nombre: se muestra el codigo de pais
        int n = 1;
        while (s[n] >= '0' && s[n] <= '9' && n < 4) n++;
        if (n >= cap) n = cap - 1;
        memcpy(out, s, n);
        out[n] = '\0';
        return;
    }
    int L = (int)strlen(s);
    if (!L) {
        snprintf(out, cap, "?");
        return;
    }
    int n = utf8_next(s, 0, L);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    if (n == 1 && out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
}

// ---------- iconos de botones ----------
void glyph(int which, float cx, float cy, float size, unsigned int bg) {
    float r = size / 2;
    unsigned int c = which == GLYPH_CROSS ? G_CROSS : which == GLYPH_CIRCLE ? G_CIRCLE : which == GLYPH_TRIANGLE ? G_TRI : G_SQUARE;
    ring(cx, cy, r, 1.6f, c, bg);
    float k = r * 0.38f;
    switch (which) {
    case GLYPH_CROSS:
        thick_line(cx - k, cy - k, cx + k, cy + k, 1.8f, c);
        thick_line(cx + k, cy - k, cx - k, cy + k, 1.8f, c);
        break;
    case GLYPH_CIRCLE:
        ring(cx, cy, r * 0.45f, 1.8f, c, bg);
        break;
    case GLYPH_TRIANGLE:
        thick_line(cx, cy - k * 1.15f, cx + k * 1.1f, cy + k * 0.8f, 1.8f, c);
        thick_line(cx + k * 1.1f, cy + k * 0.8f, cx - k * 1.1f, cy + k * 0.8f, 1.8f, c);
        thick_line(cx - k * 1.1f, cy + k * 0.8f, cx, cy - k * 1.15f, 1.8f, c);
        break;
    default:
        thick_line(cx - k, cy - k, cx + k, cy - k, 1.8f, c);
        thick_line(cx + k, cy - k, cx + k, cy + k, 1.8f, c);
        thick_line(cx + k, cy + k, cx - k, cy + k, 1.8f, c);
        thick_line(cx - k, cy + k, cx - k, cy - k, 1.8f, c);
        break;
    }
}

int keycap(int x, int cy, const char *label) {
    int tw = vita2d_font_text_width(F_BOLD, 11, label);
    int w = tw + 12;
    if (w < 24) w = 24;
    rrect(x, cy - 9, w, 18, 4, C_TRACK);
    vita2d_font_draw_text(F_BOLD, x + (w - tw) / 2, cy + 4, C_SUB2, 11, label);
    return w;
}

static const unsigned int GLYPH_BTN[4] = {SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_TRIANGLE, SCE_CTRL_SQUARE};

int hint(int x, int cy, int which, const char *label) {
    glyph(which, x + 9, cy, 18, C_BAR);
    int w = txt(F_REG, x + 24, cy + 5, C_SUB2, 13, label, -1);
    touch_zone(x - 6, cy - 18, 24 + w + 12, 36, GLYPH_BTN[which & 3]);
    return x + 24 + w + 20;
}

int hint_key(int x, int cy, const char *key, unsigned int button, const char *label) {
    int w = keycap(x, cy, key);
    int t = txt(F_REG, x + w + 6, cy + 5, C_SUB2, 13, label, -1);
    if (button) touch_zone(x - 6, cy - 18, w + 6 + t + 12, 36, button);
    return x + w + 6 + t + 20;
}

// ---------- iconos dibujados ----------
void icon_back(float x, float cy, unsigned int c) {
    thick_line(x + 7, cy - 7, x, cy, 2.4f, c);
    thick_line(x, cy, x + 7, cy + 7, 2.4f, c);
}
void icon_plus(float cx, float cy, float s, unsigned int c) {
    thick_line(cx - s, cy, cx + s, cy, 2.4f, c);
    thick_line(cx, cy - s, cx, cy + s, 2.4f, c);
}
void icon_mic(float cx, float cy, float s, unsigned int c) {
    rrect(cx - s * 0.32f, cy - s, s * 0.64f, s * 1.15f, s * 0.32f, c);
    arc_dots(cx, cy - s * 0.05f, s * 0.62f, 0.15f, 2.99f, 1.8f, c);
    thick_line(cx, cy + s * 0.6f, cx, cy + s, 1.8f, c);
}
void icon_camera(float cx, float cy, float s, unsigned int c) {
    rrect(cx - s, cy - s * 0.6f, s * 2, s * 1.4f, s * 0.25f, c);
    rrect(cx - s * 0.4f, cy - s * 0.85f, s * 0.8f, s * 0.4f, s * 0.12f, c);
    circle(cx, cy + s * 0.08f, s * 0.48f, C_WHITE);
    circle(cx, cy + s * 0.08f, s * 0.3f, c);
}
void icon_image(float cx, float cy, float s, unsigned int c) {
    rrect(cx - s, cy - s * 0.75f, s * 2, s * 1.5f, s * 0.25f, c);
    rrect(cx - s + 2, cy - s * 0.75f + 2, s * 2 - 4, s * 1.5f - 4, s * 0.2f, C_WHITE);
    circle(cx - s * 0.4f, cy - s * 0.25f, s * 0.2f, c);
    fill_tri(cx - s * 0.75f, cy + s * 0.55f, cx + s * 0.1f, cy - s * 0.1f, cx + s * 0.8f, cy + s * 0.55f, c);
}
void icon_sticker(float cx, float cy, float s, unsigned int c, unsigned int bg) {
    rrect(cx - s, cy - s, s * 2, s * 2, s * 0.6f, c);
    rrect(cx - s + 2, cy - s + 2, s * 2 - 4, s * 2 - 4, s * 0.5f, bg);
    circle(cx - s * 0.35f, cy - s * 0.2f, s * 0.15f, c);
    circle(cx + s * 0.35f, cy - s * 0.2f, s * 0.15f, c);
    arc_dots(cx, cy + s * 0.05f, s * 0.45f, 0.5f, 2.64f, 1.8f, c);
}
void icon_clip(float cx, float cy, float s, unsigned int c) {
    // clip simplificado: dos curvas paralelas
    arc_dots(cx, cy - s * 0.35f, s * 0.35f, 3.14159f, 6.2832f, 1.8f, c);
    thick_line(cx - s * 0.35f, cy - s * 0.35f, cx - s * 0.35f, cy + s * 0.5f, 1.8f, c);
    thick_line(cx + s * 0.35f, cy - s * 0.35f, cx + s * 0.35f, cy + s * 0.3f, 1.8f, c);
    arc_dots(cx, cy + s * 0.5f, s * 0.35f, 0, 3.14159f, 1.8f, c);
    thick_line(cx, cy - s * 0.2f, cx, cy + s * 0.3f, 1.8f, c);
}
void icon_play(float cx, float cy, float s, unsigned int c) {
    fill_tri(cx - s * 0.45f, cy - s * 0.6f, cx - s * 0.45f, cy + s * 0.6f, cx + s * 0.6f, cy, c);
}
void icon_pause(float cx, float cy, float s, unsigned int c) {
    rrect(cx - s * 0.5f, cy - s * 0.55f, s * 0.35f, s * 1.1f, 1, c);
    rrect(cx + s * 0.15f, cy - s * 0.55f, s * 0.35f, s * 1.1f, 1, c);
}
void icon_search(float cx, float cy, float s, unsigned int c) {
    arc_dots(cx - s * 0.15f, cy - s * 0.15f, s * 0.55f, 0, 6.2832f, 2, c);
    thick_line(cx + s * 0.25f, cy + s * 0.25f, cx + s * 0.75f, cy + s * 0.75f, 2.2f, c);
}
void icon_lock(float cx, float cy, float s, unsigned int c) {
    rrect(cx - s * 0.7f, cy - s * 0.1f, s * 1.4f, s * 1.0f, s * 0.15f, c);
    arc_dots(cx, cy - s * 0.25f, s * 0.42f, 3.14159f, 6.2832f, 2, c);
    thick_line(cx - s * 0.42f, cy - s * 0.25f, cx - s * 0.42f, cy - s * 0.1f, 2, c);
    thick_line(cx + s * 0.42f, cy - s * 0.25f, cx + s * 0.42f, cy - s * 0.1f, 2, c);
}
void icon_people(float cx, float cy, float s, unsigned int c) {
    circle(cx - s * 0.35f, cy - s * 0.3f, s * 0.3f, c);
    circle(cx + s * 0.4f, cy - s * 0.25f, s * 0.25f, c);
    rrect(cx - s * 0.85f, cy + s * 0.1f, s * 1.0f, s * 0.6f, s * 0.3f, c);
    rrect(cx + s * 0.05f, cy + s * 0.15f, s * 0.75f, s * 0.5f, s * 0.25f, c);
}

void icon_mute(float cx, float cy, float s, unsigned int c) {
    // altavoz tachado
    rrect(cx - s * 1.0f, cy - s * 0.35f, s * 0.5f, s * 0.7f, 1, c);
    fill_tri(cx - s * 0.55f, cy - s * 0.35f, cx + s * 0.1f, cy - s * 0.95f, cx + s * 0.1f, cy + s * 0.95f, c);
    fill_tri(cx - s * 0.55f, cy - s * 0.35f, cx + s * 0.1f, cy + s * 0.95f, cx - s * 0.55f, cy + s * 0.35f, c);
    thick_line(cx + s * 0.35f, cy - s * 0.4f, cx + s * 1.05f, cy + s * 0.4f, 2, c);
    thick_line(cx + s * 0.35f, cy + s * 0.4f, cx + s * 1.05f, cy - s * 0.4f, 2, c);
}
void icon_pin(float cx, float cy, float s, unsigned int c) {
    // chincheta inclinada
    circle(cx + s * 0.25f, cy - s * 0.25f, s * 0.45f, c);
    thick_line(cx - s * 0.2f, cy - s * 0.7f, cx + s * 0.7f, cy + s * 0.2f, 3, c);
    thick_line(cx, cy, cx - s * 0.75f, cy + s * 0.75f, 2, c);
}

// ---------- piezas del diseno ----------
void logo(float x, float cy, float s) {
    float k = s / 30.0f;
    circle(x + 13 * k, cy - 2 * k, 11 * k, C_ACC);
    circle(x + 17 * k, cy - 2 * k, 11 * k, C_ACC);
    fill_tri(x + 6 * k, cy + 4 * k, x + 4 * k, cy + 12 * k, x + 13 * k, cy + 7 * k, C_ACC);
    for (int i = -1; i <= 1; i++) circle(x + 15 * k + i * 5.5f * k, cy - 2 * k, 1.8f * k, C_WHITE);
    rrect_grad(x + 4 * k, cy - 12 * k, 22 * k, 9 * k, 4.5f * k, HEXA(0xFFFFFF, 0x90), HEXA(0xFFFFFF, 0));   // brillo
}

void avatar(float cx, float cy, float r, const char *name, int color_idx, const char *jid) {
    if (jid && jid[0]) {
        char path[200], ej[128];
        url_enc(jid, ej, sizeof(ej));
        snprintf(path, sizeof(path), "/v1/avatar?jid=%s&s=%d", ej, r > 40 ? 256 : 96);
        vita2d_texture *t = tex_get(path);
        if (t) {
            float sc = (2 * r) / vita2d_texture_get_width(t);
            vita2d_draw_texture_scale(t, cx - r, cy - r, sc, sc);   // ya viene recortada en circulo
            if (r >= 14) {
                // aro de luz + reflejo: el "orbe" de la LiveArea
                arc_dots(cx, cy, r + 0.5f, 0, 6.2831853f, 1.6f, HEXA(0xFFFFFF, ui_dark ? 0x60 : 0xB0));
                rrect_grad(cx - r * 0.62f, cy - r * 0.92f, r * 1.24f, r * 0.62f, r * 0.31f, HEXA(0xFFFFFF, 0x50), HEXA(0xFFFFFF, 0));
            }
            return;
        }
    }
    circle(cx, cy, r, pastel_bg(color_idx));
    if (r >= 14) rrect_grad(cx - r * 0.62f, cy - r * 0.92f, r * 1.24f, r * 0.62f, r * 0.31f, HEXA(0xFFFFFF, 0x70), HEXA(0xFFFFFF, 0));
    if (jid && strstr(jid, "@g.us") && !(name && name[0] && name[0] != '+')) {
        icon_people(cx, cy, r * 0.55f, pastel_fg(color_idx));
        return;
    }
    char g[48];
    first_glyph(name ? name : "?", g, sizeof(g));
    int size = (int)(r * (g[0] == '+' ? 0.6f : 0.82f));
    int w = txt_w(F_BOLD, size, g, -1);
    txt(F_BOLD, (int)(cx - w / 2.0f), (int)(cy + size * 0.36f), pastel_fg(color_idx), size, g, -1);
}

void header_bar(void) {
    // barra de vidrio: degradado + reflejo + línea de luz + sombra suave hacia abajo
    grad_rect(0, 0, SCREEN_W, 56, TH[T_BAR_TOP], C_BAR);
    grad_rect(0, 0, SCREEN_W, 26, HEXA(0xFFFFFF, ui_dark ? 0x18 : 0x80), HEXA(0xFFFFFF, 0));
    vita2d_draw_rectangle(0, 55, SCREEN_W, 1, C_LINE);
    grad_rect(0, 56, SCREEN_W, 6, HEXA(0x000000, ui_dark ? 0x40 : 0x12), HEXA(0x000000, 0));
}

void footer_bar(void) {
    grad_rect(0, SCREEN_H - 42, SCREEN_W, 6, HEXA(0x000000, 0), HEXA(0x000000, ui_dark ? 0x40 : 0x10));
    grad_rect(0, SCREEN_H - 36, SCREEN_W, 36, C_BAR, TH[T_BAR_TOP]);
    vita2d_draw_rectangle(0, SCREEN_H - 36, SCREEN_W, 1, C_LINE);
    vita2d_draw_rectangle(0, SCREEN_H - 35, SCREEN_W, 1, HEXA(0xFFFFFF, ui_dark ? 0x14 : 0xA0));
}

void ticks(float x, float cy, int ack, unsigned int bubble) {
    unsigned int c = ack >= 4 ? C_ACC : C_MINE_SUB;
    if (ack <= 1) {
        ring(x + 6, cy, 5, 1.4f, c, bubble);
        thick_line(x + 6, cy - 2.5f, x + 6, cy, 1.2f, c);
        thick_line(x + 6, cy, x + 8, cy + 1, 1.2f, c);
        return;
    }
    thick_line(x + 1, cy + 0.5f, x + 3.8f, cy + 3.3f, 1.6f, c);
    thick_line(x + 3.8f, cy + 3.3f, x + 9.5f, cy - 3.3f, 1.6f, c);
    if (ack >= 3) thick_line(x + 6.5f, cy + 3.3f, x + 12.2f, cy - 3.3f, 1.6f, c);
}

void badge(float rx, float cy, int n) {
    char b[16];
    snprintf(b, sizeof(b), "%d", n > 99 ? 99 : n);
    int w = txt_w(F_XB, 12, b, -1) + 12;
    if (w < 20) w = 20;
    rrect(rx - w, cy - 10, w, 20, 10, C_ACC);
    txt_center(F_XB, (int)(rx - w / 2.0f), (int)cy + 5, C_WHITE, 12, b);
}

void pill(int x, int cy, const char *label, int active) {
    int w = pill_w(label, active);
    rrect(x, cy - 13, w, 26, 13, active ? C_ACC : C_CHIP);
    txt(active ? F_BOLD : F_REG, x + 11, cy + 5, active ? C_WHITE : C_SUB2, 13, label, -1);
}

int pill_w(const char *label, int active) { return txt_w(active ? F_BOLD : F_REG, 13, label, -1) + 22; }

void segmented(int x, int cy, const char **labels, int n, int active, int dot_index) {
    int widths[4], total = 6;
    for (int i = 0; i < n && i < 4; i++) {
        widths[i] = txt_w(F_BOLD, 15, labels[i], -1) + 36 + (i == dot_index ? 12 : 0);
        total += widths[i];
    }
    rrect(x, cy - 17, total, 34, 17, C_TRACK);
    int px = x + 3;
    for (int i = 0; i < n && i < 4; i++) {
        if (i == active) {
            rrect(px, cy - 14, widths[i], 28, 14, HEXA(0x1E2A36, 0x22));
            rrect(px, cy - 15, widths[i], 28, 14, C_CARD);
            gloss(px, cy - 15, widths[i], 28, 14);
        }
        txt(i == active ? F_BOLD : F_REG, px + 18, cy + 5, i == active ? C_TEXT : C_SUB, 15, labels[i], -1);
        if (i == dot_index) circle(px + widths[i] - 18, cy, 3.5f, C_ACC);
        px += widths[i];
    }
}

void battery(int rx, int cy) {
    int bat = scePowerGetBatteryLifePercent();
    if (bat < 0) bat = 0;
    char b[16];
    snprintf(b, sizeof(b), "%d%%", bat);
    int bw = txt_w(F_REG, 13, b, -1);
    txt(F_REG, rx - bw, cy + 5, C_SUB, 13, b, -1);
    int x = rx - bw - 28;
    rrect(x, cy - 6, 19, 12, 3, C_SUB);
    rrect(x + 1.2f, cy - 4.8f, 16.6f, 9.6f, 2, C_BAR);
    vita2d_draw_rectangle(x + 2.5f, cy - 3.5f, 14.0f * bat / 100.0f, 7, bat <= 15 ? C_RED : C_SUB);
    vita2d_draw_rectangle(x + 19.5f, cy - 2.5f, 2, 5, C_SUB);
}
