// Inicio: pestañas Chats y Estados.

#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <vita2d.h>

#include "app.h"
#include "audio.h"
#include "ime.h"
#include "touch.h"

static int home_tab = 0;      // 0 chats, 1 estados
static int filter = 0;        // 0 todos, 1 no leidos, 2 grupos
static int chat_sel = 0;
static int fidx[MAX_CHATS], fcount = 0;
static char sel_jid[64];
static unsigned int sel_changed_at = 0;
static Scroller list_sc;

#define LIST_X 0
#define LIST_W 380
#define LIST_Y 100
#define LIST_B 508
#define ROW_H 66

static void build_filter(void) {
    fcount = 0;
    for (int i = 0; i < view.nchats; i++) {
        const Chat *c = &view.chats[i];
        // los archivados solo salen en su filtro (como en el teléfono)
        if (filter == 3) { if (!c->archived) continue; }
        else if (c->archived) continue;
        if (filter == 1 && c->unread <= 0) continue;
        if (filter == 2 && !c->group) continue;
        fidx[fcount++] = i;
    }
}

static int total_unread_chats(void) {
    int n = 0;
    for (int i = 0; i < view.nchats; i++) if (view.chats[i].unread > 0) n++;
    return n;
}

static int any_unseen_status(void) {
    for (int i = 0; i < view.nstatus; i++) if (view.status[i].unseen > 0 && strcmp(view.status[i].poster, "me")) return 1;
    return 0;
}

static int is_typing(const char *sub) { return strstr(sub, "escrib") || strstr(sub, "grabando"); }

// ---------- encabezado comun ----------
static int seg_x[2], seg_w[2];

static void draw_header(void) {
    header_bar();
    brand();
    const char *labels[2] = {"Chats", "Estados"};
    int x = 176;
    segmented(x, 28, labels, 2, home_tab, any_unseen_status() ? 1 : -1);
    int px = x + 3;
    for (int i = 0; i < 2; i++) {
        seg_w[i] = txt_w(F_BOLD, 15, labels[i], -1) + 36 + (i == 1 && any_unseen_status() ? 12 : 0);
        seg_x[i] = px;
        px += seg_w[i];
    }
    battery(SCREEN_W - 18, 28);
    char num[48], line[96];
    masked_number(num, sizeof(num));
    snprintf(line, sizeof(line), "%s", num);
    int w = txt_w(F_REG, 13, line, -1);
    int rx = SCREEN_W - 18 - 76;
    txt(F_REG, rx - w, 33, C_SUB, 13, line, -1);
    circle(rx - w - 10, 28, 4, C_ONLINE);
    bell(rx - w - 44, 27, view.notif_unseen);
}

// ---------- chats ----------
static void draw_chat_row(const Chat *c, float x, float y, float w, int sel) {
    if (sel) focus_card(x, y, w, ROW_H - 4, 14);
    avatar(x + 34, y + 31, 23, c->name, c->color, c->jid);
    int unread = c->unread > 0;
    int tw = txt_w(F_REG, 12, c->time, -1);
    txt(unread ? F_BOLD : F_REG, (int)(x + w - 12 - tw), (int)y + 25, unread ? C_ACC : C_FAINT, 12, c->time, -1);
    float ix = x + w - 12 - tw - 12;
    if (c->muted) { icon_mute(ix - 4, y + 21, 5, C_FAINT); ix -= 18; tw += 18; }
    if (c->pinned) { icon_pin(ix - 4, y + 21, 6, C_FAINT); tw += 16; }
    txt_fit(F_BOLD, (int)x + 68, (int)y + 26, C_TEXT, 16, c->name, (int)(w - 68 - tw - 22));
    int bw = 0;
    if (unread) {
        badge(x + w - 12, y + 43, c->unread);
        bw = 34;
    }
    float lx = x + 68;
    if (c->typing[0]) {
        // ⌨ "escribiendo…" en lugar del último mensaje, con puntitos animados
        int tw2 = txt_fit_w(F_BOLD, 13, c->typing, (int)(x + w - lx - 40 - bw));
        txt_fit(F_BOLD, (int)lx, (int)y + 48, C_ONLINE, 13, c->typing, (int)(x + w - lx - 40 - bw));
        typing_dots(lx + tw2 + 10, y + 44, C_ONLINE);
        return;
    }
    if (!strcmp(c->lastkind, "voice")) { icon_mic(lx + 5, y + 43, 5, C_SUB); lx += 15; }
    else if (!strcmp(c->lastkind, "image")) { icon_image(lx + 6, y + 43, 5, C_SUB); lx += 17; }
    txt_fit(F_REG, (int)lx, (int)y + 48, unread ? C_SUB2 : C_SUB, 13, c->last, (int)(x + w - lx - 12 - bw));
}

static void draw_preview_pane(void) {
    float px = LIST_W, pw = SCREEN_W - px;
    if (fcount == 0) {
        txt_center(F_REG, (int)(px + pw / 2), 280, C_SUB, 15, filter ? "Nada por aquí con este filtro" : "Tus chats aparecerán aquí");
        return;
    }
    const Chat *c = &view.chats[fidx[chat_sel]];
    vita2d_draw_rectangle(px, 56, pw, 56, C_BAR);
    vita2d_draw_rectangle(px, 111, pw, 1, C_LINE);
    avatar(px + 38, 84, 19, c->name, c->color, c->jid);
    int loaded = !strcmp(view.conv.jid, c->jid);
    txt_fit(F_BOLD, (int)px + 68, loaded && view.conv.sub[0] ? 80 : 89, C_TEXT, 16, c->name, (int)pw - 200);
    if (loaded && view.conv.sub[0])
        txt_fit(F_REG, (int)px + 68, 99, is_typing(view.conv.sub) ? C_ONLINE : C_SUB, 12, view.conv.sub, (int)pw - 200);
    glyph(GLYPH_SQUARE, SCREEN_W - 120, 84, 18, C_BAR);
    txt(F_REG, SCREEN_W - 106, 89, C_SUB2, 13, "Info", -1);
    touch_zone(SCREEN_W - 132, 64, 70, 40, SCE_CTRL_SQUARE);
    if (loaded) chat_draw_preview(px + 22, 112, pw - 44, 452 - 112);

    rrect(px + 22, 460, pw - 44, 40, 20, C_CARD);
    card(px + 22, 460, pw - 44, 40, 20, C_CARD, C_LINE);
    glyph(GLYPH_CROSS, px + 46, 480, 18, C_CARD);
    txt(F_REG, (int)px + 62, 485, C_SUB, 14, "Abrir y escribir", -1);
    touch_zone(px + 22, 460, pw - 44, 40, SCE_CTRL_CROSS);
}

static void draw_home_chats(void) {
    vita2d_draw_rectangle(LIST_X, 56, LIST_W, 452, C_BAR);
    vita2d_draw_rectangle(LIST_W - 1, 56, 1, 452, C_LINE);
    int x = 14;
    x += keycap(x, 78, "L") + 8;
    char unread_label[32];
    snprintf(unread_label, sizeof(unread_label), "No leídos");
    const char *labels[4] = {"Todos", unread_label, "Grupos", "Archivo"};
    for (int i = 0; i < 4; i++) {
        pill(x, 78, labels[i], filter == i);
        x += pill_w(labels[i], filter == i) + 6;
    }
    keycap(LIST_W - 14 - 24, 78, "R");

    if (fcount == 0) {
        if (!view.nchats) spinner(LIST_W / 2, 230, frame);
        txt_center(F_REG, LIST_W / 2, 280, C_SUB, 14, view.nchats ? "Sin chats con este filtro" : "Sincronizando chats…");
    }
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(LIST_X, LIST_Y, LIST_X + LIST_W - 1, LIST_B);
    int first = (int)(list_sc.pos / ROW_H);
    if (first < 0) first = 0;
    for (int i = first; i < fcount; i++) {
        float y = LIST_Y + i * ROW_H - list_sc.pos + 2;
        if (y > LIST_B) break;
        draw_chat_row(&view.chats[fidx[i]], 8, y, LIST_W - 16, i == chat_sel);
    }
    vita2d_disable_clipping();
    // barra de scroll
    float total = fcount * ROW_H, vh = LIST_B - LIST_Y;
    if (total > vh) {
        float bh = vh * vh / total, by = LIST_Y + (vh - bh) * (list_sc.pos / (total - vh));
        rrect(LIST_W - 6, by, 3, bh, 1.5f, HEXA(0x1E2A36, 0x30));
    }
    draw_preview_pane();
}

static void select_chat_preview(void) {
    if (fcount <= 0) return;
    const Chat *c = &view.chats[fidx[chat_sel]];
    if (strcmp(sel_jid, c->jid)) {
        snprintf(sel_jid, sizeof(sel_jid), "%s", c->jid);
        sel_changed_at = now_ms();
    }
    // espera un instante antes de pedir el chat (por si solo vas pasando)
    if (strcmp(open_jid, sel_jid) && now_ms() - sel_changed_at > 250) {
        snprintf(open_jid, sizeof(open_jid), "%s", sel_jid);
        sync_open_chat(open_jid);
    }
}

static void input_chats(void) {
    int old_filter = filter, old_sel = chat_sel;
    if (btn(SCE_CTRL_LTRIGGER)) filter = (filter + 3) % 4;
    if (btn(SCE_CTRL_RTRIGGER)) filter = (filter + 1) % 4;
    // tocar los filtros
    {
        int x = 14 + 24 + 8;
        char unread_label[32];
        snprintf(unread_label, sizeof(unread_label), "No leídos");
        const char *labels[4] = {"Todos", unread_label, "Grupos", "Archivo"};
        for (int i = 0; i < 4; i++) {
            int w = pill_w(labels[i], filter == i);
            if (tap_in(x, 62, w, 32)) filter = i;
            x += w + 6;
        }
    }
    if (filter != old_filter) {
        audio_sfx(SFX_MOVE);
        chat_sel = 0;
        list_sc.pos = 0;
        list_sc.target = -1;
    }
    build_filter();
    if (rep(SCE_CTRL_DOWN) && chat_sel < fcount - 1) chat_sel++;
    if (rep(SCE_CTRL_UP) && chat_sel > 0) chat_sel--;
    if (rep(SCE_CTRL_RIGHT)) chat_sel = chat_sel + 6 < fcount ? chat_sel + 6 : fcount - 1;
    if (rep(SCE_CTRL_LEFT)) chat_sel = chat_sel > 6 ? chat_sel - 6 : 0;
    if (chat_sel >= fcount) chat_sel = fcount > 0 ? fcount - 1 : 0;
    if (chat_sel < 0) chat_sel = 0;

    // toque en una fila: la selecciona; si ya estaba, la abre
    int open_now = 0;
    if (T.tapped && T.tap_x < LIST_W && T.tap_y >= LIST_Y && T.tap_y < LIST_B) {
        int i = (int)((T.tap_y - LIST_Y + list_sc.pos) / ROW_H);
        if (i >= 0 && i < fcount) {
            if (i == chat_sel) open_now = 1;
            chat_sel = i;
        }
    }
    if (chat_sel != old_sel) {
        audio_sfx(SFX_MOVE);
        if (!T.tapped) scroller_show(&list_sc, chat_sel * ROW_H, ROW_H, LIST_B - LIST_Y);
    }
    scroller_update(&list_sc, LIST_X, LIST_Y, LIST_W, LIST_B - LIST_Y, fcount * ROW_H - (LIST_B - LIST_Y) + 4, 0);

    if (fcount > 0) {
        const Chat *c = &view.chats[fidx[chat_sel]];
        select_chat_preview();
        if (btn(SCE_CTRL_CROSS) || open_now) {
            open_chat(c->jid);
            return;
        }
        if (btn(SCE_CTRL_SQUARE)) {
            open_contact(c->jid, SCR_HOME);
            return;
        }
    }
}

// ---------- estados ----------
typedef struct { int kind; int idx; } StRow;   // kind 0 = mi estado, 1 = etiqueta, 2 = contacto
static StRow strows[MAX_STATUS + 4];
static int nstrows = 0;
static int st_sel = 0;
static char st_sel_poster[64];
static unsigned int st_changed_at = 0;
static Scroller st_sc;
static float st_top[MAX_STATUS + 4];

static int my_status_idx(void) {
    for (int i = 0; i < view.nstatus; i++) if (!strcmp(view.status[i].poster, "me")) return i;
    return -1;
}

static void build_strows(void) {
    nstrows = 0;
    strows[nstrows++] = (StRow){0, my_status_idx()};
    for (int pass = 0; pass < 2; pass++) {
        int label_added = 0;
        for (int i = 0; i < view.nstatus; i++) {
            const StatusRow *s = &view.status[i];
            if (!strcmp(s->poster, "me")) continue;
            if ((pass == 0) != (s->unseen > 0)) continue;
            if (!label_added) { strows[nstrows++] = (StRow){1, pass}; label_added = 1; }
            strows[nstrows++] = (StRow){2, i};
        }
    }
    float y = 0;
    for (int r = 0; r < nstrows; r++) {
        st_top[r] = y;
        y += strows[r].kind == 1 ? 32 : ROW_H;
    }
    st_top[nstrows] = y;
}

static int strow_selectable(int r) { return r >= 0 && r < nstrows && strows[r].kind != 1; }

static void draw_status_ring(float cx, float cy, const StatusRow *s) {
    float r = 25;
    if (s->unseen > 0) {
        int segs = s->count < 1 ? 1 : s->count;
        float gap = segs > 1 ? 0.24f : 0;
        float span = 6.28318f / segs;
        for (int k = 0; k < segs; k++) {
            float a0 = -1.5708f + k * span + gap / 2, a1 = a0 + span - gap;
            int seen_seg = k < s->count - s->unseen;
            arc_dots(cx, cy, r, a0, a1, 3, seen_seg ? C_LINE : C_ACC);
        }
    } else {
        arc_dots(cx, cy, r, 0, 6.2832f, 2, C_LINE);
    }
}

static void draw_home_status(void) {
    vita2d_draw_rectangle(LIST_X, 56, LIST_W, 452, C_BAR);
    vita2d_draw_rectangle(LIST_W - 1, 56, 1, 452, C_LINE);
    float top = 62;
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(LIST_X, (int)top, LIST_W - 1, LIST_B);
    for (int r = 0; r < nstrows; r++) {
        float y = top + st_top[r] - st_sc.pos;
        if (y > LIST_B) break;
        if (y + ROW_H < top) continue;
        StRow *row = &strows[r];
        int sel = r == st_sel;
        if (row->kind == 1) {
            txt(F_BOLD, 22, (int)y + 24, C_SUB, 12, row->idx ? "VISTOS" : "RECIENTES", -1);
            continue;
        }
        if (sel) focus_card(8, y, LIST_W - 16, ROW_H - 4, 14);
        if (row->kind == 0) {
            avatar(42, y + 31, 23, view.me_name[0] ? view.me_name : "Tú", hash_color(view.me_jid), view.me_jid);
            circle(60, y + 49, 10, sel ? C_ACC_SOFT : C_BAR);
            circle(60, y + 49, 8, C_ACC);
            icon_plus(60, y + 49, 4, C_WHITE);
            txt(F_BOLD, 80, (int)y + 26, C_TEXT, 16, "Mi estado", -1);
            char sub[64];
            if (row->idx >= 0) snprintf(sub, sizeof(sub), "%d publicado%s · %s", view.status[row->idx].count, view.status[row->idx].count == 1 ? "" : "s", view.status[row->idx].ago);
            else snprintf(sub, sizeof(sub), "Publica un texto desde la Vita");
            txt(F_REG, 80, (int)y + 47, C_SUB, 13, sub, -1);
        } else {
            const StatusRow *s = &view.status[row->idx];
            draw_status_ring(42, y + 31, s);
            avatar(42, y + 31, 20, s->name, s->color, s->poster);
            txt_fit(F_BOLD, 80, (int)y + 26, C_TEXT, 16, s->name, LIST_W - 100);
            char sub[64];
            if (s->unseen > 0) snprintf(sub, sizeof(sub), "%d nuevo%s · %s", s->unseen, s->unseen == 1 ? "" : "s", s->ago);
            else snprintf(sub, sizeof(sub), "%s", s->ago);
            txt(F_REG, 80, (int)y + 47, C_SUB, 13, sub, -1);
        }
    }
    vita2d_disable_clipping();

    // vista previa del seleccionado
    float px = LIST_W, pw = SCREEN_W - px;
    StRow *row = strow_selectable(st_sel) ? &strows[st_sel] : NULL;
    if (!row) return;
    const char *poster = row->kind == 0 ? "me" : view.status[row->idx].poster;
    int has = row->kind == 2 || row->idx >= 0;
    if (!has) {
        icon_plus(px + pw / 2, 220, 16, C_ACC);
        txt_center(F_XB, (int)(px + pw / 2), 270, C_TEXT, 22, "Comparte algo");
        txt_center(F_REG, (int)(px + pw / 2), 300, C_SUB, 14, "Pulsa × para escribir un estado de texto.");
        return;
    }
    const StatusItem *it = NULL;
    if (!strcmp(view.sitems_poster, poster) && view.nsitems > 0) {
        it = &view.sitems[0];
        for (int i = 0; i < view.nsitems; i++) if (!view.sitems[i].seen) { it = &view.sitems[i]; break; }
    }
    float cw = 230, ch = 400, cx = px + 40, cy = 56 + (452 - ch) / 2;
    shadow(cx, cy, cw, ch, 20);
    draw_status_card(cx, cy, cw, ch, it, poster);
    if (!it) spinner(cx + cw / 2, cy + ch / 2, frame);
    float ix = cx + cw + 32;
    const char *name = row->kind == 0 ? "Mi estado" : view.status[row->idx].name;
    txt_fit(F_XB, (int)ix, 220, C_TEXT, 20, name, (int)(SCREEN_W - ix - 20));
    char info[96];
    int count = view.status[row->idx].count;
    int unseen = row->kind == 0 ? 0 : view.status[row->idx].unseen;
    if (unseen) snprintf(info, sizeof(info), "%d estado%s nuevo%s", unseen, unseen == 1 ? "" : "s", unseen == 1 ? "" : "s");
    else snprintf(info, sizeof(info), "%d estado%s", count, count == 1 ? "" : "s");
    txt(F_REG, (int)ix, 248, C_SUB, 14, info, -1);
    txt(F_REG, (int)ix, 274, C_FAINT, 12, "Los videos se ven como imagen fija.", -1);
    touch_zone(cx, cy, cw, ch, row->kind == 0 ? SCE_CTRL_SQUARE : SCE_CTRL_CROSS);
}

static void input_status_tab(void) {
    build_strows();
    if (!strow_selectable(st_sel)) st_sel = 0;
    int old = st_sel;
    if (rep(SCE_CTRL_DOWN)) {
        int r = st_sel + 1;
        while (r < nstrows && !strow_selectable(r)) r++;
        if (r < nstrows) st_sel = r;
    }
    if (rep(SCE_CTRL_UP)) {
        int r = st_sel - 1;
        while (r >= 0 && !strow_selectable(r)) r--;
        if (r >= 0) st_sel = r;
    }
    int open_now = 0;
    if (T.tapped && T.tap_x < LIST_W && T.tap_y >= 62 && T.tap_y < LIST_B) {
        float yy = T.tap_y - 62 + st_sc.pos;
        for (int r = 0; r < nstrows; r++)
            if (yy >= st_top[r] && yy < st_top[r + 1] && strow_selectable(r)) {
                if (r == st_sel) open_now = 1;
                st_sel = r;
            }
    }
    if (st_sel != old) {
        audio_sfx(SFX_MOVE);
        float t = st_top[st_sel];
        if (st_sel > 0 && strows[st_sel - 1].kind == 1) t = st_top[st_sel - 1];
        if (!T.tapped) scroller_show(&st_sc, t, st_top[st_sel] + ROW_H - t, LIST_B - 62);
    }
    scroller_update(&st_sc, LIST_X, 62, LIST_W, LIST_B - 62, st_top[nstrows] - (LIST_B - 62), 0);

    StRow *row = &strows[st_sel];
    const char *poster = row->kind == 0 ? "me" : view.status[row->idx].poster;
    if (strcmp(st_sel_poster, poster)) {
        snprintf(st_sel_poster, sizeof(st_sel_poster), "%s", poster);
        st_changed_at = now_ms();
    }
    if ((row->kind == 2 || row->idx >= 0) && strcmp(view.sitems_poster, poster) && now_ms() - st_changed_at > 250)
        sync_open_status(poster);

    if (btn(SCE_CTRL_CROSS) || open_now) {
        if (row->kind == 0) {
            if (open_now && row->idx >= 0) open_status_viewer("me");
            else {
                audio_sfx(SFX_SELECT);
                ime_ask(IMEP_STATUS_POST, "Tu estado (se publica para tus contactos)", "", IME_KIND_TEXT, 500);
            }
        } else {
            open_status_viewer(poster);
        }
    }
    if (btn(SCE_CTRL_SQUARE) && row->kind == 0 && row->idx >= 0) open_status_viewer("me");
    if (btn(SCE_CTRL_START) && row->kind == 0) camera_enter_status();      // estado con foto (cámara)
    if (btn(SCE_CTRL_LTRIGGER) && row->kind == 0) gallery_enter_status();  // estado con foto (galería)
}

// ---------- pantalla ----------
void draw_home(void) {
    if (home_tab == 0) build_filter();
    else build_strows();
    draw_header();
    if (home_tab == 0) draw_home_chats();
    else draw_home_status();

    footer_bar();
    int x = 18;
    if (home_tab == 0) {
        x = hint(x, 526, GLYPH_CROSS, "Abrir");
        x = hint(x, 526, GLYPH_SQUARE, "Info");
        x = hint_key(x, 526, "START", SCE_CTRL_START, "Nuevo chat");
        x = hint(x, 526, GLYPH_TRIANGLE, "Ajustes");
        x = hint_key(x, 526, "SELECT", SCE_CTRL_SELECT, "Estados");
        txt_right(F_REG, SCREEN_W - 18, 531, C_FAINT, 13, "L / R  Filtros");
    } else {
        int mine = strow_selectable(st_sel) && strows[st_sel].kind == 0;
        x = hint(x, 526, GLYPH_CROSS, mine ? "Publicar texto" : "Ver");
        if (mine && strows[st_sel].idx >= 0) x = hint(x, 526, GLYPH_SQUARE, "Ver mis estados");
        if (mine) { x = hint_key(x, 526, "START", SCE_CTRL_START, "Foto"); x = hint_key(x, 526, "L", SCE_CTRL_LTRIGGER, "Galería"); }
        x = hint(x, 526, GLYPH_TRIANGLE, "Ajustes");
        x = hint_key(x, 526, "SELECT", SCE_CTRL_SELECT, "Chats");
    }
}

void input_home(void) {
    int tab = home_tab;
    if (btn(SCE_CTRL_SELECT)) tab ^= 1;
    for (int i = 0; i < 2; i++)
        if (tap_in(seg_x[i], 10, seg_w[i], 36)) tab = i;
    if (tab != home_tab) {
        home_tab = tab;
        audio_sfx(SFX_MOVE);
        return;
    }
    if (btn(SCE_CTRL_TRIANGLE)) { go(SCR_SETTINGS); return; }
    if (btn(SCE_CTRL_START)) { newchat_enter(); return; }
    if (home_tab == 0) input_chats();
    else input_status_tab();
}

void home_after_input(void) {
    // si la lista cambio, que la seleccion siga en el mismo chat
    if (home_tab != 0 || !sel_jid[0]) return;
    build_filter();
    if (chat_sel < fcount && !strcmp(view.chats[fidx[chat_sel]].jid, sel_jid)) return;
    for (int i = 0; i < fcount; i++)
        if (!strcmp(view.chats[fidx[i]].jid, sel_jid)) { chat_sel = i; return; }
}
