#include "chan.h"
#include "net.h"
#include "third_party/monocypher/monocypher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/rtc.h>

static char base_url[160];
static unsigned char chan_key[32];
static int have_key = 0;
static long long clock_fix = 0;   // ms a sumar a nuestra hora (lo dice el bridge si el reloj esta mal)
static SceKernelLwMutexWork mtx;
static int mtx_ready = 0;

static const unsigned char AD[4] = {'V', 'W', 'A', '1'};

static void lock(void) {
    if (!mtx_ready) {
        sceKernelCreateLwMutex(&mtx, "vitawa_chan", 0, 0, NULL);
        mtx_ready = 1;
    }
    sceKernelLockLwMutex(&mtx, 1, NULL);
}
static void unlock(void) { sceKernelUnlockLwMutex(&mtx, 1); }

void chan_set(const char *url, const unsigned char *key) {
    lock();
    snprintf(base_url, sizeof(base_url), "%s", url ? url : "");
    // sin "/" al final
    int n = (int)strlen(base_url);
    while (n > 0 && base_url[n - 1] == '/') base_url[--n] = '\0';
    if (key) {
        memcpy(chan_key, key, 32);
        have_key = 1;
    } else {
        crypto_wipe(chan_key, 32);
        have_key = 0;
    }
    unlock();
}

int chan_ready(void) { return have_key && base_url[0]; }

// ---------- reloj ----------
long long unix_ms(void) {
    SceRtcTick t;
    sceRtcGetCurrentTick(&t);   // microsegundos desde el anio 1 (UTC)
    return (long long)(t.tick / 1000ULL) - 62135596800000LL;
}

int tz_minutes(void) {
    SceDateTime local;
    SceRtcTick lt, ut;
    sceRtcGetCurrentClockLocalTime(&local);
    sceRtcGetTick(&local, &lt);
    sceRtcGetCurrentTick(&ut);
    long long diff_us = (long long)lt.tick - (long long)ut.tick;
    long long m = diff_us / 60000000LL;
    // redondeo a 15 minutos (hay zonas de :30 y :45)
    return (int)((m + (m >= 0 ? 7 : -7)) / 15 * 15);
}

// ---------- base64url ----------
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

void b64url_encode(const unsigned char *in, int len, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < len && o < cap - 4; i += 3) {
        unsigned int v = in[i] << 16 | (i + 1 < len ? in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = B64[(v >> 6) & 63];
        if (i + 2 < len) out[o++] = B64[v & 63];
    }
    out[o] = '\0';
}

int b64url_decode(const char *in, unsigned char *out, int cap) {
    unsigned int acc = 0;
    int bits = 0, n = 0;
    for (const char *p = in; *p; p++) {
        const char *k = strchr(B64, *p);
        int v;
        if (k && *p) v = (int)(k - B64);
        else if (*p == '+') v = 62;
        else if (*p == '/') v = 63;
        else if (*p == '=' || *p == ' ' || *p == '\n' || *p == '\r') continue;
        else return -1;
        acc = (acc << 6) | (unsigned int)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return n;
}

int chan_parse_pairing(const char *text, char *url, int url_cap, unsigned char key[32]) {
    char buf[300];
    snprintf(buf, sizeof(buf), "%s", text);
    char *save = NULL;
    char *tag = strtok_r(buf, " \t\r\n", &save);
    char *u = strtok_r(NULL, " \t\r\n", &save);
    char *k = strtok_r(NULL, " \t\r\n", &save);
    if (!tag || !u || !k || strcmp(tag, "VWA1")) return 0;
    unsigned char tmp[48];
    if (b64url_decode(k, tmp, sizeof(tmp)) != 32) return 0;
    memcpy(key, tmp, 32);
    // el bridge nuevo manda "host:puerto" sin "http://" (QR mas chico)
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) snprintf(url, url_cap, "http://%s", u);
    else snprintf(url, url_cap, "%s", u);
    return 1;
}

// ---------- peticiones ----------
static int call_once(const char *method, const char *path, const void *body, int body_len,
                     char **out, int *out_len, int max_out, unsigned int timeout_s,
                     volatile int *slot, long long *server_time) {
    char url[200];
    unsigned char key[32];
    lock();
    if (!have_key || !base_url[0]) {
        unlock();
        return CHAN_ERR_NOCFG;
    }
    snprintf(url, sizeof(url), "%s/x", base_url);
    memcpy(key, chan_key, 32);
    long long now = unix_ms() + clock_fix;
    unlock();

    // texto plano: "METODO RUTA\n<ms>\n" + cuerpo
    char head[1400];
    int hl = snprintf(head, sizeof(head), "%s %s%s_tz=%d\n%lld\n", method, path, strchr(path, '?') ? "&" : "?", tz_minutes(), now);
    if (hl <= 0 || hl >= (int)sizeof(head)) return CHAN_ERR_DATA;
    int plain_len = hl + body_len;
    int pkt_len = 24 + plain_len + 16;
    unsigned char *pkt = malloc(pkt_len);
    unsigned char *plain = malloc(plain_len > 0 ? plain_len : 1);
    if (!pkt || !plain) {
        free(pkt);
        free(plain);
        return CHAN_ERR_DATA;
    }
    memcpy(plain, head, hl);
    if (body_len) memcpy(plain + hl, body, body_len);

    sceKernelGetRandomNumber(pkt, 24);
    crypto_aead_lock(pkt + 24, pkt + 24 + plain_len, key, pkt, AD, sizeof(AD), plain, plain_len);
    crypto_wipe(plain, plain_len);
    free(plain);

    char *resp = NULL;
    int resp_len = 0;
    int http = net_post(url, pkt, pkt_len, &resp, &resp_len, max_out + 4096, timeout_s, slot);
    free(pkt);
    if (http < 0) {
        crypto_wipe(key, 32);
        return CHAN_ERR_NET;
    }
    if (http == 401) {
        free(resp);
        crypto_wipe(key, 32);
        return CHAN_ERR_KEY;
    }
    if (http != 200 || resp_len < 24 + 16) {
        free(resp);
        crypto_wipe(key, 32);
        return CHAN_ERR_NET;
    }

    int clen = resp_len - 24 - 16;
    unsigned char *dec = malloc(clen + 1);
    if (!dec) {
        free(resp);
        return CHAN_ERR_DATA;
    }
    int bad = crypto_aead_unlock(dec, (unsigned char *)resp + 24 + clen, key, (unsigned char *)resp, AD, sizeof(AD),
                                 (unsigned char *)resp + 24, clen);
    free(resp);
    crypto_wipe(key, 32);
    if (bad) {
        free(dec);
        return CHAN_ERR_DATA;
    }
    dec[clen] = '\0';

    // "<codigo>\n<tipo>\n" + cuerpo
    char *nl1 = memchr(dec, '\n', clen);
    char *nl2 = nl1 ? memchr(nl1 + 1, '\n', clen - (nl1 + 1 - (char *)dec)) : NULL;
    if (!nl1 || !nl2) {
        free(dec);
        return CHAN_ERR_DATA;
    }
    int code = atoi((char *)dec);
    char *payload = nl2 + 1;
    int plen = clen - (int)(payload - (char *)dec);

    if (code == 425 && server_time) *server_time = atoll(payload);

    // devolvemos solo el cuerpo (moviendolo al inicio del mismo bloque)
    memmove(dec, payload, plen);
    dec[plen] = '\0';
    *out = (char *)dec;
    *out_len = plen;
    return code;
}

int chan_call(const char *method, const char *path, const void *body, int body_len,
              char **out, int *out_len, int max_out, unsigned int timeout_s, volatile int *slot) {
    *out = NULL;
    *out_len = 0;
    long long server_time = 0;
    int code = call_once(method, path, body, body_len, out, out_len, max_out, timeout_s, slot, &server_time);
    if (code == 425 && server_time > 0) {
        // el reloj de la Vita esta mal: nos ajustamos a la hora del bridge y reintentamos
        free(*out);
        *out = NULL;
        lock();
        clock_fix = server_time - unix_ms();
        unlock();
        code = call_once(method, path, body, body_len, out, out_len, max_out, timeout_s, slot, NULL);
    }
    return code;
}
