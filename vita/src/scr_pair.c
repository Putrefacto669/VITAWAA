// Emparejar la Vita con el bridge (QR con la camara o codigo escrito) y Ajustes.

#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <vita2d.h>

#include "app.h"
#include "chan.h"
#include "camera.h"
#include "audio.h"
#include "ime.h"
#include "tex.h"
#include "touch.h"

// ---------- emparejar ----------
static int cam_ok = 0;
static int cam_back = 1;
static int pair_from_settings = 0;

static void start_camera(void) {
    cam_close();
    cam_ok = cam_open(cam_back) == 0;
    if (cam_ok) cam_scan(1, "VWA1 ");
}

void pair_enter(void) {
    pair_from_settings = cfg.paired && screen == SCR_SETTINGS;
    screen = SCR_PAIR;
    start_camera();
}

static int apply_pairing(const char *text) {
    char url[160];
    unsigned char key[32];
    if (!chan_parse_pairing(text, url, sizeof(url), key)) {
        // tambien aceptamos el codigo sin el "VWA1" del principio
        char tmp[300];
        snprintf(tmp, sizeof(tmp), "VWA1 %s", text);
        if (!chan_parse_pairing(tmp, url, sizeof(url), key)) return 0;
    }
    snprintf(cfg.url, sizeof(cfg.url), "%s", url);
    memcpy(cfg.key, key, 32);
    cfg.paired = 1;
    config_save(&cfg);
    tex_reset();
    sync_set_config(&cfg);
    cam_close();
    audio_sfx(SFX_SEND);
    show_toast("Vita emparejada con el bridge", 0);
    screen = SCR_HOME;
    return 1;
}

void pair_ime_result(const char *text) {
    if (!apply_pairing(text)) {
        audio_sfx(SFX_ERROR);
        show_toast("Ese código no es válido. Revisa que esté completo.", 1);
    }
}

static void leave_pair(void) {
    cam_close();
    go_back_sound(pair_from_settings ? SCR_SETTINGS : SCR_SETTINGS);
}

void input_pair(void) {
    char qr[300];
    if (cam_ok && cam_qr_result(qr, sizeof(qr))) {
        if (!apply_pairing(qr)) {
            audio_sfx(SFX_ERROR);
            show_toast("Leí un código, pero no es de VitaWA", 1);
        }
        return;
    }
    if (btn(SCE_CTRL_TRIANGLE)) {
        cam_back ^= 1;
        audio_sfx(SFX_MOVE);
        start_camera();
    }
    if (btn(SCE_CTRL_SQUARE)) {
        audio_sfx(SFX_SELECT);
        ime_ask(IMEP_PAIRCODE, "Código de emparejamiento (VWA1 http://… clave)", "VWA1 http://", IME_KIND_URL, 280);
    }
    if (btn(SCE_CTRL_CIRCLE) || btn(SCE_CTRL_SELECT)) leave_pair();
}

static void scan_frame(float x, float y, float w, float h) {
    // esquinas del visor
    float L = 34, t = 4;
    unsigned int c = C_WHITE;
    vita2d_draw_rectangle(x, y, L, t, c);
    vita2d_draw_rectangle(x, y, t, L, c);
    vita2d_draw_rectangle(x + w - L, y, L, t, c);
    vita2d_draw_rectangle(x + w - t, y, t, L, c);
    vita2d_draw_rectangle(x, y + h - t, L, t, c);
    vita2d_draw_rectangle(x, y + h - L, t, L, c);
    vita2d_draw_rectangle(x + w - L, y + h - t, L, t, c);
    vita2d_draw_rectangle(x + w - t, y + h - L, t, L, c);
    // linea que barre
    float k = (frame % 120) / 120.0f;
    float ly = y + 16 + (h - 32) * (k < 0.5f ? k * 2 : 2 - k * 2);
    vita2d_draw_rectangle(x + 16, ly, w - 32, 2, HEXA(0xFFFFFF, 0xB0));
}

void draw_pair(void) {
    header_bar();
    brand();
    txt_right(F_REG, SCREEN_W - 18, 33, C_SUB, 13, cfg.paired ? "Emparejar otra vez" : "Paso 1 de 2 · emparejar con tu bridge");

    // vista de la camara
    float vx = 40, vy = 76, vw = 440, vh = 330;
    shadow(vx, vy, vw, vh, 18);
    rrect(vx, vy, vw, vh, 18, HEX(0x1E2A36));
    vita2d_texture *t = cam_ok ? cam_texture() : NULL;
    if (t) {
        float s = vh / (float)CAM_H;           // 330/480: recortamos los lados
        float dw = CAM_W * s;
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle((int)vx, (int)vy, (int)(vx + vw), (int)(vy + vh));
        if (cam_is_back()) vita2d_draw_texture_scale(t, vx + (vw - dw) / 2, vy, s, s);
        else vita2d_draw_texture_scale(t, vx + (vw + dw) / 2, vy, -s, s);   // la frontal como espejo
        vita2d_disable_clipping();
        float fs = 230;
        scan_frame(vx + (vw - fs) / 2, vy + (vh - fs) / 2, fs, fs);
    } else {
        icon_camera(vx + vw / 2, vy + vh / 2 - 20, 22, C_SUB);
        txt_center(F_BOLD, (int)(vx + vw / 2), (int)(vy + vh / 2 + 30), C_WHITE, 16, "No pude abrir la cámara");
        txt_center(F_REG, (int)(vx + vw / 2), (int)(vy + vh / 2 + 54), HEX(0xB8C3CE), 13, "Puedes escribir el código con □");
    }
    rrect(vx + 14, vy + vh - 40, 150, 28, 14, HEXA(0x000000, 0x80));
    txt(F_REG, (int)vx + 28, (int)(vy + vh - 21), C_WHITE, 13, cam_is_back() ? "Cámara trasera" : "Cámara frontal", -1);

    // que esta viendo el lector
    if (cam_ok) {
        int h = cam_scan_hint();
        const char *msg = h == 3 ? "Ese QR no es de VitaWA (¿es el de WhatsApp?)"
                        : h == 2 ? "Veo un código… acércate o aléjate un poco y no te muevas"
                        : "Buscando el código…";
        unsigned int bg = h == 3 ? HEX(0xFFF4E0) : h == 2 ? HEX(0xE3F4EA) : C_CARD;
        unsigned int fg = h == 3 ? C_WARN_TX : h == 2 ? HEX(0x1F7A47) : C_SUB2;
        int w = txt_w(F_BOLD, 13, msg, -1) + 28;
        rrect(vx + (vw - w) / 2, vy + vh + 14, w, 30, 15, bg);
        txt_center(F_BOLD, (int)(vx + vw / 2), (int)(vy + vh + 34), fg, 13, msg);
        txt_center(F_REG, (int)(vx + vw / 2), (int)(vy + vh + 66), C_FAINT, 12, "Tip: que el código salga grande y completo, a unos 30–40 cm, sin reflejos");
    }

    // instrucciones
    float tx = 520;
    txt(F_XB, (int)tx, 120, C_TEXT, 28, "Empareja tu Vita", -1);
    txt(F_REG, (int)tx, 148, C_SUB, 15, "Solo se hace una vez. Luego entras directo.", -1);
    const char *steps[3] = {
        "Enciende el bridge (en tu PC o en la nube)",
        "Abre su página o mira su consola: sale un QR",
        "Apunta la cámara de la Vita a ese código",
    };
    for (int i = 0; i < 3; i++) {
        float sy = 196 + i * 50;
        char n[4];
        snprintf(n, sizeof(n), "%d", i + 1);
        circle(tx + 13, sy, 13, C_ACC);
        txt_center(F_XB, (int)tx + 13, (int)sy + 5, C_WHITE, 14, n);
        txt_fit(F_REG, (int)tx + 38, (int)sy + 6, C_TEXT, 15, steps[i], SCREEN_W - 20 - (int)tx - 38);
    }
    card(tx, 350, SCREEN_W - 30 - tx, 58, 12, C_CARD, C_LINE);
    icon_lock(tx + 24, 379, 9, C_ONLINE);
    txt(F_BOLD, (int)tx + 44, 374, C_TEXT, 13, "Conexión cifrada de punta a punta", -1);
    txt(F_REG, (int)tx + 44, 393, C_SUB, 12, "Sin el código, nadie más puede usar tu bridge.", -1);

    footer_bar();
    int x = 18;
    x = hint(x, 526, GLYPH_TRIANGLE, "Cambiar cámara");
    x = hint(x, 526, GLYPH_SQUARE, "Escribir el código");
    x = hint(x, 526, GLYPH_CIRCLE, "Ajustes");
}

// ---------- ajustes ----------
enum { SET_BRIDGE, SET_PAIR, SET_ACCENT, SET_THEME, SET_FONT, SET_SOUNDS, SET_SYSNOTIF, SET_LOGOUT, SET_COUNT };
static int set_sel = 0;
static int logout_armed = 0;
static const char *ACCENT_NAMES[ACCENT_COUNT] = {"Azul", "Verde", "Naranja", "Morado"};

static void settings_back(void) {
    if (!cfg.paired) { pair_enter(); return; }
    go_back_sound(SCR_HOME);
}

void input_settings(void) {
    int old = set_sel;
    if (rep(SCE_CTRL_DOWN) && set_sel < SET_COUNT - 1) set_sel++;
    if (rep(SCE_CTRL_UP) && set_sel > 0) set_sel--;
    for (int i = 0; i < SET_COUNT; i++)
        if (tap_in(40, 66 + i * 55, 560, 50)) {
            if (set_sel == i) pressed |= SCE_CTRL_CROSS;
            set_sel = i;
        }
    if (set_sel != old) { audio_sfx(SFX_MOVE); logout_armed = 0; }

    int dir = btn(SCE_CTRL_RIGHT) ? 1 : btn(SCE_CTRL_LEFT) ? -1 : 0;
    if (set_sel == SET_ACCENT && (dir || btn(SCE_CTRL_CROSS))) {
        if (!dir) dir = 1;
        cfg.accent = (cfg.accent + dir + ACCENT_COUNT) % ACCENT_COUNT;
        ui_set_accent(cfg.accent);
        config_save(&cfg);
        audio_sfx(SFX_MOVE);
    }
    if (set_sel == SET_THEME && (dir || btn(SCE_CTRL_CROSS))) {
        cfg.theme = !cfg.theme;
        ui_set_theme(cfg.theme);
        vita2d_set_clear_color(C_BG);
        config_save(&cfg);
        audio_sfx(SFX_SELECT);
    }
    if (set_sel == SET_FONT && (dir || btn(SCE_CTRL_CROSS))) {
        cfg.fontsize = !cfg.fontsize;
        g_body = cfg.fontsize ? 19 : 16;
        config_save(&cfg);
        audio_sfx(SFX_SELECT);
    }
    if (set_sel == SET_SYSNOTIF && (dir || btn(SCE_CTRL_CROSS))) {
        cfg.sysnotif = !cfg.sysnotif;
        config_save(&cfg);
        audio_sfx(SFX_SELECT);
    }
    if (set_sel == SET_SOUNDS && (dir || btn(SCE_CTRL_CROSS))) {
        cfg.sounds = !cfg.sounds;
        audio_set_sounds(cfg.sounds);
        config_save(&cfg);
        audio_sfx(SFX_SELECT);
    }
    if (btn(SCE_CTRL_CROSS)) {
        if (set_sel == SET_BRIDGE || set_sel == SET_PAIR) {
            audio_sfx(SFX_SELECT);
            pair_enter();
            return;
        }
        if (set_sel == SET_LOGOUT) {
            if (!connected) {
                audio_sfx(SFX_ERROR);
                show_toast("Primero tiene que estar conectado", 1);
            } else if (!logout_armed) {
                logout_armed = 1;
                audio_sfx(SFX_ERROR);
                show_toast("Pulsa × otra vez para cerrar la sesión", 0);
            } else {
                logout_armed = 0;
                job_logout();
                go(SCR_HOME);
            }
        }
    }
    if (btn(SCE_CTRL_CIRCLE) || btn(SCE_CTRL_TRIANGLE)) settings_back();
}

static void set_row(int i, const char *title, const char *sub, const char *value) {
    float x = 40, y = 66 + i * 55, w = 560, h = 50;
    if (set_sel == i) focus_card(x, y, w, h, 14);
    else card(x, y, w, h, 14, C_CARD, C_LINE);
    unsigned int tc = i == SET_LOGOUT ? C_RED_DARK : C_TEXT;
    txt(F_BOLD, (int)x + 20, (int)y + 22, tc, 16, title, -1);
    if (sub) txt_fit(F_REG, (int)x + 20, (int)y + 41, C_SUB, 12, sub, (int)w - 200);
    if (value) txt_right(F_BOLD, (int)(x + w - 20), (int)y + 31, set_sel == i ? C_ACC : C_SUB2, 15, value);
}

void draw_settings(void) {
    header_bar();
    icon_back(22, 28, C_SUB2);
    touch_zone(0, 0, 160, 56, SCE_CTRL_CIRCLE);
    txt(F_XB, 44, 36, C_TEXT, 22, "Ajustes", -1);
    battery(SCREEN_W - 18, 28);

    char sub[200];
    snprintf(sub, sizeof(sub), "%s", cfg.paired ? cfg.url : "Sin emparejar");
    const char *st = !cfg.paired ? "—" : view.net == NET_OK ? "Conectado" : view.net == NET_BADKEY ? "Código viejo" : view.net == NET_UNREACHABLE ? "Sin respuesta" : "Buscando…";
    set_row(SET_BRIDGE, "Bridge", sub, st);
    set_row(SET_PAIR, "Emparejar otra vez", "Escanea el QR de un bridge nuevo o reiniciado", NULL);
    char acc[32];
    snprintf(acc, sizeof(acc), "‹  %s  ›", ACCENT_NAMES[cfg.accent % ACCENT_COUNT]);
    set_row(SET_ACCENT, "Color de acento", "Botones, burbujas propias y selección", acc);
    set_row(SET_THEME, "Tema", "Aero claro (cielo y vidrio) o Aero noche (azul y verde)", cfg.theme ? "‹  Noche  ›" : "‹  Claro  ›");
    set_row(SET_FONT, "Tamaño de letra", "Texto de los mensajes en las conversaciones", cfg.fontsize ? "‹  Grande  ›" : "‹  Normal  ›");
    set_row(SET_SOUNDS, "Sonidos del sistema", "Al moverte, enviar y recibir mensajes", cfg.sounds ? "Activados" : "Apagados");
    set_row(SET_SYSNOTIF, "Avisos de la Vita", "También en el centro de notificaciones de la Vita", cfg.sysnotif ? "Activados" : "Apagados");
    char who[160];
    if (view.me_name[0] || view.me_num[0]) snprintf(who, sizeof(who), "Desvincula a %s %s", view.me_name, view.me_num);
    else snprintf(who, sizeof(who), "Desvincula esta Vita de tu WhatsApp");
    set_row(SET_LOGOUT, logout_armed ? "¿Seguro? Pulsa × otra vez" : "Cerrar sesión de WhatsApp", who, NULL);

    // panel derecho: acento y acerca de
    float px = 630, pw = SCREEN_W - px - 30;
    card(px, 66, pw, 226, 16, C_CARD, C_LINE);
    txt(F_BOLD, (int)px + 18, 120, C_SUB, 12, "VISTA PREVIA", -1);
    rrect(px + 18, 136, pw - 90, 40, 14, C_BUBBLE);
    card(px + 18, 136, pw - 90, 40, 14, C_BUBBLE, C_BUBBLE_LN);
    txt(F_REG, (int)px + 32, 162, C_TEXT, 14, "¿Ya llegaste?", -1);
    rrect(px + 72, 186, pw - 90, 40, 14, C_MINE);
    txt(F_REG, (int)px + 86, 212, C_TEXT, 14, "Sí, desde la Vita", -1);
    for (int i = 0; i < ACCENT_COUNT; i++) {
        float cx = px + 30 + i * 36, cy = 260;
        if (i == cfg.accent) circle(cx, cy, 14, C_TEXT);
        circle(cx, cy, 11, accent_preview(i));
    }

    card(px, 306, pw, 140, 16, C_CARD, C_LINE);
    logo(px + 16, 340, 28);
    txt(F_XB, (int)px + 52, 346, C_TEXT, 18, "VitaWA", -1);
    char ver[64];
    snprintf(ver, sizeof(ver), "App 3.6 · bridge %s", view.version[0] ? view.version : "—");
    txt(F_REG, (int)px + 18, 378, C_SUB, 13, ver, -1);
    txt(F_REG, (int)px + 18, 398, C_SUB, 12, "Cliente no oficial. Hecho para la Vita.", -1);
    txt(F_REG, (int)px + 18, 418, C_FAINT, 11, "Twemoji (CC-BY 4.0) · M PLUS 1p (OFL)", -1);

    footer_bar();
    int x = 18;
    x = hint(x, 526, GLYPH_CROSS, set_sel == SET_ACCENT ? "Cambiar color" : set_sel == SET_THEME ? "Cambiar tema" : (set_sel == SET_SOUNDS || set_sel == SET_SYSNOTIF) ? "Activar / apagar" : set_sel == SET_FONT ? "Cambiar tamaño" : "Elegir");
    x = hint(x, 526, GLYPH_CIRCLE, cfg.paired ? "Volver" : "Emparejar");
    if (set_sel == SET_ACCENT) txt(F_REG, x, 531, C_SUB2, 13, "Izq / Der  Color", -1);
}
