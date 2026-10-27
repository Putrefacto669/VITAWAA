// Chat abierto: mensajes, notas de voz, fotos, stickers, menu de acciones y reenviar.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <psp2/ctrl.h>
#include <vita2d.h>

#include "app.h"
#include "audio.h"
#include "ime.h"
#include "net.h"
#include "tex.h"
#include "touch.h"
#include "video.h"

char open_jid[64];
char reply_id[48], reply_name[64], reply_text[160];

static int msg_sel = -1;
static int stick_bottom = 1;
static Scroller conv_sc;
static int menu_open = 0, menu_sel = 0, react_sel = 0;
static int stk_open = 0, stk_tab = 0, stk_sel = 0;
static int attach_open = 0, attach_sel = 0;
static int viewer_open = 0;
static int emo_open = 0, emo_tab = 0, emo_sel = 0;     // panel de emojis (tab 0 = recientes)
static char emo_react_id[48];                           // si no está vacío: reaccionar a ese mensaje
static int vid_open = 0, vid_started = 0, vid_gif = 0;
static char vid_id[48];
static char edit_id[48];
static char find_q[128];
static char viewer_id[48];
static char clipboard[1024];
static unsigned int last_read_job = 0;

// grabacion: 0 nada, 1 con R sostenido, 2 con el dedo en el microfono, 3 modo "toca para enviar"
static int rec_mode = 0;

#define NREACT 7   // 6 rápidas + "más"
static const char *REACT_SEND[6] = {"1f44d", "2764-fe0f", "1f602", "1f62e", "1f622", "1f64f"};
static const char *REACT_SHOW[6] = {"\x1E" "1f44d\x1F", "\x1E" "2764\x1F", "\x1E" "1f602\x1F", "\x1E" "1f62e\x1F", "\x1E" "1f622\x1F", "\x1E" "1f64f\x1F"};

// ---------- utilidades ----------
static int is_media(const Msg *m) { return !strcmp(m->kind, "image") || !strcmp(m->kind, "video") || !strcmp(m->kind, "gif"); }
static int is_audio(const Msg *m) { return !strcmp(m->kind, "voice") || !strcmp(m->kind, "audio"); }
static int is_typing(const char *sub) { return strstr(sub, "escrib") || strstr(sub, "grabando"); }

static void strip_emoji(const char *in, char *out, int cap) {
    int n = 0;
    for (const char *p = in; *p && n < cap - 1; p++) {
        if (*p == '\x1E') {
            while (*p && *p != '\x1F') p++;
            if (!*p) break;
            continue;
        }
        out[n++] = *p;
    }
    out[n] = '\0';
}

static void fmt_secs(char *out, int cap, float s) {
    int t = (int)(s + 0.5f);
    snprintf(out, cap, "%d:%02d", t / 60, t % 60);
}

void open_chat(const char *jid) {
    if (strcmp(open_jid, jid)) {
        snprintf(open_jid, sizeof(open_jid), "%s", jid);
        sync_open_chat(open_jid);
        find_q[0] = '\0';
    }
    msg_sel = view.conv.n - 1;
    conv_sc.pos = 0;
    conv_sc.vel = 0;
    conv_sc.target = -1;
    stick_bottom = 1;
    menu_open = stk_open = attach_open = viewer_open = emo_open = 0;
    if (vid_open) { video_close(); vid_open = 0; }
    reply_id[0] = '\0';
    const Chat *c = chat_by_jid(jid);
    if (c && c->unread > 0) { job_read(jid); last_read_job = now_ms(); }
    go(SCR_CHAT);
}

int chat_overlay_open(void) { return menu_open || stk_open || attach_open || viewer_open || rec_mode || emo_open || vid_open; }

void chat_on_conv_update(void) {
    if (strcmp(view.conv.jid, open_jid)) return;
    if (stick_bottom || msg_sel < 0) {
        msg_sel = view.conv.n - 1;
        conv_sc.target = 0;
    }
    if (msg_sel >= view.conv.n) msg_sel = view.conv.n - 1;
}

// ---------- maquetado ----------
#define PADX 13
#define PADY 9
#define LINE_H (g_body + 5)
#define NAME_H 18
#define QUOTE_H 46
#define FWD_H 18
#define MEDIA_W 280
#define MEDIA_H 180
#define STICKER 120
#define BODY g_body
#define VOICE_W 320

static const char *display_text(const Msg *m, char *buf, int cap) {
    const char *k = m->kind;
    if (!strcmp(k, "location")) snprintf(buf, cap, "Ubicación%s%s", m->text[0] ? " · " : "", m->text);
    else if (!strcmp(k, "contact")) snprintf(buf, cap, "Contacto · %s", m->text);
    else if (!strcmp(k, "poll")) snprintf(buf, cap, "%s\n(para votar usa el teléfono)", m->text);
    else if (!strcmp(k, "other")) snprintf(buf, cap, "Mensaje no compatible: míralo en el teléfono");
    else snprintf(buf, cap, "%s", m->text);
    return buf;
}

static void layout_msg(Msg *m, int maxw, int group) {
    if (m->lay_w == maxw && m->lay_fs == g_body) return;
    m->lay_w = maxw;
    m->lay_fs = g_body;
    char buf[1100];
    int time_w = txt_w(F_REG, 11, m->time, -1) + (m->mine ? 20 : 0);
    int head = 0, w = 0;

    if (!strcmp(m->kind, "sticker")) {
        m->nlines = 0;
        m->w = STICKER;
        m->h = STICKER + 16;
        return;
    }
    if (group && !m->mine) {
        head += NAME_H;
        w = txt_w(F_BOLD, 13, m->sender, -1);
    }
    if (is_audio(m)) {
        m->nlines = 0;
        m->w = VOICE_W;
        m->h = 62 + head;
        return;
    }
    if (m->fwd) {
        head += FWD_H;
        if (w < 110) w = 110;
    }
    if (m->qname[0] || m->qtext[0]) {
        head += QUOTE_H + 6;
        if (w < 220) w = 220;
    }
    if (!strcmp(m->kind, "doc")) {
        m->nlines = 0;
        m->w = 300;
        m->h = PADY * 2 + head + 44 + 16;
        return;
    }
    if (!m->disp) {
        m->disp = 1;
        if (strcmp(m->kind, "deleted")) {
            display_text(m, buf, sizeof(buf));
            snprintf(m->text, sizeof(m->text), "%.1000s", buf);
        }
    }

    int media = is_media(m);
    int inner = media ? MEDIA_W - 2 * 4 : maxw - 2 * PADX;
    if (media) {
        head += MEDIA_H + 4;
        w = MEDIA_W - 2 * PADX + 8;
    }
    if (m->text[0]) {
        m->nlines = wrap(F_REG, BODY, m->text, inner, m->off, m->len, MAX_LINES);
        for (int i = 0; i < m->nlines; i++) {
            int lw = txt_w(F_REG, BODY, m->text + m->off[i], m->len[i]);
            if (lw > w) w = lw;
        }
        int last = txt_w(F_REG, BODY, m->text + m->off[m->nlines - 1], m->len[m->nlines - 1]);
        m->time_inline = last + 10 + time_w <= inner;
        if (m->time_inline && last + 10 + time_w > w) w = last + 10 + time_w;
    } else {
        m->nlines = 0;
        m->time_inline = 0;
        if (w < time_w) w = time_w;
    }
    m->w = w + 2 * PADX;
    if (media && m->w < MEDIA_W + 8) m->w = MEDIA_W + 8;
    m->h = PADY * 2 + head + m->nlines * LINE_H + (m->time_inline ? 0 : 16);
}

static int slot_height(const Msg *m) { return m->h + (m->reacts[0] ? 14 : 0) + 8; }
static int needs_day(const Conv *c, int i) { return i == 0 || strcmp(c->msgs[i].day, c->msgs[i - 1].day); }

// ---------- dibujo de mensajes ----------
static void draw_reactions(const Msg *m, float bx, float by) {
    if (!m->reacts[0]) return;
    int tw = txt_w(F_REG, 13, m->reacts, -1);
    float pw = tw + 14, ph = 24;
    float px = m->mine ? bx + m->w - pw - 10 : bx + 10;
    float py = by + m->h - 8;
    card(px, py, pw, ph, 12, C_CARD, C_LINE);
    txt(F_REG, (int)px + 7, (int)py + 17, C_TEXT, 13, m->reacts, -1);
}

static void draw_time(const Msg *m, float right, float base, unsigned int bubble) {
    unsigned int tc = m->mine ? C_MINE_SUB : C_FAINT;
    float x = right;
    if (m->mine) {
        x -= 16;
        ticks(x, base - 4, m->ack, bubble);
        x -= 4;
    }
    if (m->starred) {
        x -= 12;
        txt(F_REG, (int)x, (int)base, tc, 11, "★", -1);
        x -= 2;
    }
    int w = txt_w(F_REG, 11, m->time, -1);
    txt(F_REG, (int)(x - w), (int)base, tc, 11, m->time, -1);
}

static void draw_voice(const Conv *c, Msg *m, float bx, float cx, float cy, unsigned int bubble) {
    (void)c;
    char cur[48];
    float prog = 0, total = 0;
    int playing = 0;
    audio_state(cur, sizeof(cur), &prog, &total, &playing);
    int mine_loaded = !strcmp(cur, m->id);
    if (!mine_loaded) prog = 0;
    int loading = audio_is_loading(m->id);

    float pcx = cx + 20, pcy = cy + 22;
    circle(pcx, pcy, 20, m->mine ? C_ACC : C_ACC);
    if (loading) {
        arc_dots(pcx, pcy, 11, frame * 0.15f, frame * 0.15f + 4.2f, 2.4f, C_WHITE);
    } else if (mine_loaded && playing) {
        icon_pause(pcx, pcy, 8, C_WHITE);
    } else {
        icon_play(pcx + 1, pcy, 8, C_WHITE);
    }

    // onda (fija por id) con el progreso pintado
    unsigned int seed = (unsigned int)hash_color(m->id) * 2654435761u + 7u;
    for (int k = 0; k < (int)strlen(m->id); k++) seed = seed * 31u + (unsigned char)m->id[k];
    int bars = 30;
    float wx = cx + 50;
    for (int i = 0; i < bars; i++) {
        seed = seed * 1103515245u + 12345u;
        float bh = 5 + (seed >> 16) % 18;
        int done = (i + 0.5f) / bars <= prog;
        unsigned int col = done ? C_ACC : (m->mine ? C_MINE_SUB : HEX(0xB7C2CD));
        rrect(wx + i * 7.4f, pcy - bh / 2, 3.4f, bh, 1.7f, col);
    }
    char tbuf[24];
    if (mine_loaded && (playing || prog > 0)) {
        fmt_secs(tbuf, sizeof(tbuf), prog * total);
    } else {
        snprintf(tbuf, sizeof(tbuf), "%s", m->dur[0] ? m->dur : "0:00");
    }
    txt(F_REG, (int)wx, (int)(pcy + 28), m->mine ? C_MINE_SUB : C_FAINT, 11, tbuf, -1);
}

static void draw_msg(const Conv *c, Msg *m, float ax, float aw, float y, int selected) {
    int group = c->group;
    unsigned int bubble = m->mine ? C_MINE : C_BUBBLE;
    float bx = m->mine ? ax + aw - m->w : ax;

    if (!strcmp(m->kind, "sticker")) {
        if (selected) { rrect(bx - 5, y - 5, STICKER + 10, STICKER + 10, 14, C_ACC); rrect(bx - 3, y - 3, STICKER + 6, STICKER + 6, 12, C_ACC_SOFT); }
        char path[256];
        sticker_path(path, sizeof(path), c->jid, m->id, 160);
        vita2d_texture *t = tex_get(path);
        if (t) draw_sticker(t, bx, y, STICKER, STICKER);
        else rrect(bx + 20, y + 20, STICKER - 40, STICKER - 40, 16, C_PHOTO_BG);
        float tw = txt_w(F_REG, 11, m->time, -1) + (m->mine ? 20 : 0);
        draw_time(m, m->mine ? bx + STICKER : bx + tw, y + STICKER + 13, C_BG);
        draw_reactions(m, bx, y);
        return;
    }

    if (selected) rrect(bx - 3, y - 3, m->w + 6, m->h + 6, 17, C_ACC);
    // burbuja Aero: sombrita, cuerpo y reflejo de vidrio arriba
    rrect(bx + 1, y + 2, m->w, m->h, 14, HEXA(0x000000, ui_dark ? 0x40 : 0x12));
    if (m->mine) {
        rrect(bx, y, m->w, m->h, 14, bubble);
        gloss(bx, y, m->w, m->h, 14);
    } else card(bx, y, m->w, m->h, 14, bubble, C_BUBBLE_LN);

    float cx = bx + PADX, cy = y + PADY;
    float inner_w = m->w - 2 * PADX;

    if (group && !m->mine) {
        txt_fit(F_BOLD, (int)cx, (int)cy + 12, name_color(m->color), 13, m->sender, (int)inner_w);
        cy += NAME_H;
    }
    if (is_audio(m)) {
        draw_voice(c, m, bx, cx - 4, cy - 2, bubble);
        draw_time(m, bx + m->w - PADX, y + m->h - PADY, bubble);
        draw_reactions(m, bx, y);
        return;
    }
    if (m->fwd) {
        txt(F_REG, (int)cx, (int)cy + 12, m->mine ? C_MINE_SUB : C_FAINT, 12, "→ Reenviado", -1);
        cy += FWD_H;
    }
    if (m->qname[0] || m->qtext[0]) {
        float qw = m->w - 2 * PADX + 6;
        rrect(cx - 3, cy, qw, QUOTE_H, 10, m->mine ? C_MINE_Q : C_SOFT);
        vita2d_draw_rectangle(cx - 3, cy + 6, 3, QUOTE_H - 12, C_ACC);
        txt_fit(F_BOLD, (int)cx + 8, (int)cy + 18, C_ACC, 13, m->qname, (int)qw - 16);
        txt_fit(F_REG, (int)cx + 8, (int)cy + 37, C_SUB, 13, m->qtext, (int)qw - 16);
        cy += QUOTE_H + 6;
    }
    if (!strcmp(m->kind, "doc")) {
        rrect(cx, cy, 30, 38, 4, C_SUB);
        vita2d_draw_rectangle(cx + 20, cy, 10, 10, bubble);
        txt_fit(F_BOLD, (int)cx + 42, (int)cy + 17, C_TEXT, 15, m->text, (int)inner_w - 42);
        txt(F_REG, (int)cx + 42, (int)cy + 36, m->mine ? C_MINE_SUB : C_SUB, 12, "Documento · ábrelo en el teléfono", -1);
        draw_time(m, bx + m->w - PADX, y + m->h - PADY, bubble);
        draw_reactions(m, bx, y);
        return;
    }
    if (is_media(m)) {
        float mx = bx + 4;
        float my = cy == y + PADY ? y + 4 : cy;
        char path[256];
        media_path(path, sizeof(path), c->jid, m->id, 400);
        vita2d_texture *t = tex_get(path);
        if (t) draw_media_cover(t, mx, my, m->w - 8, MEDIA_H);
        else photo_placeholder(mx, my, m->w - 8, MEDIA_H, tex_failed(path) ? "No se pudo cargar" : "Cargando…");
        if (strcmp(m->kind, "image")) {
            float pcx = mx + (m->w - 8) / 2, pcy = my + MEDIA_H / 2;
            circle(pcx, pcy, 22, HEXA(0x000000, 0x90));
            icon_play(pcx + 2, pcy, 9, C_WHITE);
            if (m->dur[0]) {
                int dw = txt_w(F_BOLD, 12, m->dur, -1);
                rrect(mx + 8, my + MEDIA_H - 26, dw + 12, 20, 10, HEXA(0x000000, 0x90));
                txt(F_BOLD, (int)mx + 14, (int)(my + MEDIA_H - 11), C_WHITE, 12, m->dur, -1);
            }
        }
        cy = my + MEDIA_H + 4 + 2;
    }
    unsigned int tc = !strcmp(m->kind, "deleted") ? C_FAINT : C_TEXT;
    for (int i = 0; i < m->nlines; i++)
        txt(F_REG, (int)cx, (int)(cy + 16 + i * LINE_H), tc, BODY, m->text + m->off[i], m->len[i]);
    draw_time(m, bx + m->w - PADX, y + m->h - PADY - (m->time_inline ? 2 : 0), bubble);
    draw_reactions(m, bx, y);
}

static void day_pill(float cx, float y, const char *label) {
    int w = txt_w(F_BOLD, 12, label, -1) + 24;
    rrect(cx - w / 2.0f, y, w, 22, 11, C_DAY);
    txt_center(F_BOLD, (int)cx, (int)y + 15, C_SUB2, 12, label);
}

// Posiciones medidas desde abajo: off_b[i] = distancia del borde de abajo del area al de abajo del mensaje
static float off_b[MAX_MSGS], hgt[MAX_MSGS], content_h = 0;
static float sel_top = -1;
static int vis_first = -1, vis_last = -1;
static int tapped_msg = -1;

static void measure_conv(Conv *c, int maxw) {
    float acc = 6;
    for (int i = 0; i < c->n; i++) layout_msg(&c->msgs[i], maxw, c->group);
    for (int i = c->n - 1; i >= 0; i--) {
        hgt[i] = slot_height(&c->msgs[i]);
        off_b[i] = acc;
        acc += hgt[i];
        if (needs_day(c, i)) acc += 34;
    }
    content_h = acc + 6;
}

static void draw_conv(Conv *c, float ax, float ay, float aw, float ah, float pos, int sel, int with_touch) {
    sel_top = -1;
    vis_first = vis_last = -1;
    if (c->n == 0) {
        if (!c->jid[0]) return;
        spinner(ax + aw / 2, ay + ah / 2 - 20, frame);
        txt_center(F_REG, (int)(ax + aw / 2), (int)(ay + ah / 2 + 30), C_SUB, 14, "Cargando mensajes…");
        return;
    }
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle((int)ax - 8, (int)ay, (int)(ax + aw + 8), (int)(ay + ah));
    float bottom = ay + ah;
    for (int i = 0; i < c->n; i++) {
        float t = bottom - off_b[i] - hgt[i] + pos;   // arriba del espacio del mensaje
        if (t > ay + ah || t + hgt[i] + 40 < ay) continue;
        if (needs_day(c, i)) day_pill(ax + aw / 2, t - 30, c->msgs[i].day);
        draw_msg(c, &c->msgs[i], ax, aw, t, i == sel);
        if (i == sel) sel_top = t;
        if (t >= ay - 4 && t + hgt[i] <= ay + ah + 4) {
            if (vis_first < 0) vis_first = i;
            vis_last = i;
        }
        if (with_touch && T.tapped && T.tap_y >= t && T.tap_y < t + hgt[i] && T.tap_y >= ay && T.tap_y < ay + ah) {
            Msg *m = &c->msgs[i];
            float bx = m->mine ? ax + aw - m->w : ax;
            if (T.tap_x >= bx - 10 && T.tap_x <= bx + m->w + 10) tapped_msg = i;   // lo usa input_chat
        }
    }
    vita2d_disable_clipping();
}

// vista previa en Inicio
void chat_draw_preview(float ax, float ay, float aw, float ah) {
    Conv *c = &view.conv;
    measure_conv(c, (int)(aw * 0.72f));
    draw_conv(c, ax, ay, aw, ah, 0, -1, 0);
}

// ---------- geometria del chat ----------
#define AX 110
#define AW (SCREEN_W - 220)

static float composer_y(void) {
    float bottom = SCREEN_H - 36;
    float panel_h = (stk_open || emo_open) ? 196 : 0;
    return bottom - panel_h - 58;
}
static float area_bottom(void) { return composer_y() - (reply_id[0] ? 58 : 0) - 4; }
#define AREA_TOP 56

// ---------- menu de acciones ----------
enum { MA_REPLY, MA_FWD, MA_COPY, MA_STAR, MA_INFO, MA_EDIT, MA_REVOKE, MA_DELETE };
static const char *MENU_ITEMS[8] = {"Responder", "Reenviar", "Copiar texto", "Destacar", "Info del contacto", "Editar", "Eliminar para todos", "Eliminar para mí"};
static int menu_items[8], MENU_N = 0;
static int can_edit(const Msg *m) { return m->mine && !strcmp(m->kind, "text"); }
static void build_menu(const Msg *m) {
    MENU_N = 0;
    for (int a = 0; a < 8; a++) {
        if (a == MA_EDIT && !can_edit(m)) continue;
        if (a == MA_REVOKE && (!m->mine || !strcmp(m->kind, "deleted"))) continue;
        menu_items[MENU_N++] = a;
    }
}

static void menu_icon(int i, float x, float cy, unsigned int c) {
    switch (i) {
    case 0:
        thick_line(x + 5, cy - 5, x, cy, 2, c);
        thick_line(x, cy, x + 5, cy + 5, 2, c);
        thick_line(x, cy, x + 12, cy, 2, c);
        thick_line(x + 12, cy, x + 16, cy + 6, 2, c);
        break;
    case 1:
        thick_line(x + 11, cy - 5, x + 16, cy, 2, c);
        thick_line(x + 16, cy, x + 11, cy + 5, 2, c);
        thick_line(x + 16, cy, x + 4, cy, 2, c);
        thick_line(x + 4, cy, x, cy + 6, 2, c);
        break;
    case 2:
        rrect(x + 5, cy - 4, 11, 11, 2, c);
        rrect(x + 6.5f, cy - 2.5f, 8, 8, 1, C_CARD);
        thick_line(x + 1, cy + 3, x + 1, cy - 8, 2, c);
        thick_line(x + 1, cy - 8, x + 10, cy - 8, 2, c);
        break;
    case 3: {
        float pts[10][2];
        for (int k = 0; k < 10; k++) {
            float a = -1.5708f + k * 0.6283f, r = (k % 2) ? 3.5f : 8.5f;
            pts[k][0] = x + 8 + r * cosf(a);
            pts[k][1] = cy + r * sinf(a);
        }
        for (int k = 0; k < 10; k++) thick_line(pts[k][0], pts[k][1], pts[(k + 1) % 10][0], pts[(k + 1) % 10][1], 1.8f, c);
        break;
    }
    case 4:
        ring(x + 8, cy, 8.5f, 1.8f, c, C_CARD);
        rrect(x + 7, cy - 1, 2, 6, 1, c);
        circle(x + 8, cy - 4, 1.3f, c);
        break;
    case 5:   // lápiz
        thick_line(x + 2, cy + 7, x + 13, cy - 4, 4, c);
        fill_tri(x - 1, cy + 10, x + 1, cy + 4, x + 5, cy + 8, c);
        thick_line(x + 3, cy + 11, x + 16, cy + 11, 1.5f, c);
        break;
    default:
        thick_line(x, cy - 5, x + 16, cy - 5, 2, c);
        thick_line(x + 2, cy - 5, x + 3, cy + 8, 2, c);
        thick_line(x + 14, cy - 5, x + 13, cy + 8, 2, c);
        thick_line(x + 3, cy + 8, x + 13, cy + 8, 2, c);
        thick_line(x + 5, cy - 8, x + 11, cy - 8, 2, c);
        break;
    }
}

#define MENU_W 290
#define MENU_X (SCREEN_W - 40 - MENU_W)
#define MENU_Y 70

static void draw_menu(const Msg *m) {
    float mw = MENU_W, mx = MENU_X, my = MENU_Y;
    float mh = 62 + MENU_N * 44 + 12;
    shadow(mx, my, mw, mh, 18);
    card(mx, my, mw, mh, 18, C_CARD, C_LINE);
    gloss(mx, my, mw, 62, 18);
    for (int i = 0; i < NREACT; i++) {
        float cx = mx + 25 + i * 40, cy = my + 31;
        if (menu_sel == -1 && react_sel == i) ring(cx, cy, 19, 2, C_ACC, C_ACC_SOFT);
        if (i < 6) txt(F_REG, (int)cx - 12, (int)cy + 9, C_TEXT, 21, REACT_SHOW[i], -1);
        else { circle(cx, cy, 14, C_SOFT); icon_plus(cx, cy, 7, C_SUB2); }
    }
    vita2d_draw_rectangle(mx, my + 62, mw, 1, C_LINE);
    for (int i = 0; i < MENU_N; i++) {
        int a = menu_items[i];
        float iy = my + 68 + i * 44;
        int sel = menu_sel == i;
        int danger = a == MA_DELETE || a == MA_REVOKE;
        if (sel) rrect(mx + 6, iy, mw - 12, 44, 10, C_ACC_SOFT);
        unsigned int c = danger ? C_RED_DARK : (sel ? C_ACC : C_SUB);
        menu_icon(a == MA_EDIT ? 5 : danger ? 7 : a, mx + 18, iy + 22, c);
        const char *label = MENU_ITEMS[a];
        if (a == MA_STAR && m->starred) label = "Quitar destacado";
        if (a == MA_INFO && view.conv.group && m->mine) label = "Info del grupo";
        txt(sel ? F_BOLD : F_REG, (int)mx + 48, (int)iy + 28, danger ? C_RED_DARK : C_TEXT, 16, label, -1);
    }
}

// ---------- panel de stickers ----------
static void draw_sticker_panel(void) {
    float py = SCREEN_H - 36 - 196;
    vita2d_draw_rectangle(0, py, SCREEN_W, 196, C_BAR);
    vita2d_draw_rectangle(0, py, SCREEN_W, 1, C_LINE);
    const char *tabs[3] = {"Recientes", "Favoritos", "Recibidos"};
    int x = 20;
    for (int i = 0; i < 3; i++) {
        pill(x, (int)py + 22, tabs[i], stk_tab == i);
        x += pill_w(tabs[i], stk_tab == i) + 8;
    }
    txt_right(F_REG, SCREEN_W - 20, (int)py + 27, C_FAINT, 13, "L / R  Cambiar pestaña");
    int ready = view.stk_tab == stk_tab && v_stk >= 0;
    if (!ready) { spinner(480, py + 110, frame); return; }
    if (view.nstk == 0) {
        txt_center(F_REG, 480, (int)py + 110, C_SUB, 15,
                   stk_tab == 1 ? "Marca stickers con □ para tenerlos aquí" : "Aún no hay stickers en esta pestaña");
        return;
    }
    float cell = (SCREEN_W - 40 - 7 * 10) / 8.0f;
    int start = (stk_sel / 8) * 8;
    for (int i = start; i < view.nstk && i < start + 8; i++) {
        int k = i - start;
        float cx = 20 + k * (cell + 10), cy = py + 50;
        if (i == stk_sel) focus_card(cx - 2, cy - 2, cell + 4, 100, 16);
        if (tap_in(cx, cy, cell, 96)) {
            if (i == stk_sel) press_next(SCE_CTRL_CROSS);
            stk_sel = i;
        }
        char path[256];
        sticker_path(path, sizeof(path), view.stk[i].jid, view.stk[i].id, 96);
        vita2d_texture *t = tex_get(path);
        if (t) draw_sticker(t, cx + 8, cy + 8, cell - 16, 80);
        else rrect(cx + 20, cy + 20, cell - 40, 56, 12, C_PHOTO_BG);
    }
    if (view.nstk > 8) {
        char pg[32];
        snprintf(pg, sizeof(pg), "%d / %d", stk_sel + 1, view.nstk);
        txt_right(F_REG, SCREEN_W - 20, (int)py + 186, C_FAINT, 12, pg);
    }
}

// ---------- panel de emojis ----------
#define EMO_COLS 18
#define EMO_ROWS 3
#define RECENT_MAX 36
#define RECENT_PATH "ux0:data/VitaWA/emojis.txt"
static char recents[RECENT_MAX][24];
static int nrecents = -1;

static void recents_load(void) {
    nrecents = 0;
    FILE *f = fopen(RECENT_PATH, "r");
    if (!f) return;
    char line[32];
    while (nrecents < RECENT_MAX && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) snprintf(recents[nrecents++], 24, "%s", line);
    }
    fclose(f);
}
static void recents_push(const char *hex) {
    if (nrecents < 0) recents_load();
    int at = nrecents < RECENT_MAX ? nrecents : RECENT_MAX - 1;
    for (int i = 0; i < nrecents; i++) if (!strcmp(recents[i], hex)) { at = i; break; }
    if (at == nrecents) nrecents++;
    memmove(recents[1], recents[0], at * 24);
    snprintf(recents[0], 24, "%s", hex);
    FILE *f = fopen(RECENT_PATH, "w");
    if (!f) return;
    for (int i = 0; i < nrecents; i++) fprintf(f, "%s\n", recents[i]);
    fclose(f);
}

static int emo_count(void) {
    if (emo_tab == 0) return nrecents < 0 ? 0 : nrecents;
    return emo_tab <= view.ncats ? view.cats[emo_tab - 1].n : 0;
}
static const char *emo_name(int i) {
    if (emo_tab == 0) return recents[i];
    return view.cats[emo_tab - 1].names[i];
}
static const char *emo_tab_name(int t) { return t == 0 ? "Recientes" : t <= view.ncats ? view.cats[t - 1].name : ""; }

// "1f468-200d-1f469" -> UTF-8
static void hex_to_utf8(const char *hex, char *out, int cap) {
    int n = 0;
    const char *p = hex;
    while (*p && n < cap - 5) {
        unsigned int cp = (unsigned int)strtoul(p, (char **)&p, 16);
        if (cp < 0x80) out[n++] = cp;
        else if (cp < 0x800) { out[n++] = 0xC0 | (cp >> 6); out[n++] = 0x80 | (cp & 0x3F); }
        else if (cp < 0x10000) { out[n++] = 0xE0 | (cp >> 12); out[n++] = 0x80 | ((cp >> 6) & 0x3F); out[n++] = 0x80 | (cp & 0x3F); }
        else { out[n++] = 0xF0 | (cp >> 18); out[n++] = 0x80 | ((cp >> 12) & 0x3F); out[n++] = 0x80 | ((cp >> 6) & 0x3F); out[n++] = 0x80 | (cp & 0x3F); }
        if (*p == '-') p++;
        else break;
    }
    out[n] = 0;
}

static void emo_enter(const char *react_id) {
    if (nrecents < 0) recents_load();
    if (view.ncats == 0) job_emojis();
    snprintf(emo_react_id, sizeof(emo_react_id), "%s", react_id ? react_id : "");
    emo_open = 1;
    emo_tab = nrecents > 0 ? 0 : 1;
    emo_sel = 0;
    stk_open = attach_open = 0;
    audio_sfx(SFX_SELECT);
}

static void draw_emoji_panel(void) {
    float py = SCREEN_H - 36 - 196;
    vita2d_draw_rectangle(0, py, SCREEN_W, 196, C_BAR);
    gloss(0, py, SCREEN_W, 40, 0);
    vita2d_draw_rectangle(0, py, SCREEN_W, 1, C_LINE);
    // pestañas: la actual con nombre, las demás como puntitos
    int ntabs = 1 + view.ncats;
    const char *nm = emo_tab_name(emo_tab);
    keycap(20, (int)py + 22, "L");
    txt(F_BOLD, 50, (int)py + 28, C_TEXT, 16, nm, -1);
    int nx = 50 + txt_w(F_BOLD, 16, nm, -1) + 12;
    keycap(nx, (int)py + 22, "R");
    for (int t = 0; t < ntabs; t++) {
        float dx = 300 + t * 22;
        circle(dx, py + 22, t == emo_tab ? 6 : 4, t == emo_tab ? C_ACC : C_LINE);
        if (tap_in(dx - 11, py + 8, 22, 28)) { emo_tab = t; emo_sel = 0; }
    }
    txt_right(F_REG, SCREEN_W - 20, (int)py + 27, C_FAINT, 13, emo_react_id[0] ? "Elige una reacción" : "× Enviar emoji");
    if (emo_tab > 0 && view.ncats == 0) { spinner(480, py + 110, frame); return; }
    int n = emo_count();
    if (n == 0) { txt_center(F_REG, 480, (int)py + 110, C_SUB, 15, "Aquí aparecen los que más usas"); return; }
    float cell = (SCREEN_W - 40) / (float)EMO_COLS;
    int row0 = emo_sel / EMO_COLS - 1;
    if (row0 > (n - 1) / EMO_COLS - (EMO_ROWS - 1)) row0 = (n - 1) / EMO_COLS - (EMO_ROWS - 1);
    if (row0 < 0) row0 = 0;
    for (int i = row0 * EMO_COLS; i < n && i < (row0 + EMO_ROWS) * EMO_COLS; i++) {
        int k = i - row0 * EMO_COLS;
        float cx = 20 + (k % EMO_COLS) * cell, cy = py + 46 + (k / EMO_COLS) * 48;
        if (i == emo_sel) focus_card(cx + 2, cy, cell - 4, 46, 12);
        if (tap_in(cx, cy, cell, 46)) {
            if (i == emo_sel) press_next(SCE_CTRL_CROSS);
            emo_sel = i;
        }
        char path[96];
        snprintf(path, sizeof(path), "/v1/emoji?n=%s&s=40", emo_name(i));
        vita2d_texture *t = tex_get(path);
        if (t) draw_texture_contain(t, cx + (cell - 34) / 2, cy + 6, 34, 34);
        else circle(cx + cell / 2, cy + 23, 6, C_SOFT);
    }
    if (n > EMO_COLS * EMO_ROWS) {
        float th = 140, tt = th * EMO_ROWS / (float)((n + EMO_COLS - 1) / EMO_COLS);
        float ty = py + 48 + (th - tt) * row0 / (float)(((n + EMO_COLS - 1) / EMO_COLS) - EMO_ROWS > 0 ? ((n + EMO_COLS - 1) / EMO_COLS) - EMO_ROWS : 1);
        rrect(SCREEN_W - 12, ty, 4, tt, 2, C_LINE);
    }
}

static void input_emoji_panel(void) {
    int ntabs = 1 + view.ncats;
    int tab = emo_tab;
    if (btn(SCE_CTRL_LTRIGGER)) tab = (emo_tab + ntabs - 1) % ntabs;
    if (btn(SCE_CTRL_RTRIGGER)) tab = (emo_tab + 1) % ntabs;
    if (tab != emo_tab) { emo_tab = tab; emo_sel = 0; audio_sfx(SFX_MOVE); }
    int n = emo_count();
    if (n > 0) {
        int old = emo_sel;
        if (rep(SCE_CTRL_RIGHT) && emo_sel < n - 1) emo_sel++;
        if (rep(SCE_CTRL_LEFT) && emo_sel > 0) emo_sel--;
        if (rep(SCE_CTRL_DOWN) && emo_sel + EMO_COLS < n) emo_sel += EMO_COLS;
        if (rep(SCE_CTRL_UP) && emo_sel >= EMO_COLS) emo_sel -= EMO_COLS;
        if (emo_sel >= n) emo_sel = n - 1;
        if (emo_sel != old) audio_sfx(SFX_MOVE);
        if (btn(SCE_CTRL_CROSS)) {
            char hex[24];
            snprintf(hex, sizeof(hex), "%s", emo_name(emo_sel));
            if (emo_react_id[0]) {
                job_react(open_jid, emo_react_id, hex);
                emo_open = 0;
            } else {
                char u[32];
                hex_to_utf8(hex, u, sizeof(u));
                job_send(open_jid, reply_id, u);
                reply_id[0] = '\0';
                stick_bottom = 1;
                conv_sc.target = 0;
            }
            recents_push(hex);
            audio_sfx(SFX_SEND);
        }
    }
    if (btn(SCE_CTRL_CIRCLE) || btn(SCE_CTRL_TRIANGLE)) { emo_open = 0; audio_sfx(SFX_BACK); }
}

// ---------- reproductor de video / GIF ----------
static void vid_enter(const Msg *m) {
    snprintf(vid_id, sizeof(vid_id), "%s", m->id);
    vid_gif = !strcmp(m->kind, "gif");
    vid_started = 0;
    vid_open = 1;
    audio_stop();
    job_video(open_jid, m->id);
    audio_sfx(SFX_SELECT);
}
static void vid_leave(void) {
    if (vid_started) video_close();
    vid_open = vid_started = 0;
    audio_sfx(SFX_BACK);
}

static void draw_video(void) {
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, HEX(0x05080C));
    int mine = !strcmp(view.video_id, vid_id);
    int st = mine ? view.video_state : 1;
    if (st == 2 && !vid_started) {
        vid_started = 1;
        if (!video_open(view.video_path, vid_gif)) { vid_started = 0; st = 3; }
    }
    if (vid_started) {
        video_draw(0, 0, SCREEN_W, SCREEN_H);
        float p = video_progress();
        if (!vid_gif) {
            rrect(0, SCREEN_H - 50, SCREEN_W, 50, 0, HEXA(0x000000, 0x80));
            rrect(90, SCREEN_H - 28, SCREEN_W - 200, 6, 3, HEXA(0xFFFFFF, 0x50));
            rrect(90, SCREEN_H - 28, (SCREEN_W - 200) * p, 6, 3, C_ACC);
            circle(90 + (SCREEN_W - 200) * p, SCREEN_H - 25, 8, C_WHITE);
            char a[16], b[16];
            fmt_secs(a, sizeof(a), video_position());
            fmt_secs(b, sizeof(b), video_duration());
            txt_right(F_BOLD, 76, SCREEN_H - 19, C_WHITE, 13, a);
            txt(F_BOLD, SCREEN_W - 100, SCREEN_H - 19, C_WHITE, 13, b, -1);
            if (video_paused() || video_ended()) {
                circle(480, 250, 36, HEXA(0x000000, 0x90));
                icon_play(484, 250, 15, C_WHITE);
            }
        }
    } else if (st == 3) {
        txt_center(F_BOLD, 480, 262, HEX(0xE0E6EC), 17, "No se pudo preparar el video");
        txt_center(F_REG, 480, 290, HEX(0x9AA6B2), 14, "Puede ser muy grande (más de 16 MB) o el bridge no tiene ffmpeg");
    } else {
        spinner(480, 250, frame);
        txt_center(F_REG, 480, 300, HEX(0xB8C3CE), 15, vid_gif ? "Cargando GIF…" : "Preparando el video para la Vita…");
    }
    rrect(16, 14, 110, 36, 18, HEXA(0x000000, 0x90));
    glyph(GLYPH_CIRCLE, 38, 32, 18, HEX(0x101820));
    txt(F_REG, 54, 37, C_WHITE, 14, "Cerrar", -1);
    touch_zone(0, 0, 160, 60, SCE_CTRL_CIRCLE);
    if (vid_started && !vid_gif) {
        rrect(SCREEN_W - 150, 14, 134, 36, 18, HEXA(0x000000, 0x90));
        glyph(GLYPH_CROSS, SCREEN_W - 128, 32, 18, HEX(0x101820));
        txt(F_REG, SCREEN_W - 112, 37, C_WHITE, 14, video_paused() ? "Seguir" : "Pausa", -1);
        touch_zone(0, 60, SCREEN_W, SCREEN_H - 120, SCE_CTRL_CROSS);
    }
}

static void input_video(void) {
    if (btn(SCE_CTRL_CIRCLE)) { vid_leave(); return; }
    if (vid_started && !vid_gif && btn(SCE_CTRL_CROSS)) {
        if (video_ended()) {
            video_close();
            vid_started = video_open(view.video_path, 0);
        } else video_toggle_pause();
    }
}

// ---------- hoja de adjuntar ----------
#define ATT_N 5
static const char *ATTACH_LABELS[ATT_N] = {"Cámara", "Galería", "Nota de voz", "Stickers", "Emojis"};
static const unsigned int ATTACH_COLORS[ATT_N] = {0xD9468F, 0x7B4FD6, 0xE07A1F, 0x1F9D63, 0xF2B01E};
#define ATT_W 520
#define ATT_H 150

static void draw_attach(void) {
    float x = AX, y = composer_y() - ATT_H - 10;
    shadow(x, y, ATT_W, ATT_H, 18);
    card(x, y, ATT_W, ATT_H, 18, C_CARD, C_LINE);
    gloss(x, y, ATT_W, ATT_H, 18);
    for (int i = 0; i < ATT_N; i++) {
        float cx = x + 52 + i * 104, cy = y + 60;
        if (attach_sel == i) focus_card(cx - 48, y + 12, 96, ATT_H - 24, 14);
        if (tap_in(cx - 48, y + 12, 96, ATT_H - 24)) {
            if (attach_sel == i) press_next(SCE_CTRL_CROSS);
            attach_sel = i;
        }
        circle(cx, cy, 28, HEX(ATTACH_COLORS[i]));
        gloss(cx - 28, cy - 28, 56, 56, 28);
        switch (i) {
        case 0: icon_camera(cx, cy, 12, C_WHITE); break;
        case 1: icon_image(cx, cy, 12, C_WHITE); break;
        case 2: icon_mic(cx, cy, 11, C_WHITE); break;
        case 3: icon_sticker(cx, cy, 12, C_WHITE, HEX(ATTACH_COLORS[i])); break;
        default:   // carita
            circle(cx - 5, cy - 4, 2.5f, C_WHITE);
            circle(cx + 5, cy - 4, 2.5f, C_WHITE);
            arc_dots(cx, cy + 1, 8, 0.4f, 2.74f, 2.4f, C_WHITE);
            break;
        }
        txt_center(attach_sel == i ? F_BOLD : F_REG, (int)cx, (int)cy + 52, C_TEXT, 14, ATTACH_LABELS[i]);
    }
}

// ---------- grabando ----------
static void draw_recording(void) {
    float w = 460, h = 132, x = AX + (AW - w) / 2, y = composer_y() - h - 12;
    shadow(x, y, w, h, 18);
    card(x, y, w, h, 18, C_CARD, C_LINE);
    float s = audio_rec_seconds();
    char t[24];
    fmt_secs(t, sizeof(t), s);
    float pulse = 0.5f + 0.5f * sinf(frame * 0.15f);
    circle(x + 34, y + 36, 7 + pulse * 2, C_RED);
    txt(F_XB, (int)x + 52, (int)y + 44, C_TEXT, 22, t, -1);
    char lim[40];
    snprintf(lim, sizeof(lim), "máx. %d:%02d", REC_MAX_SECONDS / 60, REC_MAX_SECONDS % 60);
    txt_right(F_REG, (int)(x + w - 20), (int)y + 42, C_FAINT, 12, lim);
    float lv[48];
    audio_rec_levels(lv, 48);
    for (int i = 0; i < 48; i++) {
        float bh = 4 + lv[i] * 34;
        rrect(x + 22 + i * 8.8f, y + 76 - bh / 2, 4, bh, 2, i > 40 ? C_ACC : HEX(0xB7C2CD));
    }
    const char *how = rec_mode == 1 ? "Suelta R para enviar · ○ cancela" : rec_mode == 2 ? "Suelta el dedo para enviar" : "× Enviar · ○ Cancelar";
    txt_center(F_REG, (int)(x + w / 2), (int)(y + h - 16), C_SUB2, 13, how);
}

// ---------- visor de foto ----------
static void draw_viewer(void) {
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, HEX(0x0B1117));
    char path[256];
    media_path(path, sizeof(path), open_jid, viewer_id, 960);
    vita2d_texture *t = tex_get(path);
    if (t) draw_texture_contain(t, 0, 0, SCREEN_W, SCREEN_H);
    else if (tex_failed(path)) txt_center(F_REG, 480, 272, HEX(0xB8C3CE), 15, "No se pudo cargar la foto");
    else spinner(480, 272, frame);
    rrect(16, 14, 110, 36, 18, HEXA(0x000000, 0x90));
    glyph(GLYPH_CIRCLE, 38, 32, 18, HEX(0x101820));
    txt(F_REG, 54, 37, C_WHITE, 14, "Cerrar", -1);
    touch_zone(0, 0, SCREEN_W, SCREEN_H, SCE_CTRL_CIRCLE);
}

// ---------- pantalla ----------
static const Msg *sel_msg(void) {
    if (msg_sel < 0 || msg_sel >= view.conv.n) return NULL;
    return &view.conv.msgs[msg_sel];
}

void draw_chat(void) {
    if (viewer_open) { draw_viewer(); return; }
    if (vid_open) { draw_video(); return; }
    Conv *c = &view.conv;
    const Chat *ch = chat_by_jid(open_jid);
    const char *name = c->name[0] ? c->name : (ch ? ch->name : "");

    float ab = area_bottom();
    measure_conv(c, (int)(AW * 0.62f));
    draw_conv(c, AX, AREA_TOP, AW, ab - AREA_TOP, conv_sc.pos, msg_sel, !chat_overlay_open());

    header_bar();
    icon_back(22, 28, C_SUB2);
    touch_zone(0, 0, 50, 56, SCE_CTRL_CIRCLE);
    avatar(68, 28, 19, name, ch ? ch->color : hash_color(open_jid), open_jid);
    txt_fit(F_BOLD, 98, c->sub[0] ? 25 : 34, C_TEXT, 17, name, 560);
    if (c->sub[0]) txt_fit(F_REG, 98, 44, is_typing(c->sub) ? C_ONLINE : C_SUB, 12, c->sub, 600);
    if (c->sub[0] && is_typing(c->sub)) typing_dots(98 + txt_fit_w(F_REG, 12, c->sub, 600) + 8, 40, C_ONLINE);
    touch_zone(50, 0, 620, 56, SCE_CTRL_SELECT);
    hint_key(SCREEN_W - 120, 28, "SELECT", SCE_CTRL_SELECT, "Info");

    // "ir abajo" si hay scroll
    if (conv_sc.pos > 200 && !chat_overlay_open()) {
        float bx = AX + AW + 40, by = ab - 30;
        shadow(bx - 20, by - 20, 40, 40, 20);
        circle(bx, by, 20, C_CARD);
        thick_line(bx - 7, by - 3, bx, by + 4, 2.4f, C_SUB2);
        thick_line(bx, by + 4, bx + 7, by - 3, 2.4f, C_SUB2);
        touch_zone(bx - 22, by - 22, 44, 44, SCE_CTRL_RTRIGGER | SCE_CTRL_LTRIGGER);
    }

    float cy = composer_y();
    if (reply_id[0]) {
        float rh = 58, ry = cy - rh + 4;
        card(AX, ry, AW, rh - 8, 12, C_CARD, C_LINE);
        vita2d_draw_rectangle(AX + 10, ry + 8, 3, rh - 24, C_ACC);
        char who[96];
        snprintf(who, sizeof(who), "Respondiendo a %s", reply_name);
        txt_fit(F_BOLD, AX + 22, (int)ry + 20, C_ACC, 13, who, AW - 140);
        txt_fit(F_REG, AX + 22, (int)ry + 39, C_SUB, 14, reply_text, AW - 140);
        glyph(GLYPH_CIRCLE, AX + AW - 74, ry + 25, 16, C_CARD);
        txt(F_REG, AX + AW - 62, (int)ry + 30, C_SUB, 12, "quitar", -1);
    }

    // campo + adjuntar + microfono
    float fw = AW - 104;
    card(AX, cy + 6, fw, 44, 22, C_CARD, C_LINE);
    int kx = AX + 14;
    kx += keycap(kx, (int)cy + 28, "START") + 8;
    txt(F_REG, kx, (int)cy + 33, C_SUB, 15, clipboard[0] ? "Escribe un mensaje (con lo copiado)" : "Escribe un mensaje", -1);
    touch_zone(AX, cy + 6, fw, 44, SCE_CTRL_START);

    float ax2 = AX + fw + 28, mx = AX + AW - 24, my = cy + 28;
    circle(ax2, my, 22, attach_open ? C_ACC : C_CARD);
    if (!attach_open) ring(ax2, my, 22, 1, C_LINE, C_CARD);
    icon_clip(ax2, my, 10, attach_open ? C_WHITE : C_SUB2);
    glyph(GLYPH_TRIANGLE, ax2 + 16, my + 16, 14, C_BG);
    touch_zone(ax2 - 22, my - 22, 44, 44, SCE_CTRL_TRIANGLE);

    circle(mx, my, 24, rec_mode ? C_RED : C_ACC);
    icon_mic(mx, my, 10, C_WHITE);
    keycap((int)mx + 6, (int)my + 18, "R");

    if (stk_open) draw_sticker_panel();
    if (emo_open) draw_emoji_panel();
    if (attach_open) draw_attach();
    if (rec_mode) draw_recording();

    const Msg *m = sel_msg();
    if (menu_open && m) {
        vita2d_draw_rectangle(0, 56, SCREEN_W, 452, C_DIM);
        if (sel_top >= 0) {
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(AX - 8, AREA_TOP, AX + AW + 8, (int)ab);
            draw_msg(c, &c->msgs[msg_sel], AX, AW, sel_top, 1);
            vita2d_disable_clipping();
        }
        draw_menu(m);
    }

    footer_bar();
    int x = 18;
    if (menu_open) {
        x = hint(x, 526, GLYPH_CROSS, "Elegir");
        x = hint(x, 526, GLYPH_CIRCLE, "Cerrar menú");
        txt(F_REG, x, 531, C_SUB2, 13, "Izq / Der  Reacción", -1);
    } else if (rec_mode) {
        if (rec_mode == 3) x = hint(x, 526, GLYPH_CROSS, "Enviar");
        x = hint(x, 526, GLYPH_CIRCLE, "Cancelar");
    } else if (attach_open) {
        x = hint(x, 526, GLYPH_CROSS, "Elegir");
        x = hint(x, 526, GLYPH_CIRCLE, "Cerrar");
    } else if (emo_open) {
        x = hint(x, 526, GLYPH_CROSS, emo_react_id[0] ? "Reaccionar" : "Enviar");
        x = hint(x, 526, GLYPH_CIRCLE, "Cerrar panel");
        txt(F_REG, x, 531, C_SUB2, 13, "L / R  Categoría", -1);
    } else if (stk_open) {
        x = hint(x, 526, GLYPH_CROSS, "Enviar sticker");
        x = hint(x, 526, GLYPH_CIRCLE, "Cerrar panel");
        x = hint(x, 526, GLYPH_SQUARE, stk_tab == 1 ? "Quitar de favoritos" : "Favorito");
    } else {
        if (m && is_audio(m)) {
            char cur[48]; float p, tt; int pl;
            audio_state(cur, sizeof(cur), &p, &tt, &pl);
            x = hint(x, 526, GLYPH_CROSS, !strcmp(cur, m->id) && pl ? "Pausar" : "Escuchar");
        } else if (m && !strcmp(m->kind, "image")) {
            x = hint(x, 526, GLYPH_CROSS, "Ver foto");
        } else if (m && (!strcmp(m->kind, "video") || !strcmp(m->kind, "gif"))) {
            x = hint(x, 526, GLYPH_CROSS, "Reproducir");
        } else {
            x = hint(x, 526, GLYPH_CROSS, "Escribir");
        }
        x = hint(x, 526, GLYPH_CIRCLE, reply_id[0] ? "Quitar respuesta" : "Volver");
        x = hint(x, 526, GLYPH_SQUARE, "Acciones");
        x = hint(x, 526, GLYPH_TRIANGLE, "Adjuntar");
        x = hint_key(x, 526, "R", 0, "Grabar");
        x = hint_key(x, 526, "L", SCE_CTRL_LTRIGGER, "Buscar");
    }
}

// ---------- acciones ----------
static void ensure_sel_visible(void);
static void set_reply(const Msg *m) {
    snprintf(reply_id, sizeof(reply_id), "%s", m->id);
    snprintf(reply_name, sizeof(reply_name), "%s", m->mine ? "ti" : m->sender[0] ? m->sender : view.conv.name);
    if (is_media(m) || !strcmp(m->kind, "sticker") || is_audio(m) || !strcmp(m->kind, "doc")) {
        const char *label = !strcmp(m->kind, "sticker") ? "Sticker" : is_audio(m) ? "Nota de voz" : !strcmp(m->kind, "doc") ? "Documento" : !strcmp(m->kind, "image") ? "Foto" : "Video";
        snprintf(reply_text, sizeof(reply_text), "%s%s%.140s", label, m->nlines && m->text[0] ? " · " : "", m->nlines ? m->text : "");
    } else {
        int n = 0;
        for (const char *p = m->text; *p && n < (int)sizeof(reply_text) - 1; p++) reply_text[n++] = *p == '\n' ? ' ' : *p;
        reply_text[n] = '\0';
    }
}

static void write_message(void) {
    char title[128];
    if (reply_id[0]) snprintf(title, sizeof(title), "Respuesta para %s", reply_name);
    else snprintf(title, sizeof(title), "Mensaje para %s", view.conv.name[0] ? view.conv.name : "el chat");
    audio_sfx(SFX_SELECT);
    ime_ask(IMEP_MSG, title, clipboard, IME_KIND_TEXT, 1000);
    clipboard[0] = '\0';
}

static void find_next(int from) {
    char hay[1100];
    for (int i = from; i >= 0; i--) {
        const Msg *m = &view.conv.msgs[i];
        strip_emoji(m->text, hay, sizeof(hay));
        for (char *p = hay; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        if (strstr(hay, find_q)) {
            msg_sel = i;
            stick_bottom = msg_sel == view.conv.n - 1;
            ensure_sel_visible();
            audio_sfx(SFX_MOVE);
            return;
        }
    }
    audio_sfx(SFX_ERROR);
    show_toast(from == view.conv.n - 1 ? "No está en los mensajes cargados" : "No hay más coincidencias arriba", 1);
}

void chat_ime_result(int purpose, const char *text) {
    if (purpose == IMEP_FIND) {
        if (!text || !text[0]) return;
        snprintf(find_q, sizeof(find_q), "%s", text);
        for (char *p = find_q; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        find_next(view.conv.n - 1);
        return;
    }
    if (purpose == IMEP_EDIT) {
        if (text && text[0] && edit_id[0]) job_edit(open_jid, edit_id, text);
        edit_id[0] = '\0';
        return;
    }
    if (!text || !text[0]) return;
    job_send(open_jid, reply_id, text);
    reply_id[0] = '\0';
    stick_bottom = 1;
    conv_sc.target = 0;
}

static void msg_primary(const Msg *m) {
    if (is_audio(m)) {
        char cur[48]; float p, t; int pl;
        audio_state(cur, sizeof(cur), &p, &t, &pl);
        if (!strcmp(cur, m->id)) audio_toggle_pause();
        else if (!audio_is_loading(m->id)) job_audio(open_jid, m->id);
        audio_sfx(SFX_SELECT);
    } else if (!strcmp(m->kind, "image")) {
        snprintf(viewer_id, sizeof(viewer_id), "%s", m->id);
        viewer_open = 1;
        audio_sfx(SFX_SELECT);
    } else if (!strcmp(m->kind, "video") || !strcmp(m->kind, "gif")) {
        vid_enter(m);
    } else {
        write_message();
    }
}

static void rec_begin(int mode) {
    if (audio_rec_start() == 0) {
        rec_mode = mode;
        attach_open = stk_open = 0;
        audio_sfx(SFX_REC_START);
    } else {
        audio_sfx(SFX_ERROR);
        show_toast("No pude abrir el micrófono", 1);
    }
}

static void rec_end(int send) {
    if (!rec_mode) return;
    rec_mode = 0;
    if (!send) {
        audio_rec_cancel();
        audio_sfx(SFX_BACK);
        show_toast("Nota de voz descartada", 0);
        return;
    }
    unsigned char *mu = NULL;
    int n = audio_rec_finish(&mu);
    audio_sfx(SFX_REC_STOP);
    if (n <= 0) {
        free(mu);
        show_toast("Muy corta: mantén para grabar", 0);
        return;
    }
    job_voice(open_jid, reply_id, mu, n);
    reply_id[0] = '\0';
    stick_bottom = 1;
    conv_sc.target = 0;
}

static void ensure_sel_visible(void) {
    if (msg_sel < 0) return;
    float ah = area_bottom() - AREA_TOP;
    scroller_show(&conv_sc, off_b[msg_sel] - 4, hgt[msg_sel] + (needs_day(&view.conv, msg_sel) ? 34 : 0) + 8, ah);
}

void input_chat(void) {
    Conv *c = &view.conv;
    int tm = tapped_msg;
    tapped_msg = -1;

    if (viewer_open) {
        if (btn(SCE_CTRL_CIRCLE) || btn(SCE_CTRL_CROSS)) { viewer_open = 0; audio_sfx(SFX_BACK); }
        return;
    }
    if (vid_open) { input_video(); return; }
    if (emo_open) { input_emoji_panel(); return; }

    // ---- grabando ----
    if (rec_mode) {
        int over = audio_rec_seconds() >= REC_MAX_SECONDS;
        if (btn(SCE_CTRL_CIRCLE)) rec_end(0);
        else if (rec_mode == 1 && !held(SCE_CTRL_RTRIGGER)) rec_end(1);
        else if (rec_mode == 2 && !T.down) rec_end(1);
        else if (rec_mode == 3 && btn(SCE_CTRL_CROSS)) rec_end(1);
        else if (over) rec_end(1);
        return;
    }

    if (menu_open) {
        const Msg *m = sel_msg();
        if (!m) { menu_open = 0; return; }
        int old = menu_sel * 10 + react_sel;
        if (rep(SCE_CTRL_UP) && menu_sel > -1) menu_sel--;
        if (rep(SCE_CTRL_DOWN) && menu_sel < MENU_N - 1) menu_sel++;
        if (menu_sel == -1) {
            if (rep(SCE_CTRL_LEFT) && react_sel > 0) react_sel--;
            if (rep(SCE_CTRL_RIGHT) && react_sel < NREACT - 1) react_sel++;
        }
        // toques
        if (T.tapped) {
            float mx = MENU_X, my = MENU_Y;
            int hit = 0;
            for (int i = 0; i < NREACT; i++)
                if (tap_in(mx + 25 + i * 40 - 20, my + 8, 40, 46)) { menu_sel = -1; react_sel = i; hit = 1; }
            for (int i = 0; i < MENU_N; i++)
                if (tap_in(mx, my + 68 + i * 44, MENU_W, 44)) { menu_sel = i; hit = 1; }
            if (hit) pressed |= SCE_CTRL_CROSS;
            else pressed |= SCE_CTRL_CIRCLE;
        }
        if (menu_sel * 10 + react_sel != old) audio_sfx(SFX_MOVE);
        if (btn(SCE_CTRL_CIRCLE) || btn(SCE_CTRL_SQUARE)) { menu_open = 0; audio_sfx(SFX_BACK); return; }
        if (btn(SCE_CTRL_CROSS)) {
            menu_open = 0;
            audio_sfx(SFX_SELECT);
            int act = menu_sel < 0 ? -1 : menu_items[menu_sel];
            switch (act) {
            case -1:
                if (react_sel < 6) job_react(open_jid, m->id, REACT_SEND[react_sel]);
                else emo_enter(m->id);
                break;
            case MA_REPLY: set_reply(m); break;
            case MA_FWD: open_forward_for(m); break;
            case MA_COPY:
                strip_emoji(m->text, clipboard, sizeof(clipboard));
                show_toast(clipboard[0] ? "Copiado: se pega la próxima vez que escribas" : "Este mensaje no tiene texto", 0);
                break;
            case MA_STAR: job_star(open_jid, m->id, !m->starred); break;
            case MA_INFO:
                if (c->group && !m->mine && m->sender_jid[0]) open_contact(m->sender_jid, SCR_CHAT);
                else open_contact(open_jid, SCR_CHAT);
                break;
            case MA_EDIT: {
                char cur[1024];
                strip_emoji(m->text, cur, sizeof(cur));
                snprintf(edit_id, sizeof(edit_id), "%s", m->id);
                ime_ask(IMEP_EDIT, "Editar mensaje (hasta 15 min después de enviarlo)", cur, IME_KIND_TEXT, 1000);
                break;
            }
            case MA_REVOKE: job_revoke(open_jid, m->id); break;
            case MA_DELETE: job_delete(open_jid, m->id); break;
            }
        }
        return;
    }

    if (attach_open) {
        int old = attach_sel;
        if (rep(SCE_CTRL_RIGHT) && attach_sel < ATT_N - 1) attach_sel++;
        if (rep(SCE_CTRL_LEFT) && attach_sel > 0) attach_sel--;
        if (attach_sel != old) audio_sfx(SFX_MOVE);
        if (btn(SCE_CTRL_CIRCLE) || btn(SCE_CTRL_TRIANGLE)) { attach_open = 0; audio_sfx(SFX_BACK); return; }
        if (T.tapped && !tap_in(AX, composer_y() - ATT_H - 10, ATT_W, ATT_H + 70)) {   // toque fuera de la hoja: cerrar
            attach_open = 0;
            return;
        }
        if (btn(SCE_CTRL_CROSS)) {
            attach_open = 0;
            switch (attach_sel) {
            case 0: camera_enter(); break;
            case 1: gallery_enter(); break;
            case 2: rec_begin(3); break;
            case 4: emo_enter(NULL); break;
            default:
                stk_open = 1;
                stk_sel = 0;
                sync_load_stickers(stk_tab);
                v_stk = -1;
                audio_sfx(SFX_SELECT);
                break;
            }
        }
        return;
    }

    if (stk_open) {
        int tab = stk_tab;
        if (btn(SCE_CTRL_LTRIGGER)) tab = (stk_tab + 2) % 3;
        if (btn(SCE_CTRL_RTRIGGER)) tab = (stk_tab + 1) % 3;
        {
            float py = SCREEN_H - 36 - 196;
            int x = 20;
            const char *tabs[3] = {"Recientes", "Favoritos", "Recibidos"};
            for (int i = 0; i < 3; i++) {
                int w = pill_w(tabs[i], stk_tab == i);
                if (tap_in(x, py + 6, w, 32)) tab = i;
                x += w + 8;
            }
        }
        if (tab != stk_tab) {
            stk_tab = tab;
            stk_sel = 0;
            sync_load_stickers(stk_tab);
            v_stk = -1;
            audio_sfx(SFX_MOVE);
        }
        int ready = view.stk_tab == stk_tab && v_stk >= 0;
        if (ready && view.nstk > 0) {
            int old = stk_sel;
            if (rep(SCE_CTRL_RIGHT) && stk_sel < view.nstk - 1) stk_sel++;
            if (rep(SCE_CTRL_LEFT) && stk_sel > 0) stk_sel--;
            if (rep(SCE_CTRL_DOWN) && stk_sel + 8 < view.nstk) stk_sel += 8;
            if (rep(SCE_CTRL_UP) && stk_sel >= 8) stk_sel -= 8;
            if (stk_sel >= view.nstk) stk_sel = view.nstk - 1;
            if (stk_sel != old) audio_sfx(SFX_MOVE);
            StickerRef *s = &view.stk[stk_sel];
            if (btn(SCE_CTRL_CROSS)) {
                job_sticker(open_jid, s->jid, s->id, reply_id);
                reply_id[0] = '\0';
                stk_open = 0;
                stick_bottom = 1;
                conv_sc.target = 0;
            }
            if (btn(SCE_CTRL_SQUARE)) {
                job_sticker_fav(s->jid, s->id, stk_tab != 1);
                if (stk_tab == 1) { sync_load_stickers(1); v_stk = -1; }
            }
        }
        if (btn(SCE_CTRL_CIRCLE) || btn(SCE_CTRL_TRIANGLE)) { stk_open = 0; audio_sfx(SFX_BACK); }
        return;
    }

    // ---- normal ----
    float ah = area_bottom() - AREA_TOP;
    scroller_update(&conv_sc, AX - 100, AREA_TOP, AW + 200, ah, content_h - ah, 1);
    if (conv_sc.grabbed) stick_bottom = conv_sc.pos < 4;

    int old = msg_sel;
    if (rep(SCE_CTRL_UP) || rep(SCE_CTRL_DOWN)) {
        // si la seleccion quedo fuera de la vista (por arrastrar), empezamos desde lo visible
        if (vis_last >= 0 && (msg_sel < vis_first || msg_sel > vis_last)) msg_sel = vis_last;
        else if (rep(SCE_CTRL_UP) && msg_sel > 0) msg_sel--;
        else if (rep(SCE_CTRL_DOWN) && msg_sel < c->n - 1) msg_sel++;
        stick_bottom = msg_sel == c->n - 1;
    }
    if (btn(SCE_CTRL_RTRIGGER) && btn(SCE_CTRL_LTRIGGER)) {   // boton "ir abajo"
        msg_sel = c->n - 1;
        stick_bottom = 1;
        conv_sc.target = 0;
        pressed &= ~(SCE_CTRL_RTRIGGER | SCE_CTRL_LTRIGGER);
    }
    if (btn(SCE_CTRL_LTRIGGER) && !held(SCE_CTRL_RTRIGGER)) {
        // L: buscar en el chat; si ya buscaste y sigues sobre una coincidencia, salta a la anterior
        if (find_q[0] && msg_sel > 0) {
            char hay[1100];
            strip_emoji(c->msgs[msg_sel].text, hay, sizeof(hay));
            for (char *p = hay; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
            if (strstr(hay, find_q)) { find_next(msg_sel - 1); return; }
        }
        audio_sfx(SFX_SELECT);
        ime_ask(IMEP_FIND, "Buscar en este chat", find_q, IME_KIND_TEXT, 100);
        return;
    }
    if (tm >= 0 && tm < c->n) {
        if (tm == msg_sel) {
            const Msg *m = &c->msgs[tm];
            if (is_audio(m) || is_media(m)) msg_primary(m);
            else { build_menu(m); menu_open = 1; menu_sel = 0; react_sel = 0; audio_sfx(SFX_SELECT); }
            return;
        }
        msg_sel = tm;
        stick_bottom = msg_sel == c->n - 1;
    }
    if (msg_sel != old) {
        audio_sfx(SFX_MOVE);
        if (tm < 0) ensure_sel_visible();
    }

    // microfono: mantener R, o el dedo sobre el boton
    float my = composer_y() + 28, mx = AX + AW - 24;
    if (btn(SCE_CTRL_RTRIGGER)) { rec_begin(1); return; }
    if (T.down && !T.dragging && T.start_x >= mx - 30 && T.start_x <= mx + 30 && T.start_y >= my - 30 && T.start_y <= my + 30 &&
        T.x >= mx - 40 && T.x <= mx + 40 && T.y >= my - 40 && T.y <= my + 40) {
        static int touch_hold = 0;
        if (++touch_hold > 12) { touch_hold = 0; rec_begin(2); return; }
    }

    const Msg *m = sel_msg();
    if (btn(SCE_CTRL_CROSS)) {
        if (m) msg_primary(m);
        else write_message();
    }
    if (btn(SCE_CTRL_START)) write_message();
    if (btn(SCE_CTRL_SQUARE) && m) {
        build_menu(m);
        menu_open = 1;
        menu_sel = 0;
        react_sel = 0;
        audio_sfx(SFX_SELECT);
    }
    if (btn(SCE_CTRL_TRIANGLE)) {
        attach_open = 1;
        attach_sel = 0;
        audio_sfx(SFX_SELECT);
    }
    if (btn(SCE_CTRL_SELECT)) { open_contact(open_jid, SCR_CHAT); return; }
    if (btn(SCE_CTRL_CIRCLE)) {
        if (reply_id[0]) { reply_id[0] = '\0'; audio_sfx(SFX_BACK); }
        else {
            audio_stop();
            go_back_sound(SCR_HOME);
        }
    }

    const Chat *ch = chat_by_jid(open_jid);
    if (ch && ch->unread > 0 && now_ms() - last_read_job > 2000) {
        job_read(open_jid);
        last_read_job = now_ms();
    }
}

// ---------- reenviar ----------
static char fwd_jid[64], fwd_id[48], fwd_preview[300];
static int fwd_sel = 0, fwd_count = 0;
static char fwd_mark[5][64];
static Scroller fwd_sc;
#define FWD_ROW 60

static int fwd_marked(const char *jid) {
    for (int i = 0; i < fwd_count; i++) if (!strcmp(fwd_mark[i], jid)) return i;
    return -1;
}

void open_forward_for(const Msg *m) {
    snprintf(fwd_jid, sizeof(fwd_jid), "%s", open_jid);
    snprintf(fwd_id, sizeof(fwd_id), "%s", m->id);
    if (m->text[0] && !is_audio(m)) {
        int n = 0;
        for (const char *p = m->text; *p && n < (int)sizeof(fwd_preview) - 1; p++) fwd_preview[n++] = *p == '\n' ? ' ' : *p;
        fwd_preview[n] = '\0';
    } else {
        snprintf(fwd_preview, sizeof(fwd_preview), "%s", !strcmp(m->kind, "sticker") ? "Sticker" : is_audio(m) ? "Nota de voz" : !strcmp(m->kind, "image") ? "Foto" : "Archivo");
    }
    fwd_sel = fwd_count = 0;
    fwd_sc.pos = 0;
    fwd_sc.target = -1;
    memset(fwd_mark, 0, sizeof(fwd_mark));
    go(SCR_FORWARD);
}

void draw_forward(void) {
    header_bar();
    icon_back(22, 28, C_SUB2);
    touch_zone(0, 0, 200, 56, SCE_CTRL_CIRCLE);
    txt(F_XB, 44, 36, C_TEXT, 20, "Reenviar a…", -1);
    char cnt[48];
    snprintf(cnt, sizeof(cnt), "%d de 5 seleccionados", fwd_count);
    txt_right(F_REG, SCREEN_W - 18, 33, C_SUB, 13, cnt);

    txt(F_BOLD, 30, 84, C_SUB, 12, "CHATS RECIENTES", -1);
    float top = 96, bottom = 508;
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(0, (int)top, 620, (int)bottom);
    for (int idx = 0; idx < view.nchats; idx++) {
        float y = top + idx * FWD_ROW - fwd_sc.pos;
        if (y > bottom) break;
        if (y + FWD_ROW < top) continue;
        const Chat *c = &view.chats[idx];
        if (idx == fwd_sel) focus_card(14, y, 592, FWD_ROW - 4, 12);
        int marked = fwd_marked(c->jid) >= 0;
        if (marked) {
            rrect(26, y + 16, 24, 24, 7, C_ACC);
            thick_line(31, y + 28, 36, y + 33, 2.6f, C_WHITE);
            thick_line(36, y + 33, 45, y + 23, 2.6f, C_WHITE);
        } else {
            card(26, y + 16, 24, 24, 7, C_CARD, HEX(0xB7C2CD));
        }
        avatar(84, y + 28, 20, c->name, c->color, c->jid);
        txt_fit(F_BOLD, 116, (int)y + 25, C_TEXT, 16, c->name, 460);
        txt(F_REG, 116, (int)y + 45, C_SUB, 12, c->group ? "Grupo" : "Chat", -1);
    }
    vita2d_disable_clipping();

    float ax = 620, aw = SCREEN_W - ax;
    vita2d_draw_rectangle(ax, 56, aw, 452, C_BAR);
    vita2d_draw_rectangle(ax, 56, 1, 452, C_LINE);
    txt(F_BOLD, (int)ax + 20, 84, C_SUB, 12, "MENSAJE", -1);
    short off[4], len[4];
    int n = wrap(F_REG, 15, fwd_preview, (int)aw - 66, off, len, 4);
    float bh = 34 + n * 20;
    card(ax + 20, 96, aw - 40, bh, 14, C_BUBBLE, C_BUBBLE_LN);
    txt(F_REG, (int)ax + 33, 116, C_FAINT, 12, "→ Reenviado", -1);
    for (int i = 0; i < n; i++) txt(F_REG, (int)ax + 33, (int)(136 + i * 20), C_TEXT, 15, fwd_preview + off[i], len[i]);

    float py = 96 + bh + 26;
    txt(F_BOLD, (int)ax + 20, (int)py, C_SUB, 12, "PARA", -1);
    float cx = ax + 20, cy = py + 12;
    for (int i = 0; i < fwd_count; i++) {
        const Chat *c = chat_by_jid(fwd_mark[i]);
        const char *nm = c ? c->name : fwd_mark[i];
        int w = txt_w(F_BOLD, 14, nm, -1) + 24;
        if (w > aw - 40) w = (int)aw - 40;
        if (cx + w > SCREEN_W - 20) { cx = ax + 20; cy += 36; }
        rrect(cx, cy, w, 30, 15, C_ACC_SOFT);
        txt_fit(F_BOLD, (int)cx + 12, (int)cy + 20, C_ACC, 14, nm, w - 20);
        cx += w + 8;
    }
    float by = 508 - 20 - 52;
    rrect(ax + 20, by, aw - 40, 52, 14, fwd_count ? C_ACC : C_CHIP);
    char lbl[48];
    snprintf(lbl, sizeof(lbl), fwd_count ? "Enviar a %d chat%s" : "Elige al menos un chat", fwd_count, fwd_count == 1 ? "" : "s");
    txt_center(F_XB, (int)(ax + aw / 2), (int)by + 33, fwd_count ? C_WHITE : C_SUB, 16, lbl);
    touch_zone(ax + 20, by, aw - 40, 52, SCE_CTRL_START);

    footer_bar();
    int x = 18;
    x = hint(x, 526, GLYPH_CROSS, "Marcar");
    x = hint(x, 526, GLYPH_CIRCLE, "Cancelar");
    x = hint_key(x, 526, "START", SCE_CTRL_START, "Enviar");
    txt_right(F_REG, SCREEN_W - 18, 531, C_FAINT, 13, "Hasta 5 chats a la vez");
}

void input_forward(void) {
    int old = fwd_sel;
    if (rep(SCE_CTRL_DOWN) && fwd_sel < view.nchats - 1) fwd_sel++;
    if (rep(SCE_CTRL_UP) && fwd_sel > 0) fwd_sel--;
    int toggle = btn(SCE_CTRL_CROSS);
    if (T.tapped && T.tap_x < 620 && T.tap_y >= 96 && T.tap_y < 508) {
        int i = (int)((T.tap_y - 96 + fwd_sc.pos) / FWD_ROW);
        if (i >= 0 && i < view.nchats) { fwd_sel = i; toggle = 1; }
    }
    if (fwd_sel != old) {
        audio_sfx(SFX_MOVE);
        if (!T.tapped) scroller_show(&fwd_sc, fwd_sel * FWD_ROW, FWD_ROW, 508 - 96);
    }
    scroller_update(&fwd_sc, 0, 96, 620, 508 - 96, view.nchats * FWD_ROW - (508 - 96), 0);
    if (toggle && fwd_sel < view.nchats) {
        const char *jid = view.chats[fwd_sel].jid;
        int at = fwd_marked(jid);
        if (at >= 0) {
            memmove(fwd_mark[at], fwd_mark[at + 1], sizeof(fwd_mark[0]) * (fwd_count - at - 1));
            fwd_count--;
            audio_sfx(SFX_MOVE);
        } else if (fwd_count < 5) {
            snprintf(fwd_mark[fwd_count++], sizeof(fwd_mark[0]), "%s", jid);
            audio_sfx(SFX_SELECT);
        } else {
            audio_sfx(SFX_ERROR);
            show_toast("Máximo 5 chats a la vez", 1);
        }
    }
    if (btn(SCE_CTRL_START) && fwd_count > 0) {
        char csv[400] = "";
        for (int i = 0; i < fwd_count; i++) {
            if (i) strncat(csv, ",", sizeof(csv) - strlen(csv) - 1);
            strncat(csv, fwd_mark[i], sizeof(csv) - strlen(csv) - 1);
        }
        job_forward(fwd_jid, fwd_id, csv);
        screen = SCR_CHAT;
    }
    if (btn(SCE_CTRL_CIRCLE)) go_back_sound(SCR_CHAT);
}
