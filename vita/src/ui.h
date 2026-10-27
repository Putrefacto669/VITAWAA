#ifndef VITAWA_UI_H
#define VITAWA_UI_H

// Primitivas de dibujo con el look de VitaWA fase 3 (tema claro, estilo sistema).
// Todo el texto pasa por txt(): entiende los marcadores de emoji que manda el bridge
// (\x1E<nombre>\x1F) y los dibuja como imagenes.

#include <vita2d.h>

#define SCREEN_W 960
#define SCREEN_H 544

#define HEX(v) ((unsigned int)RGBA8(((v) >> 16) & 0xFF, ((v) >> 8) & 0xFF, (v) & 0xFF, 0xFF))
#define HEXA(v, a) ((unsigned int)RGBA8(((v) >> 16) & 0xFF, ((v) >> 8) & 0xFF, (v) & 0xFF, (a)))

// ── Tema (Aero claro / Aero noche) ──────────────────────────────────────────
// Los colores de superficies y texto viven en TH[] para poder cambiar de tema en
// caliente desde Ajustes. Los nombres C_* se quedaron igual que antes.
enum {
    T_BG, T_BAR, T_CARD, T_SOFT, T_LINE, T_LINE2, T_CHIP, T_TRACK, T_DAY, T_BUBBLE,
    T_BUBBLE_LN, T_PHOTO_BG, T_DIM, T_TEXT, T_SUB, T_SUB2, T_FAINT, T_WARN_BG, T_WARN_TX,
    T_BG_TOP, T_BG_BOT, T_BAR_TOP, T_GLOSS, T_COUNT
};
extern unsigned int TH[T_COUNT];
extern int ui_dark;
void ui_set_theme(int dark);
#define THEME_COUNT 2

// fondo y superficies
#define C_BG        TH[T_BG]
#define C_BAR       TH[T_BAR]
#define C_CARD      TH[T_CARD]
#define C_SOFT      TH[T_SOFT]
#define C_LINE      TH[T_LINE]
#define C_LINE2     TH[T_LINE2]
#define C_CHIP      TH[T_CHIP]
#define C_TRACK     TH[T_TRACK]
#define C_DAY       TH[T_DAY]
#define C_BUBBLE    TH[T_BUBBLE]
#define C_BUBBLE_LN TH[T_BUBBLE_LN]
#define C_PHOTO_BG  TH[T_PHOTO_BG]
#define C_DIM       TH[T_DIM]
// texto
#define C_TEXT      TH[T_TEXT]
#define C_SUB       TH[T_SUB]
#define C_SUB2      TH[T_SUB2]
#define C_FAINT     TH[T_FAINT]
#define C_WHITE     HEX(0xFFFFFF)
#define C_BLACK     HEX(0x000000)
#define C_ONLINE    HEX(0x2E9E5B)
#define C_RED       HEX(0xE0533D)
#define C_RED_DARK  HEX(0xB83A27)
#define C_WARN_BG   TH[T_WARN_BG]
#define C_WARN_TX   TH[T_WARN_TX]
// botones de la Vita
#define G_CROSS     HEX(0x3B82D6)
#define G_CIRCLE    HEX(0xE0533D)
#define G_TRI       HEX(0x2E9E5B)
#define G_SQUARE    HEX(0xD9468F)

// Acento elegido en ajustes (azul, verde, naranja, morado)
extern unsigned int C_ACC, C_ACC_SOFT, C_MINE, C_MINE_SUB, C_MINE_Q;
void ui_set_accent(int idx);
#define ACCENT_COUNT 4
unsigned int accent_preview(int idx);

extern vita2d_font *F_REG, *F_BOLD, *F_XB;

int  ui_init(void);
void ui_term(void);

// Colores por indice (0..7)
unsigned int name_color(int idx);
unsigned int pastel_bg(int idx);
unsigned int pastel_fg(int idx);
int hash_color(const char *s);

// Formas
void rrect(float x, float y, float w, float h, float r, unsigned int c);
void card(float x, float y, float w, float h, float r, unsigned int fill, unsigned int border);
void focus_card(float x, float y, float w, float h, float r);   // fila seleccionada (relleno + borde acento)
void shadow(float x, float y, float w, float h, float r);
// ── Aero ──
void rrect_grad(float x, float y, float w, float h, float r, unsigned int top, unsigned int bot);
void grad_rect(float x, float y, float w, float h, unsigned int top, unsigned int bot);
void gloss(float x, float y, float w, float h, float r);          // brillo "vidrio" en la mitad de arriba
void ui_background(int frame);                                     // fondo degradado con burbujas y onda de luz
void typing_dots(float x, float cy, unsigned int c);               // los tres puntitos animados
int  txt_fit_w(vita2d_font *f, int size, const char *s, int maxw); // ancho que ocupa txt_fit
void circle(float cx, float cy, float r, unsigned int c);
void ring(float cx, float cy, float r, float t, unsigned int c, unsigned int inner);
void thick_line(float x0, float y0, float x1, float y1, float t, unsigned int c);
void fill_tri(float x0, float y0, float x1, float y1, float x2, float y2, unsigned int c);
void arc_dots(float cx, float cy, float r, float a0, float a1, float t, unsigned int c);
void spinner(float cx, float cy, int frame);

// Texto (size en pixeles, y = linea base). len < 0 => hasta el final.
int  txt_w(vita2d_font *f, int size, const char *s, int len);
int  txt(vita2d_font *f, int x, int y, unsigned int color, int size, const char *s, int len);
void txt_fit(vita2d_font *f, int x, int y, unsigned int color, int size, const char *s, int maxw);
void txt_center(vita2d_font *f, int cx, int y, unsigned int color, int size, const char *s);
void txt_right(vita2d_font *f, int rx, int y, unsigned int color, int size, const char *s);
int  wrap(vita2d_font *f, int size, const char *s, int maxw, short *off, short *len, int max_lines);
void first_glyph(const char *s, char *out, int cap);

// Iconos y botones de la Vita
enum { GLYPH_CROSS, GLYPH_CIRCLE, GLYPH_TRIANGLE, GLYPH_SQUARE };
void glyph(int which, float cx, float cy, float size, unsigned int bg);
int  keycap(int x, int cy, const char *label);
// Ayudas de la barra de abajo: tambien quedan como zona tactil del boton
int  hint(int x, int cy, int which, const char *label);
int  hint_key(int x, int cy, const char *key, unsigned int button, const char *label);

// Iconos dibujados
void icon_back(float x, float cy, unsigned int c);
void icon_plus(float cx, float cy, float s, unsigned int c);
void icon_mic(float cx, float cy, float s, unsigned int c);
void icon_camera(float cx, float cy, float s, unsigned int c);
void icon_image(float cx, float cy, float s, unsigned int c);
void icon_sticker(float cx, float cy, float s, unsigned int c, unsigned int bg);
void icon_clip(float cx, float cy, float s, unsigned int c);
void icon_play(float cx, float cy, float s, unsigned int c);
void icon_pause(float cx, float cy, float s, unsigned int c);
void icon_search(float cx, float cy, float s, unsigned int c);
void icon_lock(float cx, float cy, float s, unsigned int c);
void icon_people(float cx, float cy, float s, unsigned int c);
void icon_mute(float cx, float cy, float s, unsigned int c);
void icon_pin(float cx, float cy, float s, unsigned int c);

// Piezas comunes del diseno
void logo(float x, float cy, float s);
// Avatar: foto de perfil si el bridge la tiene; si no, inicial sobre color pastel
void avatar(float cx, float cy, float r, const char *name, int color_idx, const char *jid);
void header_bar(void);
void footer_bar(void);
void ticks(float x, float cy, int ack, unsigned int bubble);
void badge(float rx, float cy, int n);
void pill(int x, int cy, const char *label, int active);
int  pill_w(const char *label, int active);
void segmented(int x, int cy, const char **labels, int n, int active, int dot_index);
void battery(int rx, int cy);

#endif
