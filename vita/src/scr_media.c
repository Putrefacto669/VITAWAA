// Mandar fotos: con la camara de la Vita o desde la galeria (ux0:picture).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <vita2d.h>

#include "app.h"
#include "audio.h"
#include "camera.h"
#include "gallery.h"
#include "ime.h"
#include "touch.h"

// La cámara y la galería sirven para un chat o para publicar un ESTADO con foto
static int to_status = 0;
static const char *target_jid(void) { return to_status ? "status@broadcast" : open_jid; }
static int back_screen(void) { return to_status ? SCR_HOME : SCR_CHAT; }

static void back_to_chat(void) { go_back_sound(back_screen()); }

static void chat_title(char *out, int cap) {
    if (to_status) { snprintf(out, cap, "Para tu estado"); return; }
    const Chat *c = chat_by_jid(open_jid);
    snprintf(out, cap, "Para %s", view.conv.name[0] ? view.conv.name : c ? c->name : "el chat");
}

// ---------- camara ----------
static int cam_ok = 0, cam_back_sel = 1;
static int review = 0;                 // foto tomada, esperando enviar / repetir
static unsigned char *shot = NULL;
static int shot_len = 0, shot_mirror = 0;
static int flash = 0;

static void drop_shot(void) {
    free(shot);
    shot = NULL;
    shot_len = 0;
    review = 0;
    cam_freeze(0);
}

static void cam_start(void) {
    cam_close();
    cam_ok = cam_open(cam_back_sel) == 0;
}

void camera_enter_status(void) { to_status = 1; drop_shot(); cam_start(); go(SCR_CAMERA); }
void gallery_enter_status(void) { gallery_enter(); to_status = 1; }

void camera_enter(void) {
    to_status = 0;
    drop_shot();
    cam_start();
    go(SCR_CAMERA);
}

static void cam_leave(void) {
    drop_shot();
    cam_close();
    back_to_chat();
}

static void send_shot(const char *caption) {
    if (!shot) return;
    job_image(target_jid(), to_status ? "" : reply_id, caption ? caption : "", "i420", CAM_W, CAM_H, shot_mirror, shot, shot_len);
    shot = NULL;   // job_image se queda con el buffer
    shot_len = 0;
    reply_id[0] = '\0';
    review = 0;
    cam_freeze(0);
    cam_close();
    audio_sfx(SFX_SEND);
    if (to_status) show_toast("Publicando tu estado…", 0);
    screen = back_screen();
}

void camera_ime_result(const char *text) {
    if (text) send_shot(text);
}

void input_camera(void) {
    if (review) {
        if (btn(SCE_CTRL_CROSS)) { send_shot(""); return; }
        if (btn(SCE_CTRL_SQUARE)) {
            audio_sfx(SFX_SELECT);
            char t[96];
            chat_title(t, sizeof(t));
            ime_ask(IMEP_CAPTION, t, "", IME_KIND_TEXT, 500);
            return;
        }
        if (btn(SCE_CTRL_CIRCLE)) { drop_shot(); audio_sfx(SFX_BACK); }
        return;
    }
    if (btn(SCE_CTRL_CIRCLE)) { cam_leave(); return; }
    if (btn(SCE_CTRL_TRIANGLE)) {
        cam_back_sel ^= 1;
        audio_sfx(SFX_MOVE);
        cam_start();
    }
    if ((btn(SCE_CTRL_CROSS) || btn(SCE_CTRL_RTRIGGER)) && cam_ok) {
        shot_len = cam_snapshot_i420(&shot);
        if (shot_len <= 0) {
            audio_sfx(SFX_ERROR);
            show_toast("No pude tomar la foto", 1);
            return;
        }
        shot_mirror = !cam_is_back();
        cam_freeze(1);
        review = 1;
        flash = 10;
        audio_sfx(SFX_SELECT);
    }
}

void draw_camera(void) {
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, HEX(0x0B1117));
    // vista: 640x480 escalada a 725x544 (pantalla completa a lo alto)
    float s = SCREEN_H / (float)CAM_H, dw = CAM_W * s, vx = 40;
    vita2d_texture *t = cam_ok ? cam_texture() : NULL;
    if (t) {
        if (cam_is_back()) vita2d_draw_texture_scale(t, vx, 0, s, s);
        else vita2d_draw_texture_scale(t, vx + dw, 0, -s, s);
    } else {
        icon_camera(vx + dw / 2, 240, 22, HEX(0xB8C3CE));
        txt_center(F_BOLD, (int)(vx + dw / 2), 300, C_WHITE, 16, "No pude abrir la cámara");
        txt_center(F_REG, (int)(vx + dw / 2), 324, HEX(0xB8C3CE), 13, "Prueba con la otra cámara (△)");
    }
    if (flash > 0) {
        vita2d_draw_rectangle(vx, 0, dw, SCREEN_H, HEXA(0xFFFFFF, flash * 22));
        flash--;
    }

    char title[96];
    chat_title(title, sizeof(title));
    rrect(vx + 14, 14, txt_w(F_BOLD, 14, title, -1) + 28, 32, 16, HEXA(0x000000, 0x90));
    txt(F_BOLD, (int)vx + 28, 35, C_WHITE, 14, title, -1);

    // columna de controles
    float cx = vx + dw + (SCREEN_W - vx - dw) / 2;
    unsigned int hc = HEX(0xC9D3DD);
    if (!review) {
        circle(cx, 272, 36, C_WHITE);
        circle(cx, 272, 31, HEX(0x0B1117));
        circle(cx, 272, 28, C_WHITE);
        glyph(GLYPH_CROSS, cx, 330, 18, HEX(0x0B1117));
        touch_zone(cx - 40, 232, 80, 80, SCE_CTRL_CROSS);
        circle(cx, 140, 22, HEX(0x2B3946));
        icon_camera(cx, 140, 9, C_WHITE);
        glyph(GLYPH_TRIANGLE, cx, 182, 16, HEX(0x0B1117));
        txt_center(F_REG, (int)cx, 210, hc, 11, cam_is_back() ? "Trasera" : "Frontal");
        touch_zone(cx - 30, 110, 60, 110, SCE_CTRL_TRIANGLE);
        glyph(GLYPH_CIRCLE, cx, 470, 18, HEX(0x0B1117));
        txt_center(F_REG, (int)cx, 500, hc, 12, "Volver");
        touch_zone(cx - 40, 446, 80, 70, SCE_CTRL_CIRCLE);
    } else {
        rrect(cx - 44, 170, 88, 60, 16, C_ACC);
        txt_center(F_XB, (int)cx, 207, C_WHITE, 15, "Enviar");
        glyph(GLYPH_CROSS, cx, 248, 16, HEX(0x0B1117));
        touch_zone(cx - 44, 170, 88, 92, SCE_CTRL_CROSS);
        rrect(cx - 44, 290, 88, 50, 14, HEX(0x2B3946));
        txt_center(F_REG, (int)cx, 320, C_WHITE, 13, "Con texto");
        glyph(GLYPH_SQUARE, cx, 358, 16, HEX(0x0B1117));
        touch_zone(cx - 44, 290, 88, 80, SCE_CTRL_SQUARE);
        glyph(GLYPH_CIRCLE, cx, 446, 16, HEX(0x0B1117));
        txt_center(F_REG, (int)cx, 474, hc, 12, "Repetir");
        touch_zone(cx - 44, 424, 88, 64, SCE_CTRL_CIRCLE);
    }
}

// ---------- galeria ----------
static int gal_folder = GAL_SHOTS;
static int gal_sel = 0;
static int gal_n = 0;
static Scroller gal_sc;
#define G_COLS 5
#define G_X 20
#define G_TOP 104
#define G_BOT 508
#define G_GAP 10
#define G_CW ((SCREEN_W - 2 * G_X - (G_COLS - 1) * G_GAP) / G_COLS)
#define G_CH 132

static void gal_load(void) {
    gal_n = gallery_scan(gal_folder);
    gal_sel = 0;
    gal_sc.pos = 0;
    gal_sc.target = -1;
}

void gallery_enter(void) {
    to_status = 0;
    gal_load();
    if (gal_n == 0 && gal_folder == GAL_SHOTS) { gal_folder = GAL_ALL; gal_load(); }
    go(SCR_GALLERY);
}

static void gal_leave(void) {
    gallery_free();
    back_to_chat();
}

static void gal_send(const char *caption) {
    if (gal_sel < 0 || gal_sel >= gal_n) return;
    unsigned char *data = NULL;
    int len = gallery_read(gal_sel, &data);
    if (len <= 0) {
        free(data);
        audio_sfx(SFX_ERROR);
        show_toast("No pude leer esa imagen", 1);
        return;
    }
    if (len > 12 * 1024 * 1024) {
        free(data);
        audio_sfx(SFX_ERROR);
        show_toast("La imagen es muy grande", 1);
        return;
    }
    job_image(target_jid(), to_status ? "" : reply_id, caption ? caption : "", "file", 0, 0, 0, data, len);
    if (!to_status) reply_id[0] = '\0';
    audio_sfx(SFX_SEND);
    gallery_free();
    if (to_status) show_toast("Publicando tu estado…", 0);
    screen = back_screen();
}

void gallery_ime_result(const char *text) {
    if (text) gal_send(text);
}

static const char *FOLDERS[3] = {"Capturas", "Cámara", "Todo"};

void input_gallery(void) {
    if (btn(SCE_CTRL_CIRCLE)) { gal_leave(); return; }
    int folder = gal_folder;
    if (btn(SCE_CTRL_LTRIGGER)) folder = (gal_folder + 2) % 3;
    if (btn(SCE_CTRL_RTRIGGER)) folder = (gal_folder + 1) % 3;
    {
        int x = 20 + 24 + 8;
        for (int i = 0; i < 3; i++) {
            int w = pill_w(FOLDERS[i], gal_folder == i);
            if (tap_in(x, 62, w, 32)) folder = i;
            x += w + 8;
        }
    }
    if (folder != gal_folder) {
        gal_folder = folder;
        audio_sfx(SFX_MOVE);
        gal_load();
        return;
    }
    int old = gal_sel;
    if (rep(SCE_CTRL_RIGHT) && gal_sel < gal_n - 1) gal_sel++;
    if (rep(SCE_CTRL_LEFT) && gal_sel > 0) gal_sel--;
    if (rep(SCE_CTRL_DOWN) && gal_sel + G_COLS < gal_n) gal_sel += G_COLS;
    if (rep(SCE_CTRL_UP) && gal_sel >= G_COLS) gal_sel -= G_COLS;
    int send_now = 0;
    if (T.tapped && T.tap_y >= G_TOP && T.tap_y < G_BOT) {
        int col = (int)((T.tap_x - G_X) / (G_CW + G_GAP));
        int row = (int)((T.tap_y - G_TOP + gal_sc.pos) / (G_CH + G_GAP));
        int i = row * G_COLS + col;
        if (col >= 0 && col < G_COLS && i >= 0 && i < gal_n) {
            if (i == gal_sel) send_now = 1;
            gal_sel = i;
        }
    }
    if (gal_sel != old) {
        audio_sfx(SFX_MOVE);
        if (!T.tapped) scroller_show(&gal_sc, (gal_sel / G_COLS) * (G_CH + G_GAP), G_CH + G_GAP, G_BOT - G_TOP);
    }
    int rows = (gal_n + G_COLS - 1) / G_COLS;
    scroller_update(&gal_sc, 0, G_TOP, SCREEN_W, G_BOT - G_TOP, rows * (G_CH + G_GAP) - (G_BOT - G_TOP), 0);

    if (gal_n <= 0) return;
    if (btn(SCE_CTRL_CROSS) || send_now) { gal_send(""); return; }
    if (btn(SCE_CTRL_SQUARE)) {
        audio_sfx(SFX_SELECT);
        char t[96];
        chat_title(t, sizeof(t));
        ime_ask(IMEP_CAPTION, t, "", IME_KIND_TEXT, 500);
    }
}

void draw_gallery(void) {
    header_bar();
    icon_back(22, 28, C_SUB2);
    touch_zone(0, 0, 200, 56, SCE_CTRL_CIRCLE);
    txt(F_XB, 44, 36, C_TEXT, 20, "Galería", -1);
    char title[96];
    chat_title(title, sizeof(title));
    txt_right(F_REG, SCREEN_W - 18, 33, C_SUB, 13, title);

    int x = 20;
    x += keycap(x, 78, "L") + 8;
    for (int i = 0; i < 3; i++) {
        pill(x, 78, FOLDERS[i], gal_folder == i);
        x += pill_w(FOLDERS[i], gal_folder == i) + 8;
    }
    keycap(x, 78, "R");
    if (gal_n > 0) {
        char c[32];
        snprintf(c, sizeof(c), "%d / %d", gal_sel + 1, gal_n);
        txt_right(F_REG, SCREEN_W - 20, 83, C_FAINT, 12, c);
    }

    if (gal_n == 0) {
        icon_image(480, 250, 20, C_SUB);
        txt_center(F_BOLD, 480, 300, C_TEXT, 16, "No hay imágenes aquí");
        txt_center(F_REG, 480, 324, C_SUB, 13, gal_folder == GAL_SHOTS ? "Haz una captura con PS + START, o mira otra carpeta con L / R" : "Toma fotos con la app Cámara de la Vita");
    }

    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(0, G_TOP - 6, SCREEN_W, G_BOT);
    for (int i = 0; i < gal_n; i++) {
        int row = i / G_COLS, col = i % G_COLS;
        float cx = G_X + col * (G_CW + G_GAP), cy = G_TOP + row * (G_CH + G_GAP) - gal_sc.pos;
        if (cy > G_BOT) break;
        if (cy + G_CH < G_TOP - 6) continue;
        if (i == gal_sel) rrect(cx - 4, cy - 4, G_CW + 8, G_CH + 8, 14, C_ACC);
        vita2d_texture *t = gallery_thumb(i);
        rrect(cx, cy, G_CW, G_CH, 10, C_PHOTO_BG);
        if (t) draw_media_cover(t, cx, cy, G_CW, G_CH);
        else spinner(cx + G_CW / 2, cy + G_CH / 2, frame);
    }
    vita2d_disable_clipping();

    footer_bar();
    x = 18;
    x = hint(x, 526, GLYPH_CROSS, "Enviar");
    x = hint(x, 526, GLYPH_SQUARE, "Enviar con texto");
    x = hint(x, 526, GLYPH_CIRCLE, "Volver");
    txt_right(F_REG, SCREEN_W - 18, 531, C_FAINT, 13, "L / R  Carpeta");
}
