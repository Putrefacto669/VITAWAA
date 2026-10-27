// Info de contacto / grupo y "Nuevo chat".

#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <vita2d.h>

#include "app.h"
#include "audio.h"
#include "ime.h"
#include "touch.h"

// ---------- info de contacto ----------
// pila pequeña para volver (grupo -> miembro -> ...)
static struct { char jid[64]; int back_to; } cstack[6];
static int cdepth = 0;
static int c_sel = 0;          // -1 = boton "Enviar mensaje"; >= 0 fila seleccionable
static Scroller c_sc;

void open_contact(const char *jid, int back_to) {
    if (screen == SCR_CONTACT && cdepth < 6) {
        // desde otro contacto: se apila
    } else {
        cdepth = 0;
    }
    if (cdepth >= 6) cdepth = 5;
    snprintf(cstack[cdepth].jid, sizeof(cstack[0].jid), "%s", jid);
    cstack[cdepth].back_to = screen == SCR_CONTACT ? SCR_CONTACT : back_to;
    cdepth++;
    c_sel = -1;
    c_sc.pos = 0;
    c_sc.target = -1;
    job_contact(jid);
    go(SCR_CONTACT);
}

static const char *cur_jid(void) { return cdepth > 0 ? cstack[cdepth - 1].jid : ""; }

static void contact_back(void) {
    if (cdepth <= 1) {
        int to = cdepth == 1 ? cstack[0].back_to : SCR_HOME;
        cdepth = 0;
        go_back_sound(to);
        return;
    }
    cdepth--;
    c_sel = -1;
    c_sc.pos = 0;
    job_contact(cur_jid());
    audio_sfx(SFX_BACK);
}

// filas del panel derecho
enum { R_HEAD, R_TEXT, R_KV, R_GROUP, R_PART, R_GAP };
typedef struct { int kind; const char *a; const char *b; int idx; float y, h; } CRow;
static CRow rows[MAX_PARTS + MAX_CGROUPS + 16];
static int nrows = 0;
static short about_off[12], about_len[12];
static int about_n = 0;

static void add_row(int kind, const char *a, const char *b, int idx, float h) {
    if (nrows >= (int)(sizeof(rows) / sizeof(rows[0]))) return;
    float y = nrows ? rows[nrows - 1].y + rows[nrows - 1].h : 0;
    rows[nrows++] = (CRow){kind, a, b, idx, y, h};
}

#define PX 400
#define PW (SCREEN_W - PX - 30)
#define PTOP 70
#define PBOT 500

static void build_rows(const ContactInfo *ci) {
    nrows = 0;
    static char members[48], about_when[80];
    if (ci->group) {
        add_row(R_HEAD, "DESCRIPCIÓN", NULL, 0, 34);
        about_n = ci->about[0] ? wrap(F_REG, 15, ci->about, PW - 40, about_off, about_len, 12) : 0;
        add_row(R_TEXT, ci->about[0] ? ci->about : "Sin descripción", NULL, 0, 20 + (about_n ? about_n : 1) * 21);
        snprintf(members, sizeof(members), "%d MIEMBROS", ci->members ? ci->members : ci->nparts);
        add_row(R_GAP, NULL, NULL, 0, 12);
        add_row(R_HEAD, members, NULL, 0, 34);
        for (int i = 0; i < ci->nparts; i++) add_row(R_PART, ci->parts[i].name, NULL, i, 56);
    } else {
        add_row(R_HEAD, "INFO", NULL, 0, 34);
        about_n = ci->about[0] ? wrap(F_REG, 15, ci->about, PW - 40, about_off, about_len, 12) : 0;
        add_row(R_TEXT, ci->about[0] ? ci->about : "Sin info", NULL, 0, 20 + (about_n ? about_n : 1) * 21 + (ci->about_at[0] ? 18 : 0));
        if (ci->about_at[0]) snprintf(about_when, sizeof(about_when), "%s", ci->about_at);
        add_row(R_GAP, NULL, NULL, 0, 12);
        add_row(R_HEAD, "NOMBRES", NULL, 0, 34);
        add_row(R_KV, "Guardado en tus contactos", ci->saved[0] ? ci->saved : "— (no lo tienes guardado)", 0, 50);
        add_row(R_KV, "Nombre que se puso", ci->push[0] ? ci->push : "—", 0, 50);
        add_row(R_KV, "Número", ci->number[0] ? ci->number : "—", 0, 50);
        if (ci->ngroups) {
            add_row(R_GAP, NULL, NULL, 0, 12);
            add_row(R_HEAD, "GRUPOS EN COMÚN", NULL, 0, 34);
            for (int i = 0; i < ci->ngroups; i++) add_row(R_GROUP, ci->groups[i].name, NULL, i, 56);
        }
    }
}

static int row_selectable(int r) { return r >= 0 && r < nrows && (rows[r].kind == R_GROUP || rows[r].kind == R_PART); }

static int content_ready(void) { return !strcmp(view.contact.jid, cur_jid()) && view.contact.loaded != 0; }

void draw_contact(void) {
    const ContactInfo *ci = &view.contact;
    const char *jid = cur_jid();
    const Chat *ch = chat_by_jid(jid);
    int ready = content_ready() && ci->loaded == 1;
    const char *name = ready && ci->name[0] ? ci->name : ch ? ch->name : "";
    int is_group = strstr(jid, "@g.us") != NULL;

    header_bar();
    icon_back(22, 28, C_SUB2);
    touch_zone(0, 0, 260, 56, SCE_CTRL_CIRCLE);
    txt(F_XB, 44, 36, C_TEXT, 20, is_group ? "Info del grupo" : "Info del contacto", -1);
    battery(SCREEN_W - 18, 28);

    // columna izquierda: foto grande y nombre
    vita2d_draw_rectangle(0, 56, 370, 452, C_BAR);
    vita2d_draw_rectangle(369, 56, 1, 452, C_LINE);
    avatar(185, 134, 64, name, ch ? ch->color : hash_color(jid), jid);
    short off[2], len[2];
    int n = wrap(F_XB, 22, name, 320, off, len, 2);
    for (int i = 0; i < n; i++) {
        int lw = txt_w(F_XB, 22, name + off[i], len[i]);
        txt(F_XB, 185 - lw / 2, 232 + i * 28, C_TEXT, 22, name + off[i], len[i]);
    }
    float y = 232 + n * 26;
    if (ready) {
        if (!is_group && ci->number[0] && strcmp(ci->number, name)) {
            txt_center(F_REG, 185, (int)y, C_SUB2, 15, ci->number);
            y += 22;
        }
        if (ci->sub[0]) txt_center(F_REG, 185, (int)y, strstr(ci->sub, "en línea") ? C_ONLINE : C_SUB, 13, ci->sub);
    }
    // silenciar / archivar / fijar (solo si es un chat de tu lista)
    if (ch) {
        const char *lab[3] = {ch->muted ? "Silenciado" : "Silenciar", ch->archived ? "Archivado" : "Archivar", ch->pinned ? "Fijado" : "Fijar"};
        const char *key[3] = {"L", "R", "△"};
        int on[3] = {ch->muted, ch->archived, ch->pinned};
        unsigned int bt[3] = {SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER, SCE_CTRL_TRIANGLE};
        for (int i = 0; i < 3; i++) {
            float cx = 85 + i * 100, cy = 352;
            circle(cx, cy, 22, on[i] ? C_ACC : C_CARD);
            if (!on[i]) ring(cx, cy, 22, 1, C_LINE, C_CARD);
            gloss(cx - 22, cy - 22, 44, 44, 22);
            unsigned int ic = on[i] ? C_WHITE : C_SUB2;
            if (i == 0) icon_mute(cx, cy, 9, ic);
            else if (i == 1) {
                rrect(cx - 10, cy - 8, 20, 6, 2, ic);
                rrect(cx - 8, cy - 1, 16, 10, 2, ic);
                rrect(cx - 3, cy + 2, 6, 2, 1, on[i] ? C_ACC : C_CARD);
            } else icon_pin(cx, cy, 9, ic);
            txt_center(on[i] ? F_BOLD : F_REG, (int)cx, 392, on[i] ? C_ACC : C_SUB, 12, lab[i]);
            if (i < 2) keycap((int)cx + 12, (int)cy - 20, key[i]);
            else glyph(GLYPH_TRIANGLE, cx + 18, cy - 16, 14, C_BAR);
            touch_zone(cx - 40, cy - 26, 80, 74, bt[i]);
        }
    }
    float by = 428;
    if (c_sel == -1) focus_card(40, by, 290, 50, 25);
    else card(40, by, 290, 50, 25, C_CARD, C_LINE);
    glyph(GLYPH_CROSS, 100, by + 25, 20, c_sel == -1 ? C_ACC_SOFT : C_CARD);
    txt(F_BOLD, 118, (int)by + 31, c_sel == -1 ? C_ACC : C_TEXT, 16, "Enviar mensaje", -1);
    touch_zone(40, by, 290, 50, SCE_CTRL_START);

    // panel derecho
    if (!content_ready()) {
        spinner(PX + PW / 2, 260, frame);
        txt_center(F_REG, PX + PW / 2, 320, C_SUB, 14, "Buscando la info…");
    } else if (ci->loaded < 0) {
        txt_center(F_BOLD, PX + PW / 2, 260, C_RED_DARK, 16, "No pude traer la info");
        txt_center(F_REG, PX + PW / 2, 286, C_SUB, 13, "Revisa la conexión y vuelve a entrar.");
    } else {
        build_rows(ci);
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(PX - 10, PTOP - 10, PX + PW + 10, PBOT + 8);
        for (int r = 0; r < nrows; r++) {
            CRow *row = &rows[r];
            float ry = PTOP + row->y - c_sc.pos;
            if (ry > PBOT || ry + row->h < PTOP - 10) continue;
            switch (row->kind) {
            case R_HEAD:
                txt(F_BOLD, PX + 4, (int)ry + 22, C_SUB, 12, row->a, -1);
                break;
            case R_TEXT:
                card(PX, ry, PW, row->h - 6, 12, C_CARD, C_LINE);
                if (about_n) for (int i = 0; i < about_n; i++)
                    txt(F_REG, PX + 18, (int)(ry + 26 + i * 21), C_TEXT, 15, row->a + about_off[i], about_len[i]);
                else txt(F_REG, PX + 18, (int)ry + 26, C_FAINT, 15, row->a, -1);
                if (!ci->group && ci->about_at[0]) txt(F_REG, PX + 18, (int)(ry + row->h - 16), C_FAINT, 12, ci->about_at, -1);
                break;
            case R_KV:
                card(PX, ry, PW, row->h - 4, 12, C_CARD, C_LINE);
                txt(F_REG, PX + 18, (int)ry + 19, C_FAINT, 12, row->a, -1);
                txt_fit(F_BOLD, PX + 18, (int)ry + 38, C_TEXT, 15, row->b, PW - 36);
                break;
            case R_GROUP: {
                const GroupRef *g = &ci->groups[row->idx];
                if (c_sel == r) focus_card(PX, ry, PW, row->h - 4, 12);
                else card(PX, ry, PW, row->h - 4, 12, C_CARD, C_LINE);
                avatar(PX + 28, ry + 26, 17, g->name, hash_color(g->jid), g->jid);
                txt_fit(F_BOLD, PX + 56, (int)ry + 23, C_TEXT, 15, g->name, PW - 80);
                char m[32];
                snprintf(m, sizeof(m), "%d miembros", g->members);
                txt(F_REG, PX + 56, (int)ry + 41, C_SUB, 12, m, -1);
                break;
            }
            case R_PART: {
                const Participant *p = &ci->parts[row->idx];
                if (c_sel == r) focus_card(PX, ry, PW, row->h - 4, 12);
                else card(PX, ry, PW, row->h - 4, 12, C_CARD, C_LINE);
                avatar(PX + 28, ry + 26, 17, p->name, p->color, p->jid);
                int self = view.me_jid[0] && !strcmp(p->jid, view.me_jid);
                txt_fit(F_BOLD, PX + 56, (int)ry + 31, self ? C_SUB : name_color(p->color), 15, self ? "Tú" : p->name, PW - 170);
                if (p->admin) {
                    int w = txt_w(F_BOLD, 11, "Admin", -1) + 16;
                    rrect(PX + PW - 16 - w, ry + 15, w, 22, 11, C_ACC_SOFT);
                    txt(F_BOLD, PX + PW - 8 - w, (int)ry + 30, C_ACC, 11, "Admin", -1);
                }
                break;
            }
            }
        }
        vita2d_disable_clipping();
    }

    footer_bar();
    int x = 18;
    if (c_sel >= 0 && row_selectable(c_sel)) x = hint(x, 526, GLYPH_CROSS, rows[c_sel].kind == R_GROUP ? "Abrir grupo" : "Ver contacto");
    else x = hint(x, 526, GLYPH_CROSS, "Enviar mensaje");
    x = hint(x, 526, GLYPH_CIRCLE, "Volver");
    if (c_sel >= 0 && row_selectable(c_sel) && rows[c_sel].kind == R_PART) x = hint(x, 526, GLYPH_SQUARE, "Escribirle");
}

void input_contact(void) {
    const ContactInfo *ci = &view.contact;
    if (btn(SCE_CTRL_CIRCLE)) { contact_back(); return; }
    int ready = content_ready() && ci->loaded == 1;
    if (ready) build_rows(ci);
    else nrows = 0;

    int old = c_sel;
    if (rep(SCE_CTRL_DOWN)) {
        int r = c_sel + 1;
        while (r < nrows && !row_selectable(r)) r++;
        if (r < nrows) c_sel = r;
        else if (c_sel == -1) c_sc.target = c_sc.pos + 120;   // sin filas: solo baja el texto
    }
    if (rep(SCE_CTRL_UP)) {
        int r = c_sel - 1;
        while (r >= 0 && !row_selectable(r)) r--;
        c_sel = r >= 0 ? r : -1;
    }
    if (rep(SCE_CTRL_LEFT)) c_sel = -1;
    float vh = PBOT - PTOP;
    int open_row = 0;
    if (T.tapped && T.tap_x >= PX && T.tap_y >= PTOP && T.tap_y < PBOT) {
        float yy = T.tap_y - PTOP + c_sc.pos;
        for (int r = 0; r < nrows; r++)
            if (row_selectable(r) && yy >= rows[r].y && yy < rows[r].y + rows[r].h) { c_sel = r; open_row = 1; }
    }
    if (c_sel != old) {
        audio_sfx(SFX_MOVE);
        if (c_sel >= 0 && !T.tapped) scroller_show(&c_sc, rows[c_sel].y, rows[c_sel].h, vh);
        if (c_sel == -1) c_sc.target = 0;
    }
    float total = nrows ? rows[nrows - 1].y + rows[nrows - 1].h : 0;
    scroller_update(&c_sc, PX - 10, PTOP, PW + 20, vh, total - vh + 10, 0);

    if (btn(SCE_CTRL_START) || ((btn(SCE_CTRL_CROSS)) && c_sel == -1)) {
        open_chat(cur_jid());
        return;
    }
    if ((btn(SCE_CTRL_CROSS) || open_row) && row_selectable(c_sel)) {
        CRow *row = &rows[c_sel];
        if (row->kind == R_GROUP) { open_chat(ci->groups[row->idx].jid); return; }
        const Participant *p = &ci->parts[row->idx];
        if (view.me_jid[0] && !strcmp(p->jid, view.me_jid)) return;
        char j[64];
        snprintf(j, sizeof(j), "%s", p->jid);
        open_contact(j, SCR_CONTACT);
        return;
    }
    {
        const Chat *ch = chat_by_jid(cur_jid());
        if (ch) {
            const char *a = NULL;
            if (btn(SCE_CTRL_LTRIGGER)) a = ch->muted ? "unmute" : "mute";
            if (btn(SCE_CTRL_RTRIGGER)) a = ch->archived ? "unarchive" : "archive";
            if (btn(SCE_CTRL_TRIANGLE)) a = ch->pinned ? "unpin" : "pin";
            if (a) {
                job_chat_action(ch->jid, a);
                audio_sfx(SFX_SELECT);
                show_toast(!strcmp(a, "mute") ? "Chat silenciado: sin avisos" : !strcmp(a, "unmute") ? "Avisos activados" :
                           !strcmp(a, "archive") ? "Chat archivado" : !strcmp(a, "unarchive") ? "Chat desarchivado" :
                           !strcmp(a, "pin") ? "Chat fijado arriba" : "Chat desfijado", 0);
            }
        }
    }
    if (btn(SCE_CTRL_SQUARE) && row_selectable(c_sel) && rows[c_sel].kind == R_PART) {
        const Participant *p = &ci->parts[rows[c_sel].idx];
        if (!(view.me_jid[0] && !strcmp(p->jid, view.me_jid))) open_chat(p->jid);
    }
}

// ---------- nuevo chat ----------
static char nc_query[96];
static int nc_sel = 0;         // 0 = "escribir un numero"; 1.. = contactos (o el resultado del numero)
static Scroller nc_sc;
static int nc_number_mode = 0; // se escribio un numero y se esta revisando / mostrando
#define NC_TOP 150
#define NC_BOT 508
#define NC_ROW 60

void newchat_enter(void) {
    nc_query[0] = '\0';
    nc_sel = 1;
    nc_number_mode = 0;
    nc_sc.pos = 0;
    nc_sc.target = -1;
    job_search("");
    go(SCR_NEWCHAT);
}

void newchat_ime_result(int purpose, const char *text) {
    if (purpose == IMEP_SEARCH) {
        if (!text) return;
        snprintf(nc_query, sizeof(nc_query), "%s", text);
        job_search(nc_query);
        nc_sel = 1;
        nc_sc.pos = 0;
        nc_number_mode = 0;
        return;
    }
    if (purpose == IMEP_NUMBER && text && text[0]) {
        nc_number_mode = 1;
        nc_sel = 0;
        job_check(text);
    }
}

static int nc_count(void) { return 1 + view.nsearch; }

void draw_newchat(void) {
    header_bar();
    icon_back(22, 28, C_SUB2);
    touch_zone(0, 0, 220, 56, SCE_CTRL_CIRCLE);
    txt(F_XB, 44, 36, C_TEXT, 20, "Nuevo chat", -1);
    battery(SCREEN_W - 18, 28);

    // buscador
    card(30, 70, 560, 46, 23, C_CARD, C_LINE);
    icon_search(56, 93, 8, C_SUB);
    if (nc_query[0]) txt_fit(F_REG, 76, 99, C_TEXT, 15, nc_query, 400);
    else txt(F_REG, 76, 99, C_SUB, 15, "Buscar un contacto", -1);
    hint_key(500, 93, "START", SCE_CTRL_START, "");
    touch_zone(30, 70, 560, 46, SCE_CTRL_START);

    txt(F_BOLD, 34, 140, C_SUB, 12, nc_query[0] ? "RESULTADOS" : "TUS CONTACTOS", -1);

    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(0, NC_TOP, 620, NC_BOT);
    int n = nc_count();
    for (int i = 0; i < n; i++) {
        float y = NC_TOP + i * NC_ROW - nc_sc.pos;
        if (y > NC_BOT) break;
        if (y + NC_ROW < NC_TOP) continue;
        int sel = i == nc_sel;
        if (sel) focus_card(20, y, 580, NC_ROW - 4, 12);
        if (i == 0) {
            circle(56, y + 28, 20, C_ACC);
            icon_plus(56, y + 28, 8, C_WHITE);
            txt(F_BOLD, 90, (int)y + 25, C_TEXT, 16, "Escribir un número", -1);
            txt(F_REG, 90, (int)y + 45, C_SUB, 12, "Para alguien que no tienes guardado", -1);
            glyph(GLYPH_SQUARE, 570, y + 28, 18, sel ? C_ACC_SOFT : C_BG);
            continue;
        }
        const ContactRow *r = &view.search[i - 1];
        avatar(56, y + 28, 20, r->name, r->color, r->jid);
        txt_fit(F_BOLD, 90, (int)y + 25, C_TEXT, 16, r->name, 480);
        char sub[160];
        if (r->push[0] && strcmp(r->push, r->name)) snprintf(sub, sizeof(sub), "%s · ~%s", r->number, r->push);
        else snprintf(sub, sizeof(sub), "%s", r->number);
        txt_fit(F_REG, 90, (int)y + 45, C_SUB, 12, sub, 480);
    }
    vita2d_disable_clipping();
    if (n == 1 && v_search >= 0)
        txt_center(F_REG, 310, NC_TOP + 110, C_SUB, 14, nc_query[0] ? "Nadie coincide. Prueba con el número." : "Aún no hay contactos sincronizados");

    // panel derecho: resultado del numero
    float ax = 620, aw = SCREEN_W - ax;
    vita2d_draw_rectangle(ax, 56, aw, 452, C_BAR);
    vita2d_draw_rectangle(ax, 56, 1, 452, C_LINE);
    const CheckResult *ck = &view.check;
    if (nc_number_mode && ck->state == 1) {
        spinner(ax + aw / 2, 240, frame);
        txt_center(F_REG, (int)(ax + aw / 2), 300, C_SUB, 14, "Revisando el número…");
    } else if (nc_number_mode && ck->state == 2) {
        avatar(ax + aw / 2, 190, 44, ck->name, hash_color(ck->jid), ck->jid);
        txt_center(F_XB, (int)(ax + aw / 2), 266, C_TEXT, 18, ck->name[0] ? ck->name : ck->number);
        if (ck->name[0] && strcmp(ck->name, ck->number)) txt_center(F_REG, (int)(ax + aw / 2), 290, C_SUB, 14, ck->number);
        txt_center(F_REG, (int)(ax + aw / 2), 314, C_ONLINE, 13, "Tiene WhatsApp");
        rrect(ax + 24, 350, aw - 48, 50, 14, C_ACC);
        txt_center(F_XB, (int)(ax + aw / 2), 381, C_WHITE, 16, "× Abrir chat");
        touch_zone(ax + 24, 350, aw - 48, 50, SCE_CTRL_CROSS);
    } else if (nc_number_mode && ck->state == 3) {
        txt_center(F_BOLD, (int)(ax + aw / 2), 250, C_RED_DARK, 16, ck->err[0] ? ck->err : "Ese número no tiene WhatsApp");
        txt_center(F_REG, (int)(ax + aw / 2), 278, C_SUB, 13, "Incluye el código de país, ej. +505");
    } else {
        icon_people(ax + aw / 2, 220, 22, C_SUB);
        txt_center(F_BOLD, (int)(ax + aw / 2), 280, C_TEXT, 16, "Empieza una conversación");
        txt_center(F_REG, (int)(ax + aw / 2), 304, C_SUB, 13, "Elige un contacto o escribe un número.");
        txt_center(F_REG, (int)(ax + aw / 2), 324, C_SUB, 13, "Si no pones código de país, uso el tuyo.");
    }

    footer_bar();
    int x = 18;
    if (nc_number_mode && ck->state == 2) x = hint(x, 526, GLYPH_CROSS, "Abrir chat");
    else x = hint(x, 526, GLYPH_CROSS, nc_sel == 0 ? "Escribir número" : "Abrir chat");
    x = hint(x, 526, GLYPH_SQUARE, "Número");
    x = hint_key(x, 526, "START", SCE_CTRL_START, "Buscar");
    x = hint(x, 526, GLYPH_CIRCLE, "Volver");
}

void input_newchat(void) {
    if (btn(SCE_CTRL_CIRCLE)) {
        if (nc_number_mode) { nc_number_mode = 0; audio_sfx(SFX_BACK); }
        else go_back_sound(SCR_HOME);
        return;
    }
    int n = nc_count();
    int old = nc_sel;
    if (rep(SCE_CTRL_DOWN) && nc_sel < n - 1) nc_sel++;
    if (rep(SCE_CTRL_UP) && nc_sel > 0) nc_sel--;
    if (nc_sel >= n) nc_sel = n - 1;
    int open_now = 0;
    if (T.tapped && T.tap_x < 620 && T.tap_y >= NC_TOP && T.tap_y < NC_BOT) {
        int i = (int)((T.tap_y - NC_TOP + nc_sc.pos) / NC_ROW);
        if (i >= 0 && i < n) { nc_sel = i; open_now = 1; }
    }
    if (nc_sel != old) {
        audio_sfx(SFX_MOVE);
        if (!T.tapped) scroller_show(&nc_sc, nc_sel * NC_ROW, NC_ROW, NC_BOT - NC_TOP);
    }
    scroller_update(&nc_sc, 0, NC_TOP, 620, NC_BOT - NC_TOP, n * NC_ROW - (NC_BOT - NC_TOP), 0);

    if (btn(SCE_CTRL_START)) {
        audio_sfx(SFX_SELECT);
        ime_ask(IMEP_SEARCH, "Buscar contacto (nombre o número)", nc_query, IME_KIND_TEXT, 80);
        return;
    }
    if (btn(SCE_CTRL_SQUARE) || ((btn(SCE_CTRL_CROSS) || open_now) && nc_sel == 0 && !(nc_number_mode && view.check.state == 2))) {
        audio_sfx(SFX_SELECT);
        ime_ask(IMEP_NUMBER, "Número con código de país (ej. +505 8888 8888)", "+", IME_KIND_NUMBER, 24);
        return;
    }
    if (btn(SCE_CTRL_CROSS) && nc_number_mode && view.check.state == 2) {
        char j[64];
        snprintf(j, sizeof(j), "%s", view.check.jid);
        open_chat(j);
        return;
    }
    if ((btn(SCE_CTRL_CROSS) || open_now) && nc_sel >= 1 && nc_sel - 1 < view.nsearch) {
        char j[64];
        snprintf(j, sizeof(j), "%s", view.search[nc_sel - 1].jid);
        open_chat(j);
    }
}
