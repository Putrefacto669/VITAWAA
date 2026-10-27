// Notificaciones (campanita): mensajes nuevos, reacciones a tus mensajes y a tus estados.
#include "app.h"
#include "touch.h"
#include "audio.h"

#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>

#define ROW_H 70
static int sel = 0, back_to = SCR_HOME;
static float scroll_y = 0;

void notifs_enter(int from) {
    back_to = from == SCR_NOTIFS ? SCR_HOME : from;
    sel = 0;
    scroll_y = 0;
    sync_lock();
    g_model.notif_unseen = 0;
    sync_unlock();
    go(SCR_NOTIFS);
}

static void open_sel(void) {
    if (sel < 0 || sel >= view.nnotifs) return;
    const Notif *n = &view.notifs[sel];
    if (!n->jid[0] || !strcmp(n->kind, "slike")) return;
    open_chat(n->jid);
}

void input_notifs(void) {
    if (btn(SCE_CTRL_CIRCLE)) { go_back_sound(back_to); return; }
    if (rep(SCE_CTRL_DOWN) && sel < view.nnotifs - 1) { sel++; audio_sfx(SFX_MOVE); }
    if (rep(SCE_CTRL_UP) && sel > 0) { sel--; audio_sfx(SFX_MOVE); }
    if (btn(SCE_CTRL_CROSS)) open_sel();
}

void draw_notifs(void) {
    header_bar();
    icon_back(18, 28, C_TEXT);
    if (tap_in(0, 0, 60, 56)) press_next(SCE_CTRL_CIRCLE);
    txt(F_XB, 40, 36, C_TEXT, 20, "Notificaciones", -1);
    txt_right(F_REG, SCREEN_W - 18, 33, C_SUB, 13, "Mensajes, reacciones y tus estados");

    float top = 68, vis = SCREEN_H - 36 - top;
    float want = sel * ROW_H - vis / 2 + ROW_H / 2;
    if (want < 0) want = 0;
    float maxs = view.nnotifs * ROW_H - vis;
    if (maxs < 0) maxs = 0;
    if (want > maxs) want = maxs;
    scroll_y += (want - scroll_y) * 0.25f;

    if (!view.nnotifs) center_msg("Nada nuevo por aquí", "Aquí verás quién te escribe o reacciona a tus mensajes y estados.", C_TEXT, 0);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(0, (int)top, SCREEN_W, (int)(top + vis));
    for (int i = 0; i < view.nnotifs; i++) {
        const Notif *n = &view.notifs[i];
        float x = 120, w = SCREEN_W - 240, y = top + 4 + i * ROW_H - scroll_y;
        if (y + ROW_H < top || y > top + vis) continue;
        if (i == sel) focus_card(x, y, w, ROW_H - 8, 16);
        else { shadow(x, y, w, ROW_H - 8, 16); card(x, y, w, ROW_H - 8, 16, C_CARD, C_LINE2); gloss(x, y, w, ROW_H - 8, 16); }
        unsigned int k = !strcmp(n->kind, "msg") ? C_ACC : !strcmp(n->kind, "react") ? HEX(0xBE2D7A) : HEX(0xE0533D);
        rrect(x + 10, y + 12, 4, ROW_H - 32, 2, k);
        avatar(x + 48, y + (ROW_H - 8) / 2, 22, n->title, hash_color(n->jid), n->jid);
        char t1[256], t2[256];
        notif_line(n, t1, sizeof(t1), t2, sizeof(t2));
        txt_fit(F_BOLD, (int)x + 82, (int)y + 27, C_TEXT, 15, t1, (int)w - 160);
        txt_fit(F_REG, (int)x + 82, (int)y + 48, C_SUB, 13, t2, (int)w - 100);
        txt_right(F_REG, (int)(x + w - 14), (int)y + 25, C_FAINT, 11, n->time);
        if (tap_in(x, y, w, ROW_H - 8) && y >= top) {
            if (sel == i) press_next(SCE_CTRL_CROSS);
            sel = i;
        }
    }
    vita2d_disable_clipping();

    footer_bar();
    int x = 18;
    x = hint(x, 526, GLYPH_CROSS, "Abrir chat");
    hint(x, 526, GLYPH_CIRCLE, "Volver");
}
