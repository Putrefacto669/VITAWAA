#include "tex.h"
#include "net.h"
#include "chan.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <psp2/kernel/threadmgr.h>

#define TEX_SLOTS   220
#define QUEUE_LEN   32
#define KEY_LEN     200
#define MAX_PNG     (1024 * 1024)

enum { T_EMPTY, T_QUEUED, T_LOADING, T_READY, T_FAILED };

typedef struct {
    char key[KEY_LEN];
    int state;
    vita2d_texture *tex;
    unsigned int last_used;
    // PNG descargado esperando a convertirse en textura
    char *png;
    int png_len;
} Slot;

static Slot slots[TEX_SLOTS];
static unsigned int frame_no = 0;
static SceKernelLwMutexWork tmtx;
static SceUID tsema = -1;
static volatile int generation = 0;
static volatile int req_slots[2] = {-1, -1};
// texturas a liberar: se liberan en tex_pump, fuera del dibujo y con la GPU ya libre
static vita2d_texture *to_free[TEX_SLOTS];
static int nfree = 0;

static void lock(void)   { sceKernelLockLwMutex(&tmtx, 1, NULL); }
static void unlock(void) { sceKernelUnlockLwMutex(&tmtx, 1); }

static int find(const char *key) {
    for (int i = 0; i < TEX_SLOTS; i++)
        if (slots[i].state != T_EMPTY && !strcmp(slots[i].key, key)) return i;
    return -1;
}

// Libera la textura menos usada que no se haya dibujado en este frame
static int evict(void) {
    int best = -1;
    unsigned int oldest = 0xFFFFFFFF;
    for (int i = 0; i < TEX_SLOTS; i++) {
        if (slots[i].state == T_EMPTY) return i;
        if ((slots[i].state == T_READY || slots[i].state == T_FAILED) &&
            slots[i].last_used + 2 < frame_no && slots[i].last_used < oldest) {
            oldest = slots[i].last_used;
            best = i;
        }
    }
    if (best >= 0 && slots[best].tex) {
        if (nfree < TEX_SLOTS) to_free[nfree++] = slots[best].tex;
        slots[best].tex = NULL;
    }
    if (best >= 0) slots[best].state = T_EMPTY;
    return best;
}

static int loader(SceSize args, void *argp) {
    int me = (args >= sizeof(int)) ? *(int *)argp : 0;
    for (;;) {
        sceKernelWaitSema(tsema, 1, NULL);

        // toma la siguiente imagen pedida (la mas reciente primero: es la que esta en pantalla)
        lock();
        int pick = -1;
        unsigned int newest = 0;
        for (int i = 0; i < TEX_SLOTS; i++) {
            if (slots[i].state == T_QUEUED && slots[i].last_used >= newest) {
                newest = slots[i].last_used;
                pick = i;
            }
        }
        char key[KEY_LEN];
        int gen = generation;
        if (pick >= 0) {
            slots[pick].state = T_LOADING;
            snprintf(key, sizeof(key), "%s", slots[pick].key);
        }
        unlock();
        if (pick < 0) continue;

        char *buf = NULL;
        int len = 0;
        int code = chan_call("GET", key, NULL, 0, &buf, &len, MAX_PNG, 25, &req_slots[me]);

        lock();
        if (gen == generation && !strcmp(slots[pick].key, key) && slots[pick].state == T_LOADING) {
            if (code == 200 && len > 8) {
                slots[pick].png = buf;
                slots[pick].png_len = len;
                buf = NULL;
            } else {
                slots[pick].state = T_FAILED;
            }
        }
        unlock();
        free(buf);
    }
    return 0;
}

void tex_init(void) {
    memset(slots, 0, sizeof(slots));
    sceKernelCreateLwMutex(&tmtx, "vitawa_tex", 0, 0, NULL);
    tsema = sceKernelCreateSema("vitawa_tex_sema", 0, 0, 1000, NULL);
    // dos descargas en paralelo: una foto grande no frena a los emojis
    for (int i = 0; i < 2; i++) {
        SceUID th = sceKernelCreateThread("vitawa_tex", loader, 0x10000100, 0x40000, 0, 0, NULL);
        if (th >= 0) sceKernelStartThread(th, sizeof(int), &i);   // la Vita copia el argumento
    }
}

vita2d_texture *tex_get(const char *path) {
    lock();
    int i = find(path);
    if (i < 0) {
        i = evict();
        if (i < 0) { unlock(); return NULL; }
        memset(&slots[i], 0, sizeof(Slot));
        snprintf(slots[i].key, KEY_LEN, "%s", path);
        slots[i].state = T_QUEUED;
        slots[i].last_used = frame_no;
        unlock();
        sceKernelSignalSema(tsema, 1);
        return NULL;
    }
    slots[i].last_used = frame_no;
    vita2d_texture *t = slots[i].state == T_READY ? slots[i].tex : NULL;
    unlock();
    return t;
}

int tex_failed(const char *path) {
    lock();
    int i = find(path);
    int r = i >= 0 && slots[i].state == T_FAILED;
    unlock();
    return r;
}

void tex_pump(void) {
    frame_no++;
    int made = 0;
    lock();
    if (nfree > 0) {
        vita2d_wait_rendering_done();
        for (int i = 0; i < nfree; i++) vita2d_free_texture(to_free[i]);
        nfree = 0;
    }
    for (int i = 0; i < TEX_SLOTS && made < 3; i++) {
        if (slots[i].state == T_LOADING && slots[i].png) {
            vita2d_texture *t = vita2d_load_PNG_buffer(slots[i].png);
            free(slots[i].png);
            slots[i].png = NULL;
            if (t) {
                vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
                slots[i].tex = t;
                slots[i].state = T_READY;
            } else {
                slots[i].state = T_FAILED;
            }
            made++;
        }
    }
    unlock();
}

void tex_reset(void) {
    lock();
    generation++;
    vita2d_wait_rendering_done();
    for (int i = 0; i < nfree; i++) vita2d_free_texture(to_free[i]);
    nfree = 0;
    for (int i = 0; i < TEX_SLOTS; i++) {
        if (slots[i].tex) vita2d_free_texture(slots[i].tex);
        free(slots[i].png);
        memset(&slots[i], 0, sizeof(Slot));
    }
    unlock();
    net_abort(&req_slots[0]);
    net_abort(&req_slots[1]);
}
