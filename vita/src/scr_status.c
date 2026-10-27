// Visor de estados (pantalla completa, fondo oscuro como el visor de fotos del sistema).

#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <vita2d.h>

#include "app.h"
#include "audio.h"
#include "ime.h"
#include "net.h"
#include "tex.h"
#include "touch.h"

static char sv_poster[64];
static int sv_item = -1;
static unsigned int sv_start = 0;
static int sv_paused = 0;
static unsigned int sv_paused_at = 0;
static char sv_marked[48];
#define SV_ITEM_MS 6000
#define SV_BG HEX(0x0B1117)

static void status_media_path(char *out, int cap, const char *poster, const char *id) {
    char ep[128], ei[96];
    url_enc(poster, ep, sizeof(ep));
    url_enc(id, ei, sizeof(ei));
    snprintf(out, cap, "/v1/media?jid=status%%40broadcast&poster=%s&id=%s&max=512", ep, ei);
}

static int is_visual(const StatusItem *it) {
    return !strcmp(it->kind, "image") || !strcmp(it->kind, "video") || !strcmp(it->kind, "gif");
}

void draw_status_card(float x, float y, float w, float h, const StatusItem *it, const char *poster) {
    unsigned int bg = it && it->bg ? (it->bg | 0xFF000000) : name_color(hash_color(poster));
    rrect(x, y, w, h, 20, bg);
    if (!it) return;
    if (is_visual(it)) {
        char path[320];
        status_media_path(path, sizeof(path), poster, it->id);
        vita2d_texture *t = tex_get(path);
        rrect(x, y, w, h, 20, HEX(0x1E2A36));
        if (t) draw_texture_contain(t, x + 6, y + 6, w - 12, h - 12);
        else photo_placeholder(x + 10, y + 10, w - 20, h - 20, "Cargando…");
        return;
    }
    short off[12], len[12];
    int size = w > 260 ? 24 : 20;
    int n = wrap(F_BOLD, size, it->text, (int)w - 40, off, len, 12);
    float ty = y + h / 2 - n * (size + 6) / 2.0f + size;
    for (int i = 0; i < n; i++) {
        int lw = txt_w(F_BOLD, size, it->text + off[i], len[i]);
        txt(F_BOLD, (int)(x + (w - lw) / 2), (int)(ty + i * (size + 6)), C_WHITE, size, it->text + off[i], len[i]);
    }
}

static int poster_index(const char *poster) {
    for (int i = 0; i < view.nstatus; i++) if (!strcmp(view.status[i].poster, poster)) return i;
    return -1;
}

void open_status_viewer(const char *poster) {
    snprintf(sv_poster, sizeof(sv_poster), "%s", poster);
    sync_open_status(poster);
    sv_item = -1;
    sv_paused = 0;
    sv_marked[0] = '\0';
    if (screen != SCR_STATUS) go(SCR_STATUS);
}

static int sv_ready(void) { return !strcmp(view.sitems_poster, sv_poster) && view.nsitems > 0; }

static void sv_next_poster(int dir) {
    int i = poster_index(sv_poster);
    int j = i + dir;
    while (j >= 0 && j < view.nstatus && !strcmp(view.status[j].poster, "me") && strcmp(sv_poster, "me")) j += dir;
    if (i < 0 || j < 0 || j >= view.nstatus) {
        go_back_sound(SCR_HOME);
        return;
    }
    audio_sfx(SFX_MOVE);
    open_status_viewer(view.status[j].poster);
}

static void sv_go(int dir) {
    if (!sv_ready()) return;
    int n = sv_item + dir;
    if (n < 0) { sv_next_poster(-1); return; }
    if (n >= view.nsitems) { sv_next_poster(1); return; }
    sv_item = n;
    sv_start = now_ms();
    sv_paused = 0;
}

static void sv_pause(int on) {
    if (on && !sv_paused) { sv_paused = 1; sv_paused_at = now_ms(); }
    if (!on && sv_paused) { sv_start += now_ms() - sv_paused_at; sv_paused = 0; }
}

static void draw_side_card(float x, const StatusRow *s, const char *key) {
    rrect(x, 150, 120, 210, 16, HEX(0x1A242E));
    avatar(x + 60, 222, 26, s->name, s->color, s->poster);
    short off[2], len[2];
    int n = wrap(F_BOLD, 13, s->name, 104, off, len, 2);
    for (int i = 0; i < n; i++) {
        int lw = txt_w(F_BOLD, 13, s->name + off[i], len[i]);
        txt(F_BOLD, (int)(x + 60 - lw / 2), 282 + i * 17, HEX(0xC9D3DD), 13, s->name + off[i], len[i]);
    }
    keycap((int)x + 48, 384, key);
}

#define CX 330
#define CY 12
#define CW 300
#define CH 520

void draw_status_viewer(void) {
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, SV_BG);
    int pi = poster_index(sv_poster);
    const char *name = pi >= 0 ? view.status[pi].name : "";

    int prev = pi - 1, next = pi + 1;
    if (pi >= 0 && prev >= 0 && strcmp(view.status[prev].poster, "me")) {
        draw_side_card(170, &view.status[prev], "L");
        touch_zone(170, 150, 120, 250, SCE_CTRL_LTRIGGER);
    }
    if (pi >= 0 && next < view.nstatus && next > 0) {
        draw_side_card(SCREEN_W - 170 - 120, &view.status[next], "R");
        touch_zone(SCREEN_W - 290, 150, 120, 250, SCE_CTRL_RTRIGGER);
    }

    rrect(CX, CY, CW, CH, 20, HEX(0x1A242E));
    if (!sv_ready()) {
        spinner(CX + CW / 2, CY + CH / 2, frame);
        return;
    }
    if (sv_item < 0) {
        sv_item = 0;
        for (int i = 0; i < view.nsitems; i++) if (!view.sitems[i].seen) { sv_item = i; break; }
        sv_start = now_ms();
    }
    if (sv_item >= view.nsitems) sv_item = view.nsitems - 1;
    StatusItem *it = &view.sitems[sv_item];

    if (strcmp(sv_poster, "me") && !it->seen && strcmp(sv_marked, it->id)) {
        job_status_seen(sv_poster, it->id);
        snprintf(sv_marked, sizeof(sv_marked), "%s", it->id);
    }

    unsigned int elapsed = sv_paused ? sv_paused_at - sv_start : now_ms() - sv_start;
    float prog = elapsed / (float)SV_ITEM_MS;
    if (prog > 1) prog = 1;

    float ay = CY + 70, ah = CH - 70 - 60;
    if (is_visual(it)) {
        char path[320];
        status_media_path(path, sizeof(path), sv_poster, it->id);
        float ih = ah - (it->text[0] ? 40 : 0);
        vita2d_texture *t = tex_get(path);
        if (t) draw_texture_contain(t, CX + 12, ay, CW - 24, ih);
        else photo_placeholder(CX + 12, ay, CW - 24, ih, "Cargando…");
        if (strcmp(it->kind, "image")) {
            circle(CX + CW / 2, ay + ih / 2, 24, HEXA(0x000000, 0x90));
            icon_play(CX + CW / 2 + 2, ay + ih / 2, 10, C_WHITE);
            txt_center(F_REG, CX + CW / 2, (int)(ay + ih / 2 + 44), C_WHITE, 13, "Video: míralo en el teléfono");
        }
        if (it->text[0]) {
            short off[3], len[3];
            int n = wrap(F_REG, 15, it->text, CW - 40, off, len, 3);
            for (int i = 0; i < n; i++) {
                int lw = txt_w(F_REG, 15, it->text + off[i], len[i]);
                txt(F_REG, CX + (CW - lw) / 2, (int)(ay + ah - 18 + i * 18), C_WHITE, 15, it->text + off[i], len[i]);
            }
        }
    } else {
        draw_status_card(CX + 12, ay, CW - 24, ah, it, sv_poster);
    }

    // segmentos de progreso
    float gx = CX + 12, gw = (CW - 24 - (view.nsitems - 1) * 4) / (float)view.nsitems;
    for (int i = 0; i < view.nsitems; i++) {
        float x = gx + i * (gw + 4);
        rrect(x, CY + 10, gw, 3, 1.5f, HEXA(0xFFFFFF, 0x59));
        float f = i < sv_item ? 1 : i == sv_item ? prog : 0;
        if (f > 0) rrect(x, CY + 10, gw * f, 3, 1.5f, C_WHITE);
    }
    avatar(CX + 31, CY + 41, 17, name, pi >= 0 ? view.status[pi].color : 0, sv_poster);
    txt_fit(F_BOLD, CX + 56, CY + 39, C_WHITE, 15, name, 200);
    txt(F_REG, CX + 56, CY + 56, HEX(0xB8C3CE), 12, it->ago, -1);
    if (sv_paused) txt_right(F_BOLD, CX + CW - 14, CY + 45, HEX(0xC9D3DD), 12, "EN PAUSA");

    float ry = CY + CH - 52;
    if (strcmp(sv_poster, "me")) {
        rrect(CX + 12, ry, CW - 24 - 50, 40, 20, HEX(0x2B3946));
        glyph(GLYPH_CROSS, CX + 36, ry + 20, 18, HEX(0x2B3946));
        txt(F_REG, CX + 52, (int)ry + 25, HEX(0xC9D3DD), 14, "Responder", -1);
        touch_zone(CX + 12, ry, CW - 74, 40, SCE_CTRL_CROSS);
        circle(CX + CW - 32, ry + 20, 20, HEX(0x2B3946));
        txt(F_REG, CX + CW - 46, (int)ry + 30, C_WHITE, 18, "\x1E" "2764\x1F", -1);
        touch_zone(CX + CW - 54, ry - 2, 44, 44, SCE_CTRL_SQUARE);
    } else {
        txt_center(F_REG, CX + CW / 2, (int)ry + 25, HEX(0xB8C3CE), 13, "Tu estado");
    }
    // tocar la mitad izquierda / derecha de la tarjeta: anterior / siguiente
    touch_zone(CX, CY + 60, CW / 3, CH - 120, SCE_CTRL_LEFT);
    touch_zone(CX + CW / 3, CY + 60, CW / 3, CH - 120, SCE_CTRL_TRIANGLE);
    touch_zone(CX + 2 * CW / 3, CY + 60, CW / 3, CH - 120, SCE_CTRL_RIGHT);

    // ayuda
    float hx = SCREEN_W - 160;
    unsigned int hc = HEX(0xB8C3CE);
    glyph(GLYPH_CIRCLE, hx + 8, 32, 16, SV_BG);
    txt(F_REG, (int)hx + 22, 37, hc, 13, "Salir", -1);
    touch_zone(hx - 6, 14, 100, 36, SCE_CTRL_CIRCLE);
    txt(F_REG, (int)hx, 62, hc, 13, "Izq / Der  estado", -1);
    txt(F_REG, (int)hx, 82, hc, 13, "L / R  contacto", -1);
    glyph(GLYPH_TRIANGLE, hx + 8, 102, 16, SV_BG);
    txt(F_REG, (int)hx + 22, 107, hc, 13, "Pausa", -1);
    if (strcmp(sv_poster, "me")) {
        glyph(GLYPH_SQUARE, hx + 8, 127, 16, SV_BG);
        txt(F_REG, (int)hx + 22, 132, hc, 13, "Mandar ", -1);
        txt(F_REG, (int)hx + 74, 132, hc, 13, "\x1E" "2764\x1F", -1);
    }

    if (!sv_paused && prog >= 1) sv_go(1);
}

void input_status(void) {
    if (btn(SCE_CTRL_CIRCLE)) { go_back_sound(SCR_HOME); return; }
    if (btn(SCE_CTRL_RIGHT)) sv_go(1);
    if (btn(SCE_CTRL_LEFT)) sv_go(-1);
    if (btn(SCE_CTRL_RTRIGGER)) { sv_next_poster(1); return; }
    if (btn(SCE_CTRL_LTRIGGER)) { sv_next_poster(-1); return; }
    if (btn(SCE_CTRL_TRIANGLE) && sv_ready()) sv_pause(!sv_paused);
    if (sv_ready() && sv_item >= 0 && strcmp(sv_poster, "me")) {
        StatusItem *it = &view.sitems[sv_item];
        if (btn(SCE_CTRL_CROSS)) {
            sv_pause(1);
            audio_sfx(SFX_SELECT);
            ime_ask(IMEP_STATUS_REPLY, "Responder al estado", "", IME_KIND_TEXT, 500);
        }
        if (btn(SCE_CTRL_SQUARE)) job_status_reply(sv_poster, it->id, "\xE2\x9D\xA4\xEF\xB8\x8F");
    }
}

void status_ime_result(int purpose, const char *text) {
    if (purpose == IMEP_STATUS_POST) {
        if (text && text[0]) job_status_post(text);
        return;
    }
    if (text && text[0] && sv_ready() && sv_item >= 0 && sv_item < view.nsitems)
        job_status_reply(sv_poster, view.sitems[sv_item].id, text);
    sv_pause(0);
}
