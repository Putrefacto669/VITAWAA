#ifndef VITAWA_SYNC_H
#define VITAWA_SYNC_H

// Modelo de datos y comunicacion con el bridge (por el canal cifrado de chan.c).
//  - Un hilo hace long-poll a /v1/status y baja lo que cambio (chats, chat abierto, estados).
//  - Otro hilo ejecuta las acciones en orden (enviar, fotos, notas de voz, consultas...).
// La UI lee g_model con sync_lock()/sync_unlock() y copia lo que necesita.

#define MAX_CHATS    60
#define MAX_MSGS     80
#define MAX_LINES    40
#define MAX_STATUS   40
#define MAX_SITEMS   30
#define MAX_STICKERS 32
#define MAX_PARTS    64
#define MAX_CGROUPS  20
#define MAX_CONTACTS 150
#define QR_MAX       177

typedef struct {
    char url[160];
    unsigned char key[32];
    int  paired;
    int  accent;
    int  sounds;
    int  theme;           // 0 Aero claro · 1 Aero noche
    char phone[24];       // tu número (para vincular WhatsApp con código)
    int  fontsize;        // 0 normal · 1 grande
    int  sysnotif;        // 1 = avisos del sistema de la Vita
} Config;

int  config_load(Config *c);
void config_save(const Config *c);

enum { NET_IDLE, NET_OK, NET_UNREACHABLE, NET_BADKEY, NET_UNPAIRED };

typedef struct {
    char jid[64];
    char name[96];
    int  unread;
    char time[12];
    char last[192];
    int  ver;
    int  group;
    char lastkind[10];
    int  color;
    char typing[80];      // "escribiendo…", "Ana está escribiendo…" (vacío si nadie)
    int  archived, muted, pinned;
} Chat;

typedef struct {
    char id[48];
    int  mine;
    char sender[64];
    int  color;
    char time[8];
    char day[24];
    char kind[10];        // text image video gif audio voice sticker doc location contact poll deleted other
    char text[1024];
    char qname[64];
    char qtext[160];
    char reacts[96];
    int  ack;
    int  starred;
    int  fwd;
    char dur[8];
    char sender_jid[64];
    // cache de maquetado (solo la UI lo toca)
    int  lay_w;
    int  nlines;
    short off[MAX_LINES];
    short len[MAX_LINES];
    int  h, w;
    int  time_inline;
    int  disp;
    int  lay_fs;          // tamaño de letra con el que se maquetó (para re-maquetar al cambiarlo)
} Msg;

typedef struct {
    char jid[64];
    char name[96];
    char sub[128];
    char number[32];
    int  group;
    int  ver;
    int  n;
    Msg  msgs[MAX_MSGS];
} Conv;

typedef struct {
    char poster[64];
    char name[96];
    int  count, unseen;
    char ago[24];
    int  color;
} StatusRow;

typedef struct {
    char id[48];
    char kind[10];
    char text[512];
    unsigned int bg;      // color vita2d, 0 = sin color
    char ago[24];
    int  seen;
    char dur[8];
} StatusItem;

typedef struct {
    char jid[64];
    char id[48];
} StickerRef;

typedef struct {
    char jid[64];
    char name[96];
    int  admin;
    int  color;
} Participant;

typedef struct {
    char jid[64];
    char name[96];
    int  members;
} GroupRef;

typedef struct {
    char jid[64];
    int  loaded;          // 0 cargando, 1 listo, -1 error
    int  group;
    char name[96];
    char number[32];
    char about[400];
    char about_at[48];
    char saved[96];
    char push[96];
    char sub[128];
    int  members;
    int  nparts;
    Participant parts[MAX_PARTS];
    int  ngroups;
    GroupRef groups[MAX_CGROUPS];
} ContactInfo;

typedef struct {
    char jid[64];
    char name[96];
    char number[32];
    int  color;
    char push[96];
} ContactRow;

typedef struct {
    int  state;           // 0 nada, 1 buscando, 2 listo, 3 error
    char jid[64];
    char name[96];
    char number[32];
    char err[96];
} CheckResult;

#define MAX_NOTIFS 30
typedef struct {
    int  seq;
    char kind[8];         // msg | react | slike
    char jid[64];
    char title[128];
    char emoji[48];       // marcador \x1E<hex>\x1F
    char body[200];
    char time[12];
} Notif;

typedef struct {
    // conexion
    int  net, http_err;
    int  seq;
    char state[16];       // starting | qr | connecting | open | logged_out
    char me_name[64];
    char me_num[32];
    char me_jid[64];
    char version[16];
    int  unread;

    int  qr_size, qr_version, qr_left;
    unsigned int qr_at;
    unsigned char qr[QR_MAX * QR_MAX];

    int  chats_ver;
    int  nchats;
    Chat chats[MAX_CHATS];

    int  conv_ver;
    Conv conv;

    int  st_ver;
    int  nstatus;
    StatusRow status[MAX_STATUS];

    int  sitems_ver;
    char sitems_poster[64];
    int  nsitems;
    StatusItem sitems[MAX_SITEMS];

    int  stk_ver;
    int  stk_tab;
    int  nstk;
    StickerRef stk[MAX_STICKERS];

    int  contact_ver;
    ContactInfo contact;

    int  search_ver;
    int  nsearch;
    ContactRow search[MAX_CONTACTS];

    int  check_ver;
    CheckResult check;

    int  toast_ver;
    char toast[96];
    int  toast_err;

    int  incoming;        // sube cuando llega un mensaje nuevo (para el sonido)

    // notificaciones: historial (lo más nuevo primero) + cola de banners
    int  notif_ver;
    int  nnotifs;
    Notif notifs[MAX_NOTIFS];
    int  nbanner;
    Notif banner[6];
    int  notif_unseen;

    // vincular WhatsApp con número
    char paircode[16];

    // video que se está preparando / listo para reproducir
    int  video_ver;
    int  video_state;     // 0 nada · 1 bajando · 2 listo · 3 error
    char video_id[48];
    char video_path[128];

    // catálogo del panel de emojis
    int  emoji_ver;
    int  ncats;
    struct { char name[24]; int n; char (*names)[24]; } cats[10];
} Model;

extern Model g_model;

void sync_start(const Config *cfg);
void sync_set_config(const Config *cfg);
void sync_lock(void);
void sync_unlock(void);
unsigned int now_ms(void);

// Que mirar con prioridad
void sync_open_chat(const char *jid);       // "" para ninguno
void sync_open_status(const char *poster);
void sync_load_stickers(int tab);           // 0 recientes, 1 favoritos, 2 recibidos

// Acciones (van a una cola y se ejecutan en orden)
void job_send(const char *jid, const char *reply_id, const char *text);
void job_image(const char *jid, const char *reply_id, const char *caption, const char *fmt, int w, int h, int mirror, unsigned char *data, int len);  // toma data (free)
void job_voice(const char *jid, const char *reply_id, unsigned char *mulaw, int len);  // toma mulaw (free)
void job_audio(const char *jid, const char *id);   // baja y reproduce una nota de voz
void job_sticker(const char *jid, const char *src_jid, const char *id, const char *reply_id);
void job_sticker_fav(const char *jid, const char *id, int on);
void job_react(const char *jid, const char *id, const char *emoji_hex);
void job_forward(const char *jid, const char *id, const char *to_csv);
void job_delete(const char *jid, const char *id);
void job_star(const char *jid, const char *id, int on);
void job_read(const char *jid);
void job_status_seen(const char *poster, const char *id);
void job_status_reply(const char *poster, const char *id, const char *text);
void job_status_post(const char *text);
void job_contact(const char *jid);
void job_search(const char *query);
void job_check(const char *number);
void job_logout(void);
void job_wapair(const char *number);   // pide el código de 8 caracteres
void job_chat_action(const char *jid, const char *action);   // mute|unmute|archive|unarchive|pin|unpin
void job_edit(const char *jid, const char *id, const char *text);
void job_revoke(const char *jid, const char *id);
void job_video(const char *jid, const char *id);              // baja el MP4 a la memoria de la Vita
void job_emojis(void);

#endif
