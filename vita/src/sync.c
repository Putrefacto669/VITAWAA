#include "sync.h"
#include "chan.h"
#include "net.h"
#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>

#define APP_DIR     "ux0:data/VitaWA"
#define CONFIG_PATH APP_DIR "/config.txt"

Model g_model;

static SceKernelLwMutexWork mtx;
static Config cur;
static volatile int cfg_dirty = 1;
static volatile int poll_slot = -1;
static volatile int work_slot = -1;

static char want_chat[64];
static volatile int want_chat_dirty = 0;
static char want_poster[64];
static volatile int want_poster_dirty = 0;
static volatile int want_stk = -1;
static volatile int kick = 0;   // "vuelve a preguntar ya" (despues de una accion)

void sync_lock(void)   { sceKernelLockLwMutex(&mtx, 1, NULL); }
void sync_unlock(void) { sceKernelUnlockLwMutex(&mtx, 1); }

unsigned int now_ms(void) { return (unsigned int)(sceKernelGetProcessTimeWide() / 1000ULL); }

// ---------- config ----------
static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = '\0';
    char *b = s;
    while (*b == ' ') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

int config_load(Config *c) {
    memset(c, 0, sizeof(*c));
    c->sounds = 1;
    c->sysnotif = 1;
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcmp(k, "url")) snprintf(c->url, sizeof(c->url), "%s", v);
        else if (!strcmp(k, "key")) c->paired = b64url_decode(v, c->key, sizeof(c->key)) == 32;
        else if (!strcmp(k, "accent")) c->accent = atoi(v);
        else if (!strcmp(k, "sounds")) c->sounds = atoi(v);
        else if (!strcmp(k, "theme")) c->theme = atoi(v);
        else if (!strcmp(k, "phone")) snprintf(c->phone, sizeof(c->phone), "%s", v);
        else if (!strcmp(k, "fontsize")) c->fontsize = atoi(v);
        else if (!strcmp(k, "sysnotif")) c->sysnotif = atoi(v);
    }
    fclose(f);
    if (!c->url[0]) c->paired = 0;
    return c->paired;
}

void config_save(const Config *c) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(APP_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    char k[64] = "";
    if (c->paired) b64url_encode(c->key, 32, k, sizeof(k));
    fprintf(f, "url=%s\nkey=%s\naccent=%d\nsounds=%d\ntheme=%d\nphone=%s\nfontsize=%d\nsysnotif=%d\n", c->url, k, c->accent, c->sounds, c->theme, c->phone, c->fontsize, c->sysnotif);
    fclose(f);
}

static void set_toast(const char *msg, int err) {
    sync_lock();
    snprintf(g_model.toast, sizeof(g_model.toast), "%s", msg);
    g_model.toast_err = err;
    g_model.toast_ver++;
    sync_unlock();
}

// ---------- parseo ----------
static int split_tabs(char *line, char **f, int max) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == '\t') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

static char *next_line(char **cur_p) {
    char *s = *cur_p;
    if (!s || !*s) return NULL;
    char *nl = strchr(s, '\n');
    if (nl) { *nl = '\0'; *cur_p = nl + 1; }
    else *cur_p = s + strlen(s);
    return s;
}

// Copia recortando sin partir un caracter UTF-8 ni un marcador de emoji.
// multiline: "\n" literal -> salto real; si no, -> espacio.
static void copyf(char *dst, int cap, const char *src, int multiline) {
    int n = 0;
    for (const char *p = src; *p && n < cap - 1; p++) {
        if (p[0] == '\\' && p[1] == 'n') {
            dst[n++] = multiline ? '\n' : ' ';
            p++;
        } else {
            dst[n++] = *p;
        }
    }
    dst[n] = '\0';
    if (n >= cap - 1) {
        while (n > 0 && ((unsigned char)dst[n] & 0xC0) == 0x80) n--;
        dst[n] = '\0';
        char *open = strrchr(dst, '\x1E');
        if (open && !strchr(open, '\x1F')) *open = '\0';
    }
}
#define FIELD(dst, i, ml) copyf((dst), sizeof(dst), nf > (i) ? f[i] : "", (ml))

static unsigned int parse_hex(const char *s) {
    if (!s || strlen(s) != 6) return 0;
    unsigned int v = (unsigned int)strtoul(s, NULL, 16);
    return 0xFF000000u | ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
}

// ---------- descargas del hilo de sondeo ----------
static int get(const char *path, char **out, unsigned int timeout_s) {
    int len;
    return chan_call("GET", path, NULL, 0, out, &len, 512 * 1024, timeout_s, &poll_slot);
}

static void fetch_qr(void) {
    char *buf = NULL;
    if (get("/v1/qr", &buf, 15) == 200) {
        static unsigned char qr_local[QR_MAX * QR_MAX];
        char *cur_p = buf, *f[4];
        char *line = next_line(&cur_p);
        int nf = line ? split_tabs(line, f, 4) : 0;
        int size = nf > 0 ? atoi(f[0]) : 0;
        int left = nf > 2 ? atoi(f[2]) : 20;
        if (size > 0 && size <= QR_MAX) {
            int y = 0;
            while (y < size && (line = next_line(&cur_p))) {
                for (int x = 0; x < size && line[x]; x++) qr_local[y * size + x] = line[x] == '1';
                y++;
            }
            if (y == size) {
                sync_lock();
                g_model.qr_size = size;
                g_model.qr_left = left;
                g_model.qr_at = now_ms();
                memcpy(g_model.qr, qr_local, (size_t)size * size);
                g_model.qr_version++;
                sync_unlock();
            }
        }
    }
    free(buf);
}

static int fetch_chats(void) {
    char *buf = NULL;
    int incoming = 0;
    if (get("/v1/chats?limit=60", &buf, 15) == 200) {
        static Chat local[MAX_CHATS];
        int n = 0;
        char *cur_p = buf, *line, *f[14];
        while (n < MAX_CHATS && (line = next_line(&cur_p))) {
            int nf = split_tabs(line, f, 14);
            if (nf < 7) continue;
            Chat *c = &local[n++];
            memset(c, 0, sizeof(*c));
            FIELD(c->jid, 0, 0);
            FIELD(c->name, 1, 0);
            c->unread = atoi(f[2]);
            FIELD(c->time, 3, 0);
            FIELD(c->last, 4, 0);
            c->ver = atoi(f[5]);
            c->group = atoi(f[6]);
            FIELD(c->lastkind, 7, 0);
            c->color = nf > 8 ? atoi(f[8]) : 0;
            FIELD(c->typing, 9, 0);
            c->archived = nf > 10 ? atoi(f[10]) : 0;
            c->muted = nf > 11 ? atoi(f[11]) : 0;
            c->pinned = nf > 12 ? atoi(f[12]) : 0;
        }
        sync_lock();
        int before = 0, after = 0;
        for (int i = 0; i < g_model.nchats; i++) before += g_model.chats[i].unread;
        for (int i = 0; i < n; i++) after += local[i].unread;
        if (g_model.chats_ver > 0 && after > before) incoming = 1;
        memcpy(g_model.chats, local, sizeof(Chat) * n);
        g_model.nchats = n;
        g_model.chats_ver++;
        if (incoming) g_model.incoming++;
        sync_unlock();
    }
    free(buf);
    return incoming;
}

static int loaded_chat_ver = -1;
static char loaded_chat[64];

static void fetch_chat(const char *jid) {
    char path[200], enc[128];
    url_enc(jid, enc, sizeof(enc));
    snprintf(path, sizeof(path), "/v1/chat?jid=%s&limit=%d", enc, MAX_MSGS);
    char *buf = NULL;
    if (get(path, &buf, 15) == 200) {
        static Conv local;
        memset(&local, 0, sizeof(local));
        snprintf(local.jid, sizeof(local.jid), "%s", jid);
        char *cur_p = buf, *line, *f[18];
        int nf;
        if ((line = next_line(&cur_p)) && (nf = split_tabs(line, f, 18)) >= 4) {
            FIELD(local.name, 0, 0);
            FIELD(local.sub, 1, 0);
            local.group = atoi(f[2]);
            local.ver = atoi(f[3]);
            FIELD(local.number, 4, 0);
        }
        while ((line = next_line(&cur_p))) {
            nf = split_tabs(line, f, 18);
            if (nf < 15) continue;
            if (local.n == MAX_MSGS) {
                memmove(&local.msgs[0], &local.msgs[1], sizeof(Msg) * (MAX_MSGS - 1));
                local.n--;
            }
            Msg *m = &local.msgs[local.n++];
            memset(m, 0, sizeof(*m));
            FIELD(m->id, 0, 0);
            m->mine = atoi(f[1]);
            FIELD(m->sender, 2, 0);
            m->color = atoi(f[3]);
            FIELD(m->time, 4, 0);
            FIELD(m->day, 5, 0);
            FIELD(m->kind, 6, 0);
            FIELD(m->text, 7, 1);
            FIELD(m->qname, 8, 0);
            FIELD(m->qtext, 9, 0);
            FIELD(m->reacts, 10, 0);
            m->ack = atoi(f[11]);
            m->starred = atoi(f[12]);
            m->fwd = atoi(f[13]);
            FIELD(m->dur, 14, 0);
            FIELD(m->sender_jid, 15, 0);
        }
        sync_lock();
        if (!strcmp(want_chat, jid)) {
            memcpy(&g_model.conv, &local, sizeof(Conv));
            g_model.conv_ver++;
            loaded_chat_ver = local.ver;
            snprintf(loaded_chat, sizeof(loaded_chat), "%s", jid);
        }
        sync_unlock();
    }
    free(buf);
}

static void fetch_statuses(void) {
    char *buf = NULL;
    if (get("/v1/statuses", &buf, 15) == 200) {
        static StatusRow local[MAX_STATUS];
        int n = 0;
        char *cur_p = buf, *line, *f[7];
        while (n < MAX_STATUS && (line = next_line(&cur_p))) {
            int nf = split_tabs(line, f, 7);
            if (nf < 5) continue;
            StatusRow *s = &local[n++];
            FIELD(s->poster, 0, 0);
            FIELD(s->name, 1, 0);
            s->count = atoi(f[2]);
            s->unseen = atoi(f[3]);
            FIELD(s->ago, 4, 0);
            s->color = nf > 5 ? atoi(f[5]) : 0;
        }
        sync_lock();
        memcpy(g_model.status, local, sizeof(StatusRow) * n);
        g_model.nstatus = n;
        g_model.st_ver++;
        sync_unlock();
    }
    free(buf);
}

static void fetch_sitems(const char *poster) {
    char path[200], enc[128];
    url_enc(poster, enc, sizeof(enc));
    snprintf(path, sizeof(path), "/v1/status_items?poster=%s", enc);
    char *buf = NULL;
    if (get(path, &buf, 15) == 200) {
        static StatusItem local[MAX_SITEMS];
        int n = 0;
        char *cur_p = buf, *line, *f[8];
        while (n < MAX_SITEMS && (line = next_line(&cur_p))) {
            int nf = split_tabs(line, f, 8);
            if (nf < 7) continue;
            StatusItem *s = &local[n++];
            FIELD(s->id, 0, 0);
            FIELD(s->kind, 1, 0);
            FIELD(s->text, 2, 1);
            s->bg = parse_hex(f[3]);
            FIELD(s->ago, 4, 0);
            s->seen = atoi(f[5]);
            FIELD(s->dur, 6, 0);
        }
        sync_lock();
        if (!strcmp(want_poster, poster)) {
            memcpy(g_model.sitems, local, sizeof(StatusItem) * n);
            g_model.nsitems = n;
            snprintf(g_model.sitems_poster, sizeof(g_model.sitems_poster), "%s", poster);
            g_model.sitems_ver++;
        }
        sync_unlock();
    }
    free(buf);
}

static void fetch_stickers(int tab) {
    static const char *tabs[] = {"recent", "fav", "received"};
    char path[64];
    snprintf(path, sizeof(path), "/v1/stickers?tab=%s", tabs[tab % 3]);
    char *buf = NULL;
    if (get(path, &buf, 15) == 200) {
        static StickerRef local[MAX_STICKERS];
        int n = 0;
        char *cur_p = buf, *line, *f[3];
        while (n < MAX_STICKERS && (line = next_line(&cur_p))) {
            int nf = split_tabs(line, f, 3);
            if (nf < 2) continue;
            FIELD(local[n].jid, 0, 0);
            FIELD(local[n].id, 1, 0);
            n++;
        }
        sync_lock();
        memcpy(g_model.stk, local, sizeof(StickerRef) * n);
        g_model.nstk = n;
        g_model.stk_tab = tab;
        g_model.stk_ver++;
        sync_unlock();
    }
    free(buf);
}

// Notificaciones nuevas. La primera vez (al conectar) solo llena el historial:
// no tiene caso tirar 20 banners de cosas viejas.
static void fetch_notifs(int since, int banners) {
    char path[64];
    snprintf(path, sizeof(path), "/v1/notifs?since=%d", since);
    char *buf = NULL;
    if (get(path, &buf, 15) == 200) {
        char *cur_p = buf, *line, *f[8];
        sync_lock();
        while ((line = next_line(&cur_p))) {
            int nf = split_tabs(line, f, 8);
            if (nf < 7) continue;
            Notif n;
            memset(&n, 0, sizeof(n));
            n.seq = atoi(f[0]);
            FIELD(n.kind, 1, 0);
            FIELD(n.jid, 2, 0);
            FIELD(n.title, 3, 0);
            FIELD(n.emoji, 4, 0);
            FIELD(n.body, 5, 0);
            FIELD(n.time, 6, 0);
            if (g_model.nnotifs == MAX_NOTIFS) g_model.nnotifs--;
            memmove(&g_model.notifs[1], &g_model.notifs[0], sizeof(Notif) * g_model.nnotifs);
            g_model.notifs[0] = n;
            g_model.nnotifs++;
            if (banners) {
                // no avisar de un mensaje del chat que ya tienes abierto
                int watching = !strcmp(n.kind, "msg") && !strcmp(n.jid, want_chat);
                if (!watching) {
                    g_model.notif_unseen++;
                    if (g_model.nbanner < 6) g_model.banner[g_model.nbanner++] = n;
                }
            }
        }
        g_model.notif_ver++;
        sync_unlock();
    }
    free(buf);
}

static int poll_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int known_seq = 0, known_status_ver = -1, known_notif = -1;
    int have_cfg = 0;
    char *buf = NULL;

    for (;;) {
        if (cfg_dirty) {
            sync_lock();
            cfg_dirty = 0;
            have_cfg = cur.paired && cur.url[0];
            chan_set(cur.url, have_cfg ? cur.key : NULL);
            int keep_toast = g_model.toast_ver;
            memset(&g_model, 0, sizeof(g_model));
            g_model.toast_ver = keep_toast;
            g_model.net = have_cfg ? NET_IDLE : NET_UNPAIRED;
            loaded_chat_ver = -1;
            loaded_chat[0] = '\0';
            sync_unlock();
            known_seq = 0;
            known_status_ver = -1;
            known_notif = -1;
        }
        if (!have_cfg) {
            sceKernelDelayThread(200 * 1000);
            continue;
        }

        // lo que la UI acaba de pedir va primero
        if (want_chat_dirty) {
            char jid[64];
            sync_lock();
            want_chat_dirty = 0;
            snprintf(jid, sizeof(jid), "%s", want_chat);
            sync_unlock();
            if (jid[0] && !strcmp(g_model.state, "open")) fetch_chat(jid);
            continue;
        }
        if (want_poster_dirty) {
            char poster[64];
            sync_lock();
            want_poster_dirty = 0;
            snprintf(poster, sizeof(poster), "%s", want_poster);
            sync_unlock();
            if (poster[0]) fetch_sitems(poster);
            continue;
        }
        if (want_stk >= 0) {
            int tab = want_stk;
            want_stk = -1;
            fetch_stickers(tab);
            continue;
        }

        char path[96];
        snprintf(path, sizeof(path), "/v1/status?since=%d&wait=20", known_seq);
        free(buf);
        buf = NULL;
        int code = get(path, &buf, 28);
        if (kick) { kick = 0; continue; }
        if (cfg_dirty || want_chat_dirty || want_poster_dirty || want_stk >= 0) continue;   // nos cortaron a proposito

        if (code != 200) {
            sync_lock();
            g_model.net = code == CHAN_ERR_KEY ? NET_BADKEY : NET_UNREACHABLE;
            g_model.http_err = code;
            sync_unlock();
            known_seq = 0;
            sceKernelDelayThread((code == CHAN_ERR_KEY ? 4000 : 2500) * 1000);
            continue;
        }

        char *cur_p = buf, *f[12];
        char *line = next_line(&cur_p);
        int nf = line ? split_tabs(line, f, 12) : 0;
        if (nf < 6) {
            sceKernelDelayThread(1000 * 1000);
            continue;
        }
        int seq = atoi(f[0]);
        int status_ver = atoi(f[5]);
        char state[16];
        snprintf(state, sizeof(state), "%s", f[1]);

        sync_lock();
        g_model.net = NET_OK;
        g_model.http_err = 0;
        g_model.seq = seq;
        snprintf(g_model.state, sizeof(g_model.state), "%s", state);
        FIELD(g_model.me_name, 2, 0);
        FIELD(g_model.me_num, 3, 0);
        g_model.unread = atoi(f[4]);
        FIELD(g_model.version, 6, 0);
        FIELD(g_model.me_jid, 7, 0);
        int notif_seq = nf > 8 ? atoi(f[8]) : 0;
        FIELD(g_model.paircode, 9, 0);
        sync_unlock();

        if (notif_seq != known_notif && !strcmp(state, "open")) {
            int first = known_notif < 0 || notif_seq < known_notif;   // recién conectados o el bridge reinició
            fetch_notifs(first ? 0 : known_notif, !first);
            known_notif = notif_seq;
        }

        if (seq == known_seq) continue;   // timeout del long-poll sin cambios
        known_seq = seq;

        if (!strcmp(state, "qr")) {
            fetch_qr();
        } else if (!strcmp(state, "open")) {
            fetch_chats();

            char jid[64];
            int ver = -1;
            sync_lock();
            snprintf(jid, sizeof(jid), "%s", want_chat);
            for (int i = 0; i < g_model.nchats; i++)
                if (!strcmp(g_model.chats[i].jid, jid)) ver = g_model.chats[i].ver;
            int stale = jid[0] && (strcmp(loaded_chat, jid) || ver != loaded_chat_ver || ver < 0);
            sync_unlock();
            if (stale) fetch_chat(jid);

            if (status_ver != known_status_ver) {
                known_status_ver = status_ver;
                fetch_statuses();
                char poster[64];
                sync_lock();
                snprintf(poster, sizeof(poster), "%s", want_poster);
                sync_unlock();
                if (poster[0]) fetch_sitems(poster);
            }
        }
    }
    return 0;
}

// ---------- acciones ----------
enum {
    J_SEND, J_IMAGE, J_VOICE, J_AUDIO, J_STICKER, J_STICKER_FAV, J_REACT, J_FORWARD, J_DELETE, J_STAR, J_READ,
    J_STATUS_SEEN, J_STATUS_REPLY, J_STATUS_POST, J_CONTACT, J_SEARCH, J_CHECK, J_LOGOUT, J_WAPAIR, J_CHAT_ACTION, J_EDIT, J_REVOKE, J_VIDEO, J_EMOJIS
};

typedef struct {
    int type;
    char a[64], b[64], c[300], d[400];
    unsigned char *body;
    int body_len;
} Job;

#define JOBS 32
static Job jobs[JOBS];
static int job_head = 0, job_tail = 0;
static SceUID job_sema = -1;

static void push(Job *j) {
    sync_lock();
    int next = (job_tail + 1) % JOBS;
    if (next == job_head) {
        sync_unlock();
        free(j->body);
        set_toast("Demasiadas acciones seguidas, espera un momento", 1);
        return;
    }
    jobs[job_tail] = *j;
    job_tail = next;
    sync_unlock();
    sceKernelSignalSema(job_sema, 1);
}

static Job mk(int type, const char *a, const char *b, const char *c, const char *d) {
    Job j;
    memset(&j, 0, sizeof(j));
    j.type = type;
    snprintf(j.a, sizeof(j.a), "%s", a ? a : "");
    snprintf(j.b, sizeof(j.b), "%s", b ? b : "");
    snprintf(j.c, sizeof(j.c), "%s", c ? c : "");
    snprintf(j.d, sizeof(j.d), "%s", d ? d : "");
    return j;
}
static void text_body(Job *j, const char *text) {
    if (!text) return;
    j->body_len = (int)strlen(text);
    j->body = malloc(j->body_len + 1);
    if (j->body) memcpy(j->body, text, j->body_len + 1);
    else j->body_len = 0;
}

void job_send(const char *jid, const char *reply_id, const char *text) { Job j = mk(J_SEND, jid, reply_id, NULL, NULL); text_body(&j, text); push(&j); }
void job_image(const char *jid, const char *reply_id, const char *caption, const char *fmt, int w, int h, int mirror, unsigned char *data, int len) {
    char d[64];
    snprintf(d, sizeof(d), "%s&w=%d&h=%d&mirror=%d", fmt, w, h, mirror);
    Job j = mk(J_IMAGE, jid, reply_id, caption, d);
    j.body = data;
    j.body_len = len;
    push(&j);
}
void job_voice(const char *jid, const char *reply_id, unsigned char *mulaw, int len) { Job j = mk(J_VOICE, jid, reply_id, NULL, NULL); j.body = mulaw; j.body_len = len; push(&j); }
void job_audio(const char *jid, const char *id) { audio_set_loading(id); Job j = mk(J_AUDIO, jid, id, NULL, NULL); push(&j); }
void job_sticker(const char *jid, const char *src_jid, const char *id, const char *reply_id) { Job j = mk(J_STICKER, jid, src_jid, id, reply_id); push(&j); }
void job_sticker_fav(const char *jid, const char *id, int on) { Job j = mk(J_STICKER_FAV, jid, id, on ? "1" : "0", NULL); push(&j); }
void job_react(const char *jid, const char *id, const char *emoji_hex) { Job j = mk(J_REACT, jid, id, emoji_hex, NULL); push(&j); }
void job_forward(const char *jid, const char *id, const char *to_csv) { Job j = mk(J_FORWARD, jid, id, NULL, to_csv); push(&j); }
void job_delete(const char *jid, const char *id) { Job j = mk(J_DELETE, jid, id, NULL, NULL); push(&j); }
void job_star(const char *jid, const char *id, int on) { Job j = mk(J_STAR, jid, id, on ? "1" : "0", NULL); push(&j); }
void job_read(const char *jid) { Job j = mk(J_READ, jid, NULL, NULL, NULL); push(&j); }
void job_status_seen(const char *poster, const char *id) { Job j = mk(J_STATUS_SEEN, poster, id, NULL, NULL); push(&j); }
void job_status_reply(const char *poster, const char *id, const char *text) { Job j = mk(J_STATUS_REPLY, poster, id, NULL, NULL); text_body(&j, text); push(&j); }
void job_status_post(const char *text) { Job j = mk(J_STATUS_POST, NULL, NULL, NULL, NULL); text_body(&j, text); push(&j); }
void job_contact(const char *jid) {
    sync_lock();
    memset(&g_model.contact, 0, sizeof(g_model.contact));
    snprintf(g_model.contact.jid, sizeof(g_model.contact.jid), "%s", jid);
    g_model.contact_ver++;
    sync_unlock();
    Job j = mk(J_CONTACT, jid, NULL, NULL, NULL);
    push(&j);
}
void job_search(const char *query) { Job j = mk(J_SEARCH, NULL, NULL, query, NULL); push(&j); }
void job_check(const char *number) {
    sync_lock();
    memset(&g_model.check, 0, sizeof(g_model.check));
    g_model.check.state = 1;
    g_model.check_ver++;
    sync_unlock();
    Job j = mk(J_CHECK, NULL, NULL, number, NULL);
    push(&j);
}
void job_logout(void) { Job j = mk(J_LOGOUT, NULL, NULL, NULL, NULL); push(&j); }
void job_wapair(const char *number) { Job j = mk(J_WAPAIR, NULL, NULL, number, NULL); push(&j); }
void job_chat_action(const char *jid, const char *action) { Job j = mk(J_CHAT_ACTION, jid, NULL, action, NULL); push(&j); }
void job_edit(const char *jid, const char *id, const char *text) { Job j = mk(J_EDIT, jid, id, NULL, NULL); text_body(&j, text); push(&j); }
void job_revoke(const char *jid, const char *id) { Job j = mk(J_REVOKE, jid, id, NULL, NULL); push(&j); }
void job_video(const char *jid, const char *id) {
    sync_lock();
    snprintf(g_model.video_id, sizeof(g_model.video_id), "%s", id);
    g_model.video_state = 1;
    g_model.video_ver++;
    sync_unlock();
    Job j = mk(J_VIDEO, jid, id, NULL, NULL);
    push(&j);
}
void job_emojis(void) { Job j = mk(J_EMOJIS, NULL, NULL, NULL, NULL); push(&j); }

static void parse_emojis(char *buf) {
    char *cur_p = buf, *line, *f[2];
    sync_lock();
    if (g_model.ncats) { sync_unlock(); return; }   // la vista guarda los punteros: se carga una sola vez
    while (g_model.ncats < 10 && (line = next_line(&cur_p))) {
        if (split_tabs(line, f, 2) < 2) continue;
        int n = 1;
        for (char *p = f[1]; *p; p++) if (*p == ',') n++;
        typeof(g_model.cats[0]) *c = &g_model.cats[g_model.ncats];
        snprintf(c->name, sizeof(c->name), "%s", f[0]);
        c->names = malloc(sizeof(*c->names) * n);
        c->n = 0;
        if (!c->names) continue;
        char *save = NULL;
        for (char *t = strtok_r(f[1], ",", &save); t; t = strtok_r(NULL, ",", &save))
            snprintf(c->names[c->n++], sizeof(c->names[0]), "%s", t);
        g_model.ncats++;
    }
    g_model.emoji_ver++;
    sync_unlock();
}

static void parse_contact(char *buf, const char *jid) {
    static ContactInfo ci;
    memset(&ci, 0, sizeof(ci));
    snprintf(ci.jid, sizeof(ci.jid), "%s", jid);
    ci.loaded = 1;
    char *cur_p = buf, *line, *f[10];
    while ((line = next_line(&cur_p))) {
        int nf = split_tabs(line, f, 10);
        if (nf < 2) continue;
        if (!strcmp(f[0], "I")) {
            FIELD(ci.name, 1, 0);
            FIELD(ci.number, 2, 0);
            FIELD(ci.about, 3, 1);
            FIELD(ci.about_at, 4, 0);
            FIELD(ci.saved, 5, 0);
            FIELD(ci.push, 6, 0);
            FIELD(ci.sub, 7, 0);
            ci.members = nf > 8 ? atoi(f[8]) : 0;
            ci.group = strstr(jid, "@g.us") != NULL;
        } else if (!strcmp(f[0], "P") && ci.nparts < MAX_PARTS && nf >= 5) {
            Participant *p = &ci.parts[ci.nparts++];
            FIELD(p->jid, 1, 0);
            FIELD(p->name, 2, 0);
            p->admin = atoi(f[3]);
            p->color = atoi(f[4]);
        } else if (!strcmp(f[0], "G") && ci.ngroups < MAX_CGROUPS && nf >= 4) {
            GroupRef *g = &ci.groups[ci.ngroups++];
            FIELD(g->jid, 1, 0);
            FIELD(g->name, 2, 0);
            g->members = atoi(f[3]);
        }
    }
    sync_lock();
    if (!strcmp(g_model.contact.jid, jid)) {
        g_model.contact = ci;
        g_model.contact_ver++;
    }
    sync_unlock();
}

static void parse_search(char *buf) {
    static ContactRow local[MAX_CONTACTS];
    int n = 0;
    char *cur_p = buf, *line, *f[6];
    while (n < MAX_CONTACTS && (line = next_line(&cur_p))) {
        int nf = split_tabs(line, f, 6);
        if (nf < 3) continue;
        ContactRow *r = &local[n++];
        FIELD(r->jid, 0, 0);
        FIELD(r->name, 1, 0);
        FIELD(r->number, 2, 0);
        r->color = nf > 3 ? atoi(f[3]) : 0;
        FIELD(r->push, 4, 0);
    }
    sync_lock();
    memcpy(g_model.search, local, sizeof(ContactRow) * n);
    g_model.nsearch = n;
    g_model.search_ver++;
    sync_unlock();
}

static int work_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    char path[1100];
    char ea[128], eb[128], ec[600], ed[800];
    for (;;) {
        sceKernelWaitSema(job_sema, 1, NULL);
        Job j;
        sync_lock();
        if (job_head == job_tail) { sync_unlock(); continue; }
        j = jobs[job_head];
        job_head = (job_head + 1) % JOBS;
        sync_unlock();

        url_enc(j.a, ea, sizeof(ea));
        url_enc(j.b, eb, sizeof(eb));
        url_enc(j.c, ec, sizeof(ec));
        url_enc(j.d, ed, sizeof(ed));

        const char *method = "POST";
        const char *ok = NULL, *fail = "No se pudo completar la acción";
        int sfx_ok = -1;
        unsigned int timeout = 30;
        switch (j.type) {
        case J_SEND:         snprintf(path, sizeof(path), "/v1/send?jid=%s&reply=%s", ea, eb); fail = "No se pudo enviar el mensaje"; sfx_ok = SFX_SEND; break;
        case J_IMAGE:        snprintf(path, sizeof(path), "/v1/image?jid=%s&reply=%s&caption=%s&fmt=%s", ea, eb, ec, j.d); ok = "Foto enviada"; fail = "No se pudo enviar la foto"; sfx_ok = SFX_SEND; timeout = 90; break;
        case J_VOICE:        snprintf(path, sizeof(path), "/v1/voice?jid=%s&reply=%s", ea, eb); fail = "No se pudo enviar la nota de voz"; sfx_ok = SFX_SEND; timeout = 90; break;
        case J_AUDIO:        snprintf(path, sizeof(path), "/v1/audio?jid=%s&id=%s", ea, eb); method = "GET"; fail = "No se pudo bajar la nota de voz"; timeout = 60; break;
        case J_STICKER:      snprintf(path, sizeof(path), "/v1/sticker?jid=%s&src=%s&id=%s&reply=%s", ea, eb, ec, ed); fail = "No se pudo enviar el sticker"; sfx_ok = SFX_SEND; break;
        case J_STICKER_FAV:  snprintf(path, sizeof(path), "/v1/sticker_fav?jid=%s&id=%s&on=%s", ea, eb, ec); ok = j.c[0] == '1' ? "Agregado a favoritos" : "Quitado de favoritos"; break;
        case J_REACT:        snprintf(path, sizeof(path), "/v1/react?jid=%s&id=%s&e=%s", ea, eb, ec); fail = "No se pudo reaccionar"; break;
        case J_FORWARD:      snprintf(path, sizeof(path), "/v1/forward?jid=%s&id=%s&to=%s", ea, eb, ed); ok = "Mensaje reenviado"; fail = "No se pudo reenviar"; sfx_ok = SFX_SEND; break;
        case J_DELETE:       snprintf(path, sizeof(path), "/v1/delete?jid=%s&id=%s", ea, eb); ok = "Eliminado para ti"; break;
        case J_STAR:         snprintf(path, sizeof(path), "/v1/star?jid=%s&id=%s&on=%s", ea, eb, ec); ok = j.c[0] == '1' ? "Mensaje destacado" : "Ya no está destacado"; break;
        case J_READ:         snprintf(path, sizeof(path), "/v1/read?jid=%s", ea); fail = NULL; break;
        case J_STATUS_SEEN:  snprintf(path, sizeof(path), "/v1/status_seen?poster=%s&id=%s", ea, eb); fail = NULL; break;
        case J_STATUS_REPLY: snprintf(path, sizeof(path), "/v1/status_reply?poster=%s&id=%s", ea, eb); ok = "Respuesta enviada"; fail = "No se pudo responder"; sfx_ok = SFX_SEND; break;
        case J_STATUS_POST:  snprintf(path, sizeof(path), "/v1/status_post"); ok = "Estado publicado"; fail = "No se pudo publicar el estado"; break;
        case J_CONTACT:      snprintf(path, sizeof(path), "/v1/contact?jid=%s", ea); method = "GET"; fail = NULL; break;
        case J_SEARCH:       snprintf(path, sizeof(path), "/v1/contacts?q=%s", ec); method = "GET"; fail = NULL; break;
        case J_CHECK:        snprintf(path, sizeof(path), "/v1/check?number=%s", ec); method = "GET"; fail = NULL; break;
        case J_LOGOUT:       snprintf(path, sizeof(path), "/v1/logout"); ok = "Sesión cerrada"; break;
        case J_WAPAIR:       snprintf(path, sizeof(path), "/v1/wapair?n=%s", ec); fail = NULL; timeout = 40; break;
        case J_CHAT_ACTION:  snprintf(path, sizeof(path), "/v1/chat_action?jid=%s&a=%s", ea, ec);
            ok = !strcmp(j.c, "mute") ? "Silenciado 8 horas" : !strcmp(j.c, "unmute") ? "Notificaciones activadas" : !strcmp(j.c, "archive") ? "Chat archivado"
               : !strcmp(j.c, "unarchive") ? "Chat desarchivado" : !strcmp(j.c, "pin") ? "Chat fijado arriba" : "Chat desfijado";
            break;
        case J_EDIT:         snprintf(path, sizeof(path), "/v1/edit?jid=%s&id=%s", ea, eb); ok = "Mensaje editado"; fail = NULL; break;
        case J_REVOKE:       snprintf(path, sizeof(path), "/v1/revoke?jid=%s&id=%s", ea, eb); ok = "Eliminado para todos"; fail = NULL; break;
        case J_VIDEO:        snprintf(path, sizeof(path), "/v1/video?jid=%s&id=%s", ea, eb); method = "GET"; fail = NULL; timeout = 180; break;
        case J_EMOJIS:       snprintf(path, sizeof(path), "/v1/emojis"); method = "GET"; fail = NULL; break;
        default: free(j.body); continue;
        }

        char *resp = NULL;
        int resp_len = 0;
        int code = chan_call(method, path, j.body, j.body_len, &resp, &resp_len, j.type == J_VIDEO ? 16 * 1024 * 1024 : 8 * 1024 * 1024, timeout, &work_slot);
        free(j.body);

        if (j.type == J_AUDIO) {
            if (code == 200 && resp_len > 0) audio_play_mulaw(j.b, (unsigned char *)resp, resp_len);
            else audio_set_loading("");
        } else if (j.type == J_CONTACT) {
            if (code == 200) parse_contact(resp, j.a);
            else {
                sync_lock();
                g_model.contact.loaded = -1;
                g_model.contact_ver++;
                sync_unlock();
            }
        } else if (j.type == J_SEARCH) {
            if (code == 200) parse_search(resp);
        } else if (j.type == J_CHECK) {
            sync_lock();
            CheckResult *c = &g_model.check;
            memset(c, 0, sizeof(*c));
            if (code == 200 && resp) {
                char *f[4];
                char *cur_p = resp;
                char *line = next_line(&cur_p);
                int nf = line ? split_tabs(line, f, 4) : 0;
                if (nf >= 1) {
                    c->state = 2;
                    FIELD(c->jid, 0, 0);
                    FIELD(c->name, 1, 0);
                    FIELD(c->number, 2, 0);
                } else c->state = 3;
            } else {
                c->state = 3;
                snprintf(c->err, sizeof(c->err), "%s", code == 404 ? "Ese número no tiene WhatsApp" : code == 400 ? "Revisa el número" : "No se pudo revisar el número");
            }
            g_model.check_ver++;
            sync_unlock();
        }

        if (j.type == J_VIDEO) {
            // se guarda en la memoria de la Vita y AvPlayer lo lee de ahí
            char file[128];
            snprintf(file, sizeof(file), APP_DIR "/video.mp4");
            int okv = 0;
            if (code == 200 && resp_len > 1024) {
                sceIoMkdir(APP_DIR, 0777);
                FILE *vf = fopen(file, "wb");
                if (vf) { okv = fwrite(resp, 1, resp_len, vf) == (size_t)resp_len; fclose(vf); }
            }
            sync_lock();
            if (!strcmp(g_model.video_id, j.b)) {
                g_model.video_state = okv ? 2 : 3;
                snprintf(g_model.video_path, sizeof(g_model.video_path), "%s", file);
                g_model.video_ver++;
            }
            sync_unlock();
        } else if (j.type == J_EMOJIS) {
            if (code == 200 && resp) parse_emojis(resp);
        } else if ((j.type == J_EDIT || j.type == J_REVOKE) && code != 200) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s", (resp && resp[0] && code > 0) ? resp : "No se pudo completar");
            char *nl = strchr(msg, '\n'); if (nl) *nl = 0;
            set_toast(msg, 1);
        }

        if (j.type == J_WAPAIR) {
            if (code == 200 && resp) {
                char *f[2], *cur_p = resp, *line = next_line(&cur_p);
                int nf = line ? split_tabs(line, f, 2) : 0;
                sync_lock();
                if (nf >= 1) FIELD(g_model.paircode, 0, 0);
                sync_unlock();
            } else {
                char msg[96];
                snprintf(msg, sizeof(msg), "%s", (resp && resp[0] && code > 0) ? resp : "No se pudo pedir el código");
                char *nl = strchr(msg, '\n'); if (nl) *nl = 0;
                set_toast(msg, 1);
            }
        }

        if (code == 200) {
            if (ok) set_toast(ok, 0);
            if (sfx_ok >= 0) audio_sfx(sfx_ok);
        } else if (fail) {
            set_toast(code == CHAN_ERR_NET ? "Sin conexión con el bridge" : fail, 1);
            audio_sfx(SFX_ERROR);
        }
        free(resp);
        // que el hilo de sondeo no espere 20 s para ver el resultado
        if (j.type != J_CONTACT && j.type != J_SEARCH && j.type != J_AUDIO && j.type != J_VIDEO && j.type != J_EMOJIS) {
            kick = 1;
            net_abort(&poll_slot);
        }
    }
    return 0;
}

// ---------- arranque ----------
void sync_start(const Config *cfg) {
    sceKernelCreateLwMutex(&mtx, "vitawa_sync", 0, 0, NULL);
    memset(&g_model, 0, sizeof(g_model));
    cur = *cfg;
    cfg_dirty = 1;
    job_sema = sceKernelCreateSema("vitawa_jobs", 0, 0, JOBS, NULL);

    SceUID th = sceKernelCreateThread("vitawa_poll", poll_thread, 0x10000100, 0x40000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    th = sceKernelCreateThread("vitawa_work", work_thread, 0x10000100, 0x40000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

void sync_set_config(const Config *cfg) {
    sync_lock();
    cur = *cfg;
    cfg_dirty = 1;
    sync_unlock();
    net_abort(&poll_slot);
}

void sync_open_chat(const char *jid) {
    sync_lock();
    int changed = strcmp(want_chat, jid) != 0;
    snprintf(want_chat, sizeof(want_chat), "%s", jid);
    if (changed) {
        memset(&g_model.conv, 0, sizeof(g_model.conv));
        snprintf(g_model.conv.jid, sizeof(g_model.conv.jid), "%s", jid);
        g_model.conv_ver++;
        loaded_chat[0] = '\0';
        want_chat_dirty = 1;
    }
    sync_unlock();
    if (changed) net_abort(&poll_slot);
}

void sync_open_status(const char *poster) {
    sync_lock();
    int changed = strcmp(want_poster, poster) != 0;
    snprintf(want_poster, sizeof(want_poster), "%s", poster);
    if (changed) {
        g_model.nsitems = 0;
        snprintf(g_model.sitems_poster, sizeof(g_model.sitems_poster), "%s", poster);
        g_model.sitems_ver++;
        want_poster_dirty = 1;
    }
    sync_unlock();
    if (changed) net_abort(&poll_slot);
}

void sync_load_stickers(int tab) {
    want_stk = tab;
    net_abort(&poll_slot);
}
