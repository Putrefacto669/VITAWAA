// VitaWA — cliente de WhatsApp para PS Vita (fase 3)
//
// La Vita no habla con WhatsApp: habla con el bridge (bridge/server.mjs) por un
// canal cifrado. sync.c baja los datos en hilos aparte; las pantallas solo dibujan
// y leen los botones y la pantalla tactil.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <psp2/notificationutil.h>
#include <vita2d.h>

#include "app.h"
#include "net.h"
#include "ime.h"
#include "tex.h"
#include "touch.h"
#include "audio.h"
#include "camera.h"
#include "gallery.h"
#include "video.h"

Config cfg;
int g_body = 16;
Model view;
int screen = SCR_PAIR;
int frame = 0;
int connected = 0;
unsigned int pressed = 0;
int v_chats = -1, v_conv = -1, v_st = -1, v_sitems = -1, v_stk = -1, v_contact = -1, v_search = -1, v_check = -1;
static int v_toast = 0, v_qr = -1, v_incoming = 0, v_notif = -1;
static Notif banners[8];
static int nbanners = 0, banner_frames = 0;
static int wa_mode = 1;        // vincular WhatsApp: 1 = con número (código), 0 = con QR
static int net_ok = 1;
static int ime_purpose = IMEP_NONE;

static char toast[96];
static int toast_err = 0;
static int toast_frames = 0;

// ---------- botones ----------
static SceCtrlData pad, old_pad;
static int hold_frames[32];

int btn(unsigned int b) { return (pressed & b) != 0; }
int held(unsigned int b) { return (pad.buttons & b) != 0; }

static unsigned int queued_press = 0;
void press_next(unsigned int b) { queued_press |= b; }

int rep(unsigned int b) {
    int bit = __builtin_ctz(b);
    if (pressed & b) return 1;
    if (pad.buttons & b) {
        int h = hold_frames[bit];
        return h > 24 && (h - 24) % 5 == 0;
    }
    return 0;
}

static void read_input(void) {
    old_pad = pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    pressed = pad.buttons & ~old_pad.buttons;
    for (int i = 0; i < 32; i++) {
        if (pad.buttons & (1u << i)) hold_frames[i]++;
        else hold_frames[i] = 0;
    }
    touch_update();
    touch_zones_reset();
    pressed |= touch_zone_buttons();   // tocar una ayuda de abajo = apretar ese boton
    pressed |= queued_press;
    queued_press = 0;
}

void show_toast(const char *msg, int err) {
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_err = err;
    toast_frames = 170;
}

void go(int scr) {
    audio_sfx(SFX_SELECT);
    screen = scr;
}

void go_back_sound(int scr) {
    audio_sfx(SFX_BACK);
    screen = scr;
}

void ime_ask(int purpose, const char *title, const char *initial, int kind, int maxlen) {
    if (ime_open(title, initial, kind, maxlen) >= 0) ime_purpose = purpose;
}

// ---------- aviso del sistema (centro de notificaciones de la Vita) ----------
static int notif_util_ok = 0;
static void sys_notify(const Notif *n) {
    if (!cfg.sysnotif || !notif_util_ok) return;
    char t[128], b[200], line[400], clean[400];
    notif_line(n, t, sizeof(t), b, sizeof(b));
    snprintf(line, sizeof(line), "%s: %s", t, b);
    int k = 0;   // sin marcadores de emoji (la fuente del sistema no los tiene)
    for (const char *p = line; *p && k < (int)sizeof(clean) - 1; p++) {
        if (*p == '\x1E') { while (*p && *p != '\x1F') p++; if (!*p) break; continue; }
        clean[k++] = *p;
    }
    clean[k] = 0;
    SceWChar16 w[SCE_NOTIFICATIONUTIL_TEXT_MAX + 1];
    int o = 0;
    const unsigned char *u = (const unsigned char *)clean;
    while (*u && o < SCE_NOTIFICATIONUTIL_TEXT_MAX) {
        unsigned int cp;
        if (*u < 0x80) cp = *u++;
        else if ((*u & 0xE0) == 0xC0 && u[1]) { cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); u += 2; }
        else if ((*u & 0xF0) == 0xE0 && u[1] && u[2]) { cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); u += 3; }
        else { u++; while ((*u & 0xC0) == 0x80) u++; continue; }   // fuera del plano básico: se omite
        w[o++] = (SceWChar16)cp;
    }
    w[o] = 0;
    sceNotificationUtilSendNotification(w);
}

// ---------- copia del modelo ----------

static void refresh_view(void) {
    int conv_changed = 0;
    sync_lock();
    view.net = g_model.net;
    view.http_err = g_model.http_err;
    view.seq = g_model.seq;
    memcpy(view.state, g_model.state, sizeof(view.state));
    memcpy(view.me_name, g_model.me_name, sizeof(view.me_name));
    memcpy(view.me_num, g_model.me_num, sizeof(view.me_num));
    memcpy(view.me_jid, g_model.me_jid, sizeof(view.me_jid));
    memcpy(view.version, g_model.version, sizeof(view.version));
    view.unread = g_model.unread;
    if (g_model.qr_version != v_qr) {
        v_qr = g_model.qr_version;
        view.qr_size = g_model.qr_size;
        view.qr_version = g_model.qr_version;
        view.qr_left = g_model.qr_left;
        view.qr_at = g_model.qr_at;
        memcpy(view.qr, g_model.qr, sizeof(view.qr));
    }
    if (g_model.chats_ver != v_chats) {
        v_chats = g_model.chats_ver;
        view.nchats = g_model.nchats;
        memcpy(view.chats, g_model.chats, sizeof(Chat) * g_model.nchats);
    }
    if (g_model.conv_ver != v_conv) {
        v_conv = g_model.conv_ver;
        memcpy(&view.conv, &g_model.conv, sizeof(Conv));   // trae lay_w = 0: se vuelve a maquetar
        conv_changed = 1;
    }
    if (g_model.st_ver != v_st) {
        v_st = g_model.st_ver;
        view.nstatus = g_model.nstatus;
        memcpy(view.status, g_model.status, sizeof(StatusRow) * g_model.nstatus);
    }
    if (g_model.sitems_ver != v_sitems) {
        v_sitems = g_model.sitems_ver;
        view.nsitems = g_model.nsitems;
        memcpy(view.sitems_poster, g_model.sitems_poster, sizeof(view.sitems_poster));
        memcpy(view.sitems, g_model.sitems, sizeof(StatusItem) * g_model.nsitems);
    }
    if (g_model.stk_ver != v_stk) {
        v_stk = g_model.stk_ver;
        view.nstk = g_model.nstk;
        view.stk_tab = g_model.stk_tab;
        memcpy(view.stk, g_model.stk, sizeof(StickerRef) * g_model.nstk);
    }
    if (g_model.contact_ver != v_contact) {
        v_contact = g_model.contact_ver;
        view.contact = g_model.contact;
    }
    if (g_model.search_ver != v_search) {
        v_search = g_model.search_ver;
        view.nsearch = g_model.nsearch;
        memcpy(view.search, g_model.search, sizeof(ContactRow) * g_model.nsearch);
    }
    if (g_model.check_ver != v_check) {
        v_check = g_model.check_ver;
        view.check = g_model.check;
    }
    if (g_model.toast_ver != v_toast) {
        v_toast = g_model.toast_ver;
        show_toast(g_model.toast, g_model.toast_err);
    }
    if (g_model.notif_ver != v_notif) {
        v_notif = g_model.notif_ver;
        view.nnotifs = g_model.nnotifs;
        memcpy(view.notifs, g_model.notifs, sizeof(Notif) * g_model.nnotifs);
    }
    view.notif_unseen = g_model.notif_unseen;
    memcpy(view.paircode, g_model.paircode, sizeof(view.paircode));
    int newb = 0;
    Notif fresh[6];
    int nfresh = 0;
    for (int i = 0; i < g_model.nbanner && nbanners < 8; i++) {
        banners[nbanners++] = g_model.banner[i];
        if (nfresh < 6) fresh[nfresh++] = g_model.banner[i];
        newb++;
    }
    g_model.nbanner = 0;
    if (g_model.video_ver != view.video_ver) {
        view.video_ver = g_model.video_ver;
        view.video_state = g_model.video_state;
        memcpy(view.video_id, g_model.video_id, sizeof(view.video_id));
        memcpy(view.video_path, g_model.video_path, sizeof(view.video_path));
    }
    if (g_model.emoji_ver != view.emoji_ver) {   // los nombres se comparten (sync no los libera)
        view.emoji_ver = g_model.emoji_ver;
        view.ncats = g_model.ncats;
        memcpy(view.cats, g_model.cats, sizeof(view.cats));
    }
    int incoming = g_model.incoming;
    sync_unlock();
    for (int i = 0; i < nfresh; i++) sys_notify(&fresh[i]);
    if (newb && strcmp(banners[nbanners - 1].kind, "msg")) audio_sfx(SFX_RECEIVE);   // msg ya suena por "incoming"

    if (conv_changed) chat_on_conv_update();
    if (incoming != v_incoming) {
        if (v_incoming) audio_sfx(SFX_RECEIVE);
        v_incoming = incoming;
    }
}

// ---------- utilidades compartidas ----------
const Chat *chat_by_jid(const char *jid) {
    for (int i = 0; i < view.nchats; i++)
        if (!strcmp(view.chats[i].jid, jid)) return &view.chats[i];
    return NULL;
}

void masked_number(char *out, int cap) {
    const char *n = view.me_num;
    int L = (int)strlen(n);
    if (L < 6) { snprintf(out, cap, "%s", n); return; }
    snprintf(out, cap, "%.*s••••", L - 4, n);   // "+505 8998 6853" -> "+505 8998 ••••"
}

void brand(void) {
    logo(18, 28, 30);
    txt(F_XB, 56, 36, C_TEXT, 22, "VitaWA", -1);
}

void center_msg(const char *title, const char *sub, unsigned int color, int spin) {
    if (spin) spinner(480, 230, frame);
    txt_center(F_XB, 480, spin ? 306 : 258, color, 24, title);
    if (sub) txt_center(F_REG, 480, spin ? 338 : 292, C_SUB, 15, sub);
}

void media_path(char *out, int cap, const char *jid, const char *id, int max) {
    char ej[128], ei[96];
    url_enc(jid, ej, sizeof(ej));
    url_enc(id, ei, sizeof(ei));
    snprintf(out, cap, "/v1/media?jid=%s&id=%s&max=%d", ej, ei, max);
}

void draw_media_cover(vita2d_texture *t, float x, float y, float bw, float bh) {
    float w = vita2d_texture_get_width(t), h = vita2d_texture_get_height(t);
    float s = bw / w > bh / h ? bw / w : bh / h;
    float sw = bw / s, sh = bh / s;
    vita2d_draw_texture_part_scale(t, x, y, (w - sw) / 2, (h - sh) / 2, sw, sh, s, s);
}

void draw_texture_contain(vita2d_texture *t, float x, float y, float bw, float bh) {
    float w = vita2d_texture_get_width(t), h = vita2d_texture_get_height(t);
    float s = bw / w < bh / h ? bw / w : bh / h;
    vita2d_draw_texture_scale(t, x + (bw - w * s) / 2, y + (bh - h * s) / 2, s, s);
}

// Stickers: pedimos la "tira" animada (anim=1). Si el sticker se mueve, el bridge
// devuelve los cuadros apilados en vertical (alto = ancho * cuadros) a 10 fps.
void sticker_path(char *out, int cap, const char *jid, const char *id, int max) {
    media_path(out, cap, jid, id, max);
    size_t n = strlen(out);
    snprintf(out + n, cap - n, "&anim=1");
}

void draw_sticker(vita2d_texture *t, float x, float y, float bw, float bh) {
    float w = vita2d_texture_get_width(t), h = vita2d_texture_get_height(t);
    if (h >= w * 1.9f) {
        int frames = (int)(h / w);
        int f = (frame / 6) % frames;            // 60 fps / 6 = 10 cuadros por segundo
        float s = bw / w < bh / w ? bw / w : bh / w;
        vita2d_draw_texture_part_scale(t, x + (bw - w * s) / 2, y + (bh - w * s) / 2, 0, f * w, w, w, s, s);
        return;
    }
    draw_texture_contain(t, x, y, bw, bh);
}

void photo_placeholder(float x, float y, float w, float h, const char *label) {
    rrect(x, y, w, h, 10, C_PHOTO_BG);
    icon_image(x + w / 2, y + h / 2 - 10, 14, C_SUB);
    txt_center(F_REG, (int)(x + w / 2), (int)(y + h / 2 + 24), C_SUB, 12, label);
}

// ---------- notificaciones ----------
void notif_line(const Notif *n, char *title, int tn, char *body, int bn) {
    if (!strcmp(n->kind, "react")) {
        snprintf(title, tn, "%s reaccionó %s", n->title, n->emoji);
        snprintf(body, bn, "a: %s", n->body);
    } else if (!strcmp(n->kind, "slike")) {
        snprintf(title, tn, "%s %s", n->title, n->emoji);
        snprintf(body, bn, "reaccionó a tu estado");
    } else {
        snprintf(title, tn, "%s", n->title);
        snprintf(body, bn, "%s", n->body);
    }
}

void bell(float cx, float cy, int unseen) {
    // campana dibujada: cuerpo + borde + badajo
    unsigned int c = C_SUB2;
    circle(cx, cy - 2, 8, c);
    rrect(cx - 8, cy - 2, 16, 8, 2, c);
    rrect(cx - 11, cy + 5, 22, 3, 1.5f, c);
    circle(cx, cy + 10, 2.6f, c);
    circle(cx, cy - 2, 5, C_BAR);
    rrect(cx - 5, cy - 2, 10, 7, 1, C_BAR);
    if (unseen > 0) {
        char b[8];
        snprintf(b, sizeof(b), "%d", unseen > 9 ? 9 : unseen);
        circle(cx + 9, cy - 9, 8, C_RED);
        txt_center(F_XB, (int)cx + 9, (int)cy - 5, C_WHITE, 11, b);
    }
    if (tap_in(cx - 18, cy - 20, 36, 40)) notifs_enter(screen);
}

// Banner que baja desde arriba (3.5 s). Tocarlo abre el chat.
static void draw_banner(void) {
    if (!nbanners) return;
    if (banner_frames <= 0) banner_frames = 210;
    banner_frames--;
    Notif *n = &banners[0];
    int t = 210 - banner_frames;
    float k = t < 14 ? t / 14.0f : banner_frames < 14 ? banner_frames / 14.0f : 1.0f;
    k = 1 - (1 - k) * (1 - k);
    float w = 560, h = 64, x = (SCREEN_W - w) / 2, y = -h - 10 + (h + 18) * k;
    shadow(x, y, w, h, 20);
    unsigned int accent = !strcmp(n->kind, "msg") ? C_ACC : !strcmp(n->kind, "react") ? HEX(0xBE2D7A) : HEX(0xE0533D);
    rrect(x, y, w, h, 20, C_CARD);
    rrect(x, y, 6, h, 3, accent);
    gloss(x, y, w, h, 20);
    avatar(x + 40, y + h / 2, 22, n->title, hash_color(n->jid), n->jid);
    char t1[256], t2[256];
    notif_line(n, t1, sizeof(t1), t2, sizeof(t2));
    txt_fit(F_BOLD, (int)x + 72, (int)y + 28, C_TEXT, 15, t1, (int)w - 140);
    txt_fit(F_REG, (int)x + 72, (int)y + 49, C_SUB, 13, t2, (int)w - 100);
    txt_right(F_REG, (int)(x + w - 14), (int)y + 26, C_FAINT, 11, n->time);
    int hit = tap_in(x, y, w, h);
    if (hit && n->jid[0] && strcmp(n->kind, "slike") && connected) {
        open_chat(n->jid);
        screen = SCR_CHAT;
    }
    if (banner_frames <= 0 || hit) {
        memmove(&banners[0], &banners[1], sizeof(Notif) * (nbanners - 1));
        nbanners--;
        banner_frames = 0;
    }
}

// ---------- QR de WhatsApp ----------
static vita2d_texture *qr_tex = NULL;
static int qr_tex_version = -1;

static void update_qr_texture(void) {
    if (view.qr_size <= 0 || view.qr_version == qr_tex_version) return;
    vita2d_wait_rendering_done();
    if (qr_tex) vita2d_free_texture(qr_tex);
    int n = view.qr_size;
    qr_tex = vita2d_create_empty_texture(n, n);
    if (!qr_tex) return;
    vita2d_texture_set_filters(qr_tex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    unsigned int stride = vita2d_texture_get_stride(qr_tex) / 4;
    unsigned int *px = vita2d_texture_get_datap(qr_tex);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            px[y * stride + x] = view.qr[y * n + x] ? C_TEXT : C_WHITE;
    qr_tex_version = view.qr_version;
}

static void ask_phone(void) {
    ime_ask(IMEP_WANUM, "Tu número de WhatsApp con código de país (ej. 50212345678)", cfg.phone, IME_KIND_NUMBER, 18);
}

static void wa_request_code(void) {
    if (!cfg.phone[0]) { ask_phone(); return; }
    show_toast("Pidiendo el código a WhatsApp…", 0);
    job_wapair(cfg.phone);
}

static void draw_wa_qr(void) {
    header_bar();
    brand();
    txt_right(F_REG, SCREEN_W - 18, 33, C_SUB, 13, "Paso 2 de 2 · vincular WhatsApp");

    float qx = 56, qy = 76, qs = 330;
    shadow(qx, qy, qs, qs, 22);
    rrect(qx, qy, qs, qs, 22, HEX(0xFFFFFF));
    gloss(qx, qy, qs, qs, 22);

    if (wa_mode == 1) {
        // ── vincular con número: código de 8 caracteres ──
        float cx = qx + qs / 2;
        if (view.paircode[0]) {
            txt_center(F_REG, (int)cx, (int)qy + 70, HEX(0x5B6876), 15, "Tu código de vinculación");
            // cada caracter en su "tecla", como en WhatsApp
            const char *code = view.paircode;
            int n = (int)strlen(code);
            float kw = 31, gap = 4, total = 0;
            for (int i = 0; i < n; i++) total += code[i] == '-' ? 14 : kw + gap;
            float kx = cx - total / 2;
            for (int i = 0; i < n; i++) {
                if (code[i] == '-') { rrect(kx + 3, qy + 148, 8, 3, 1.5f, HEX(0x8A97A5)); kx += 14; continue; }
                rrect(kx, qy + 118, kw, 50, 9, HEX(0xEEF4FB));
                rrect(kx, qy + 118, kw, 2, 1, C_ACC);
                char ch[2] = { code[i], 0 };
                txt_center(F_XB, (int)(kx + kw / 2), (int)qy + 154, HEX(0x1E2A36), 28, ch);
                kx += kw + gap;
            }
            txt_center(F_REG, (int)cx, (int)qy + 214, HEX(0x5B6876), 14, "Escríbelo en tu teléfono");
            char ph[48];
            snprintf(ph, sizeof(ph), "+%s", cfg.phone);
            txt_center(F_BOLD, (int)cx, (int)qy + 246, HEX(0x1E2A36), 16, ph);
            txt_center(F_REG, (int)cx, (int)qy + 296, HEX(0x6F7C89), 12, "¿Expiró? Pide otro con ×");
        } else {
            icon_people(cx, qy + 92, 26, C_ACC);
            txt_center(F_XB, (int)cx, (int)qy + 160, HEX(0x1E2A36), 22, "Vincula con tu número");
            txt_center(F_REG, (int)cx, (int)qy + 186, HEX(0x5B6876), 14, "Te damos un código de 8 caracteres");
            float bw2 = 220, bx2 = cx - bw2 / 2, by2 = qy + 214;
            rrect(bx2, by2, bw2, 48, 24, C_ACC);
            gloss(bx2, by2, bw2, 48, 24);
            txt_center(F_BOLD, (int)cx, (int)by2 + 31, HEX(0xFFFFFF), 16, cfg.phone[0] ? "Pedir código" : "Escribir mi número");
            if (tap_in(bx2, by2, bw2, 48)) press_next(SCE_CTRL_CROSS);
            if (cfg.phone[0]) {
                char ph[64];
                snprintf(ph, sizeof(ph), "+%s · cambiar", cfg.phone);
                txt_center(F_REG, (int)cx, (int)qy + 294, C_ACC, 13, ph);
                if (tap_in(cx - 120, qy + 276, 240, 30)) ask_phone();
            }
        }
    } else if (qr_tex && view.qr_size > 0) {
        float inner = qs - 48;
        float sc = inner / view.qr_size;
        vita2d_draw_texture_scale(qr_tex, qx + 24, qy + 24, sc, sc);
    } else {
        spinner(qx + qs / 2, qy + qs / 2, frame);
    }
    if (wa_mode == 0) {
        int left = view.qr_left - (int)((now_ms() - view.qr_at) / 1000);
        if (left < 0) left = 0;
        rrect(qx, qy + qs + 16, qs, 4, 2, C_TRACK);
        rrect(qx, qy + qs + 16, qs * left / 20.0f, 4, 2, C_ACC);
        char b[64];
        snprintf(b, sizeof(b), "El código se renueva en %d s", left);
        txt_center(F_REG, (int)(qx + qs / 2), (int)(qy + qs + 40), C_SUB, 13, b);
    } else {
        txt_center(F_REG, (int)(qx + qs / 2), (int)(qy + qs + 34), C_SUB, 13, "□ / toca aquí para usar el QR");
        if (tap_in(qx, qy + qs + 14, qs, 30)) press_next(SCE_CTRL_SQUARE);
    }

    float tx = qx + qs + 50;
    txt(F_XB, (int)tx, 142, C_TEXT, 30, "Vincula tu WhatsApp", -1);
    txt(F_REG, (int)tx, 170, C_SUB, 15, "La Vita queda como un dispositivo vinculado.", -1);
    const char *stepsQr[3] = {"Abre WhatsApp en tu teléfono", NULL, "Apunta la cámara a este código"};
    const char *stepsCode[3] = {"Abre WhatsApp en tu teléfono", NULL, "«Vincular con número» y escribe el código"};
    const char **steps = wa_mode ? stepsCode : stepsQr;
    float sy = 216;
    for (int i = 0; i < 3; i++) {
        char n[4];
        snprintf(n, sizeof(n), "%d", i + 1);
        circle(tx + 13, sy + i * 46, 13, C_ACC);
        txt_center(F_XB, (int)tx + 13, (int)(sy + i * 46 + 5), C_WHITE, 14, n);
        if (steps[i]) {
            txt_fit(F_REG, (int)tx + 38, (int)(sy + i * 46 + 6), C_TEXT, 16, steps[i], (int)(SCREEN_W - 24 - tx - 38));
        } else {
            const char *chips[3] = {"Ajustes", "Dispositivos vinculados", "Vincular un dispositivo"};
            float cx = tx + 38, cy = sy + i * 46 - 12;
            for (int k = 0; k < 3; k++) {
                int w = txt_w(F_REG, 13, chips[k], -1) + 16;
                if (cx + w > SCREEN_W - 20) { cx = tx + 38; cy += 28; }
                card(cx, cy, w, 24, 7, C_CARD, C_LINE);
                txt(F_REG, (int)cx + 8, (int)cy + 17, C_TEXT, 13, chips[k], -1);
                cx += w + 4;
                if (k < 2) { txt(F_REG, (int)cx, (int)cy + 17, C_SUB, 13, "›", -1); cx += 10; }
            }
        }
    }
    float wy = 392;
    rrect(tx, wy, SCREEN_W - 20 - tx, 50, 12, C_WARN_BG);
    txt(F_REG, (int)tx + 14, (int)wy + 21, C_WARN_TX, 13, "Cliente no oficial: mientras pruebas,", -1);
    txt(F_REG, (int)tx + 14, (int)wy + 39, C_WARN_TX, 13, "mejor usa un número secundario.", -1);

    footer_bar();
    int x = 18;
    if (wa_mode) x = hint(x, 526, GLYPH_CROSS, view.paircode[0] ? "Otro código" : (cfg.phone[0] ? "Pedir código" : "Escribir número"));
    x = hint(x, 526, GLYPH_SQUARE, wa_mode ? "Usar QR" : "Usar número");
    hint(x, 526, GLYPH_TRIANGLE, "Ajustes");
    txt_right(F_REG, SCREEN_W - 18, 531, C_FAINT, 13, "La pantalla no se apaga mientras vinculas");
}

static void input_wa_qr(void) {
    if (btn(SCE_CTRL_SQUARE)) { wa_mode = !wa_mode; audio_sfx(SFX_MOVE); }
    if (wa_mode && btn(SCE_CTRL_CROSS)) wa_request_code();
    if (wa_mode && btn(SCE_CTRL_START)) ask_phone();
}

static void draw_connection_states(void) {
    header_bar();
    brand();
    if (view.net == NET_IDLE) {
        center_msg("Buscando el bridge…", cfg.url, C_TEXT, 1);
    } else if (view.net == NET_UNREACHABLE) {
        center_msg("No encuentro el bridge", cfg.url, C_RED_DARK, 0);
        txt_center(F_REG, 480, 330, C_SUB, 14, "Revisa que esté encendido y que la Vita tenga internet. Reintentando solo…");
    } else if (view.net == NET_BADKEY) {
        center_msg("El bridge ya no reconoce esta Vita", "Probablemente se creó un código nuevo. Empareja otra vez.", C_RED_DARK, 0);
    } else if (!strcmp(view.state, "logged_out")) {
        center_msg("Sesión cerrada", "Generando un QR nuevo…", C_TEXT, 1);
    } else {
        center_msg("Conectando a WhatsApp…", "El bridge está hablando con los servidores.", C_TEXT, 1);
    }
    footer_bar();
    int x = 18;
    if (view.net == NET_BADKEY) x = hint(x, 526, GLYPH_CROSS, "Emparejar otra vez");
    hint(x, 526, GLYPH_TRIANGLE, "Ajustes");
}

// ---------- teclado ----------
static void handle_ime_result(void) {
    if (!ime_is_open()) return;
    static char out[2100];
    int r = ime_poll(out, sizeof(out));
    if (r == 0) return;
    int purpose = ime_purpose;
    ime_purpose = IMEP_NONE;
    char *s = out;
    if (r == 1) {
        while (*s == ' ' || *s == '\n') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\n')) *--e = '\0';
    } else {
        out[0] = '\0';
    }
    const char *res = r == 1 ? s : NULL;
    switch (purpose) {
    case IMEP_MSG:
    case IMEP_EDIT:
    case IMEP_FIND: chat_ime_result(purpose, res); break;
    case IMEP_CAPTION:
        if (screen == SCR_CAMERA) camera_ime_result(res);
        else gallery_ime_result(res);
        break;
    case IMEP_STATUS_REPLY:
    case IMEP_STATUS_POST: status_ime_result(purpose, res); break;
    case IMEP_SEARCH:
    case IMEP_NUMBER: newchat_ime_result(purpose, res); break;
    case IMEP_PAIRCODE: if (res) pair_ime_result(res); break;
    case IMEP_WANUM:
        if (res) {
            char d[24]; int n = 0;
            for (const char *p = res; *p && n < 20; p++) if (*p >= '0' && *p <= '9') d[n++] = *p;
            d[n] = 0;
            if (n < 8) { show_toast("Incluye el código de país (ej. 502…)", 1); break; }
            snprintf(cfg.phone, sizeof(cfg.phone), "%s", d);
            config_save(&cfg);
            wa_request_code();
        }
        break;
    }
}

// ---------- main ----------
int main(void) {
    // pool de vertices de 4 MB (el de fabrica es 1 MB y se quedaba corto con tanta figura)
    vita2d_init_advanced(4 * 1024 * 1024);
    vita2d_set_clear_color(C_BG);
    ui_init();
    tex_init();
    touch_init();
    audio_init();
    net_ok = net_init() >= 0;

    int paired = config_load(&cfg);
    ui_set_theme(cfg.theme);
    ui_set_accent(cfg.accent);
    g_body = cfg.fontsize ? 19 : 16;
    notif_util_ok = sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL) >= 0;
    vita2d_set_clear_color(C_BG);
    audio_set_sounds(cfg.sounds);
    sync_start(&cfg);
    if (paired) screen = SCR_HOME;
    else pair_enter();

    memset(&pad, 0, sizeof(pad));
    SceRtcTick last_tick;
    sceRtcGetCurrentTick(&last_tick);
    for (;;) {
        frame++;
        // ¿volvimos de la suspensión? (el reloj saltó más de 3 s entre dos cuadros)
        SceRtcTick now_tick;
        sceRtcGetCurrentTick(&now_tick);
        if (now_tick.tick - last_tick.tick > 3000000ULL && net_ok) {
            net_reset();
            show_toast("Reconectando…", 0);
        }
        last_tick = now_tick;
        read_input();
        refresh_view();
        connected = net_ok && view.net == NET_OK && !strcmp(view.state, "open");
        int standalone = screen == SCR_PAIR || screen == SCR_SETTINGS;

        // ---- entrada ----
        if (ime_is_open()) {
            pressed = 0;
        } else if (screen == SCR_PAIR) {
            input_pair();
        } else if (screen == SCR_SETTINGS) {
            input_settings();
        } else if (!connected) {
            if (view.net == NET_OK && !strcmp(view.state, "qr")) input_wa_qr();
            if (btn(SCE_CTRL_TRIANGLE)) go(SCR_SETTINGS);
            if (view.net == NET_BADKEY && btn(SCE_CTRL_CROSS)) pair_enter();
        } else {
            switch (screen) {
            case SCR_HOME: input_home(); break;
            case SCR_CHAT: input_chat(); break;
            case SCR_FORWARD: input_forward(); break;
            case SCR_STATUS: input_status(); break;
            case SCR_CONTACT: input_contact(); break;
            case SCR_NEWCHAT: input_newchat(); break;
            case SCR_CAMERA: input_camera(); break;
            case SCR_GALLERY: input_gallery(); break;
            case SCR_NOTIFS: input_notifs(); break;
            }
        }
        if (screen == SCR_HOME) home_after_input();

        update_qr_texture();
        tex_pump();
        gallery_pump();
        cam_update();

        // ---- dibujo ----
        vita2d_start_drawing();
        vita2d_clear_screen();
        if (screen != SCR_CAMERA) ui_background(frame);   // cielo Aero detrás de todo

        if (!net_ok) {
            header_bar();
            brand();
            center_msg("No pude iniciar la red de la Vita", "Revisa que el Wi-Fi esté encendido y reinicia la app.", C_RED_DARK, 0);
        } else if (screen == SCR_PAIR) {
            draw_pair();
        } else if (screen == SCR_SETTINGS) {
            draw_settings();
        } else if (view.net == NET_OK && !strcmp(view.state, "qr")) {
            draw_wa_qr();
            if (btn(SCE_CTRL_TRIANGLE)) go(SCR_SETTINGS);
        } else if (!connected && !standalone) {
            draw_connection_states();
        } else {
            switch (screen) {
            case SCR_HOME: draw_home(); break;
            case SCR_CHAT: draw_chat(); break;
            case SCR_FORWARD: draw_forward(); break;
            case SCR_STATUS: draw_status_viewer(); break;
            case SCR_CONTACT: draw_contact(); break;
            case SCR_NEWCHAT: draw_newchat(); break;
            case SCR_CAMERA: draw_camera(); break;
            case SCR_GALLERY: draw_gallery(); break;
            case SCR_NOTIFS: draw_notifs(); break;
            }
        }

        if (connected) draw_banner();

        if (toast_frames > 0) {
            toast_frames--;
            int w = txt_w(F_BOLD, 14, toast, -1) + 40;
            float a = toast_frames < 20 ? toast_frames / 20.0f : 1.0f;
            unsigned int bg = toast_err ? HEXA(0xB83A27, (int)(0xF2 * a)) : HEXA(0x1E2A36, (int)(0xEC * a));
            rrect(480 - w / 2.0f, 68, w, 38, 19, bg);
            txt_center(F_BOLD, 480, 92, HEXA(0xFFFFFF, (int)(0xFF * a)), 14, toast);
        }

        vita2d_end_drawing();
        vita2d_common_dialog_update();   // necesario para que se vea el teclado
        vita2d_swap_buffers();

        handle_ime_result();

        // que no se apague la pantalla mientras ves un QR, un estado, la camara o grabas
        if (!strcmp(view.state, "qr") || screen == SCR_STATUS || screen == SCR_PAIR || screen == SCR_CAMERA || audio_rec_active() || video_active())
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    }

    vita2d_fini();
    ui_term();
    net_term();
    sceKernelExitProcess(0);
    return 0;
}
