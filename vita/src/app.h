#ifndef VITAWA_APP_H
#define VITAWA_APP_H

// Estado compartido entre las pantallas de VitaWA.

#include <vita2d.h>
#include "sync.h"
#include "ui.h"

enum {
    SCR_PAIR, SCR_SETTINGS, SCR_HOME, SCR_CHAT, SCR_FORWARD, SCR_STATUS,
    SCR_CONTACT, SCR_NEWCHAT, SCR_CAMERA, SCR_GALLERY, SCR_NOTIFS
};

enum {
    IMEP_NONE, IMEP_MSG, IMEP_CAPTION, IMEP_STATUS_REPLY, IMEP_STATUS_POST,
    IMEP_SEARCH, IMEP_NUMBER, IMEP_PAIRCODE, IMEP_WANUM, IMEP_EDIT, IMEP_FIND
};

extern Config cfg;
extern Model view;            // copia local del modelo (se actualiza cada frame)
extern int screen;
extern int frame;
extern int connected;         // bridge OK y WhatsApp abierto
extern unsigned int pressed;  // botones apretados este frame (incluye zonas tactiles)
extern int g_body;            // tamaño de letra de los mensajes (Ajustes)

// version de cada seccion de `view` (para saber si cambio)
extern int v_chats, v_conv, v_st, v_sitems, v_stk, v_contact, v_search, v_check;

int  btn(unsigned int b);
int  rep(unsigned int b);
int  held(unsigned int b);
void press_next(unsigned int b);   // "aprieta" un boton en el proximo frame (para toques detectados al dibujar)

void show_toast(const char *msg, int err);
void go(int scr);
void go_back_sound(int scr);

// Teclado del sistema con un "para que" (el resultado se reparte en main.c)
void ime_ask(int purpose, const char *title, const char *initial, int kind, int maxlen);

// Chat
extern char open_jid[64];
extern char reply_id[48], reply_name[64], reply_text[160];
void open_chat(const char *jid);
const Chat *chat_by_jid(const char *jid);

// Piezas de dibujo compartidas
void brand(void);
void center_msg(const char *title, const char *sub, unsigned int color, int spin);
void media_path(char *out, int cap, const char *jid, const char *id, int max);
void draw_texture_contain(vita2d_texture *t, float x, float y, float bw, float bh);
void sticker_path(char *out, int cap, const char *jid, const char *id, int max);
void draw_sticker(vita2d_texture *t, float x, float y, float bw, float bh);   // anima si es tira
void draw_media_cover(vita2d_texture *t, float x, float y, float bw, float bh);
void photo_placeholder(float x, float y, float w, float h, const char *label);
void masked_number(char *out, int cap);

// Pantallas
void pair_enter(void);
void draw_pair(void);
void input_pair(void);
void pair_ime_result(const char *text);
void draw_settings(void);
void input_settings(void);

void draw_home(void);
void input_home(void);
void home_after_input(void);

void draw_chat(void);
void input_chat(void);
void chat_ime_result(int purpose, const char *text);
int  chat_overlay_open(void);
void draw_forward(void);
void input_forward(void);
void open_forward_for(const Msg *m);
void chat_draw_preview(float ax, float ay, float aw, float ah);
void chat_on_conv_update(void);

void open_status_viewer(const char *poster);
void draw_status_viewer(void);
void input_status(void);
void status_ime_result(int purpose, const char *text);
void draw_status_card(float x, float y, float w, float h, const StatusItem *it, const char *poster);

void open_contact(const char *jid, int back_to);
void draw_contact(void);
void input_contact(void);
void newchat_enter(void);
void draw_newchat(void);
void input_newchat(void);
void newchat_ime_result(int purpose, const char *text);

// Notificaciones (campanita) y banner
void notifs_enter(int back_to);
void draw_notifs(void);
void input_notifs(void);
void bell(float cx, float cy, int unseen);       // icono + globito; tocarlo abre la lista
void notif_line(const Notif *n, char *title, int tn, char *body, int bn);

void camera_enter(void);
void camera_enter_status(void);   // foto para publicar como estado
void gallery_enter_status(void);
void draw_camera(void);
void input_camera(void);
void camera_ime_result(const char *text);
void gallery_enter(void);
void draw_gallery(void);
void input_gallery(void);
void gallery_ime_result(const char *text);

#endif
