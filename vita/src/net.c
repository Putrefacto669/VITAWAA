#include "net.h"

// ═════════════════════════════════════════════════════════════════════════════
//  net.c — HTTP(S) con libcurl + OpenSSL.
//
//  Antes se usaba SceHttp (el HTTP de la propia Vita), que no sabe hablar con los
//  certificados modernos: con eso el bridge solo funcionaba por http:// en tu red
//  o en un VPS. Render (y casi cualquier hosting) solo atiende por https://, así
//  que ahora usamos curl. La verificación del certificado se desactiva A PROPÓSITO:
//  todo lo que viaja ya va cifrado y autenticado de punta a punta con la llave del
//  emparejamiento (XChaCha20-Poly1305, ver chan.c), así que el TLS solo es "el sobre".
//  Además así no dependemos de que el reloj de la Vita esté bien para el TLS.
//
//  Cada hilo (identificado por su `slot`) reutiliza su propia conexión (keep-alive):
//  un handshake TLS en el A9 cuesta ~0.5 s, no queremos uno por cada avatar.
// ═════════════════════════════════════════════════════════════════════════════

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/kernel/threadmgr.h>
#include <curl/curl.h>

#define NET_POOL  (1 * 1024 * 1024)
#define HANDLES   8

static void *net_mem = NULL;
static int inited = 0;

// slot (puntero del hilo) -> handle de curl reutilizable
static struct { volatile int *slot; CURL *h; int busy; int stale; } pool[HANDLES];
static SceKernelLwMutexWork pmtx;

int net_init(void) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    net_mem = malloc(NET_POOL);
    if (!net_mem) return -1;
    SceNetInitParam p;
    p.memory = net_mem;
    p.size = NET_POOL;
    p.flags = 0;
    int r = sceNetInit(&p);
    if (r < 0 && r != (int)SCE_NET_ERROR_EBUSY) return r;
    sceNetCtlInit();
    if (curl_global_init(CURL_GLOBAL_ALL) != 0) return -1;
    sceKernelCreateLwMutex(&pmtx, "vitawa_net", 0, 0, NULL);
    memset(pool, 0, sizeof(pool));
    inited = 1;
    return 0;
}

void net_term(void) {
    for (int i = 0; i < HANDLES; i++) if (pool[i].h) curl_easy_cleanup(pool[i].h);
    memset(pool, 0, sizeof(pool));
    curl_global_cleanup();
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    free(net_mem);
    net_mem = NULL;
    inited = 0;
}

// el slot vale 1 mientras hay petición; net_abort lo pone en 2 y el callback corta
void net_abort(volatile int *slot) {
    if (slot && *slot == 1) *slot = 2;
}

// Al volver de la suspensión las conexiones keep-alive están muertas: se cortan
// las peticiones en curso y cada hilo abre una conexión nueva en la siguiente.
void net_reset(void) {
    if (!inited) return;
    sceKernelLockLwMutex(&pmtx, 1, NULL);
    for (int i = 0; i < HANDLES; i++) {
        if (!pool[i].h) continue;
        pool[i].stale = 1;
        if (pool[i].busy) net_abort(pool[i].slot);
    }
    sceKernelUnlockLwMutex(&pmtx, 1);
}

static CURL *take_handle(volatile int *slot, int *idx) {
    sceKernelLockLwMutex(&pmtx, 1, NULL);
    int free_i = -1;
    for (int i = 0; i < HANDLES; i++) {
        if (slot && pool[i].slot == slot && pool[i].h && !pool[i].busy) {
            pool[i].busy = 1; *idx = i;
            if (pool[i].stale) {   // la conexión guardada murió con la suspensión: una nueva
                pool[i].stale = 0;
                sceKernelUnlockLwMutex(&pmtx, 1);
                curl_easy_cleanup(pool[i].h);
                pool[i].h = curl_easy_init();
                return pool[i].h;
            }
            sceKernelUnlockLwMutex(&pmtx, 1);
            curl_easy_reset(pool[i].h);
            return pool[i].h;
        }
        if (free_i < 0 && !pool[i].h) free_i = i;
    }
    CURL *h = curl_easy_init();
    if (slot && free_i >= 0 && h) { pool[free_i].slot = slot; pool[free_i].h = h; pool[free_i].busy = 1; *idx = free_i; }
    else *idx = -1;   // handle de un solo uso
    sceKernelUnlockLwMutex(&pmtx, 1);
    return h;
}

static void give_handle(CURL *h, int idx) {
    if (idx < 0) { curl_easy_cleanup(h); return; }
    sceKernelLockLwMutex(&pmtx, 1, NULL);
    pool[idx].busy = 0;
    sceKernelUnlockLwMutex(&pmtx, 1);
}

typedef struct { char *buf; int used, cap, max; } Sink;

static size_t on_data(void *ptr, size_t sz, size_t n, void *ud) {
    Sink *k = ud;
    size_t add = sz * n;
    if (k->used + (int)add > k->max) {           // no cabe: tiramos lo que sobra
        add = k->max - k->used;
        if ((int)add <= 0) return sz * n;
    }
    if (k->used + (int)add + 1 > k->cap) {
        int ncap = k->cap * 2;
        while (ncap < k->used + (int)add + 1) ncap *= 2;
        if (ncap > k->max + 1) ncap = k->max + 1;
        char *nb = realloc(k->buf, ncap);
        if (!nb) return 0;
        k->buf = nb; k->cap = ncap;
    }
    memcpy(k->buf + k->used, ptr, add);
    k->used += add;
    return sz * n;
}

static int on_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    volatile int *slot = ud;
    return (slot && *slot == 2) ? 1 : 0;
}

int net_post(const char *url, const void *body, int body_len, char **out, int *out_len,
             int max_len, unsigned int recv_timeout_s, volatile int *slot) {
    *out = NULL;
    *out_len = 0;
    if (!inited) return -1;
    int idx;
    CURL *h = take_handle(slot, &idx);
    if (!h) return -1;

    Sink k = { malloc(16 * 1024), 0, 16 * 1024, max_len };
    if (!k.buf) { give_handle(h, idx); return -1; }

    struct curl_slist *hdr = curl_slist_append(NULL, "Content-Type: application/octet-stream");
    hdr = curl_slist_append(hdr, "Expect:");   // sin "100-continue": un viaje menos
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "VitaWA/3.5");
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);   // ver nota arriba: el canal ya va cifrado
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_POSTREDIR, (long)CURL_REDIR_POST_ALL);   // http→https sin perder el POST
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, (long)(recv_timeout_s + 20));
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &k);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, (void *)slot);

    if (slot) *slot = 1;
    CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (slot) *slot = -1;
    curl_slist_free_all(hdr);
    give_handle(h, idx);

    if (rc != CURLE_OK) { free(k.buf); return -1; }
    k.buf[k.used] = '\0';
    *out = k.buf;
    *out_len = k.used;
    return (int)status;
}

void url_enc(const char *in, char *out, int cap) {
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n < cap - 4; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '@' || *p == ':') {
            out[n++] = (char)*p;
        } else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = '\0';
}
