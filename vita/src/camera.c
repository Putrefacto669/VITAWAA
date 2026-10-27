#include "camera.h"
#include "third_party/quirc/quirc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <psp2/camera.h>
#include <psp2/kernel/threadmgr.h>

static vita2d_texture *tex = NULL;
static int open_dev = -1;
static int frozen = 0;
static SceCameraInfo info;
static SceCameraRead rd;

static SceKernelLwMutexWork mtx;
static int mtx_ready = 0;
static void lock(void)   { sceKernelLockLwMutex(&mtx, 1, NULL); }
static void unlock(void) { sceKernelUnlockLwMutex(&mtx, 1); }

// ---------- QR ----------
static volatile int scan_on = 0;
static char scan_prefix[16];
static char qr_text[512];
static volatile int qr_ready = 0;
static unsigned char *gray = NULL;      // copia en grises para quirc
static volatile int gray_fresh = 0;
static SceUID scan_th = -1;
static SceUID scan_sema = -1;
// para avisar en pantalla que esta pasando
static volatile int st_frames = 0, st_seen = 0, st_bad = 0, st_other = 0;

static int scan_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    struct quirc *q = quirc_new();
    if (!q || quirc_resize(q, CAM_W, CAM_H) < 0) return sceKernelExitDeleteThread(0);
    for (;;) {
        sceKernelWaitSema(scan_sema, 1, NULL);
        if (!scan_on || !gray_fresh) continue;
        int w, h;
        unsigned char *img = quirc_begin(q, &w, &h);
        lock();
        memcpy(img, gray, (size_t)CAM_W * CAM_H);
        gray_fresh = 0;
        unlock();
        quirc_end(q);
        int n = quirc_count(q);
        st_frames++;
        if (n > 0) st_seen = st_frames;
        for (int i = 0; i < n; i++) {
            static struct quirc_code code;
            static struct quirc_data data;
            quirc_extract(q, i, &code);
            if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
                quirc_flip(&code);   // por si viene en espejo (camara frontal)
                if (quirc_decode(&code, &data) != QUIRC_SUCCESS) { st_bad = st_frames; continue; }
            }
            const char *txt = (const char *)data.payload;
            if (!strncmp(txt, scan_prefix, strlen(scan_prefix))) {
                lock();
                snprintf(qr_text, sizeof(qr_text), "%s", txt);
                qr_ready = 1;
                unlock();
            } else {
                st_other = st_frames;
            }
        }
    }
    return 0;
}

static void ensure_init(void) {
    if (mtx_ready) return;
    sceKernelCreateLwMutex(&mtx, "vitawa_cam", 0, 0, NULL);
    mtx_ready = 1;
    scan_sema = sceKernelCreateSema("vitawa_scan", 0, 0, 1, NULL);
    gray = malloc((size_t)CAM_W * CAM_H);
    scan_th = sceKernelCreateThread("vitawa_qr", scan_main, 0x10000100 + 10, 0x40000, 0, 0, NULL);
    if (scan_th >= 0) sceKernelStartThread(scan_th, 0, NULL);
}

void cam_scan(int on, const char *prefix) {
    ensure_init();
    lock();
    scan_on = on;
    st_frames = st_seen = st_bad = st_other = 0;
    snprintf(scan_prefix, sizeof(scan_prefix), "%s", prefix ? prefix : "");
    qr_ready = 0;
    unlock();
}

int cam_scan_hint(void) {
    int f = st_frames;
    if (f == 0) return 0;
    if (st_other && f - st_other < 8) return 3;   // lee un QR, pero no es el de VitaWA
    if (st_bad && f - st_bad < 8) return 2;       // ve un QR pero no lo logra leer
    if (st_seen && f - st_seen < 8) return 2;
    return 1;                                      // buscando
}

int cam_qr_result(char *out, int cap) {
    if (!mtx_ready) return 0;
    lock();
    int r = qr_ready;
    if (r) {
        snprintf(out, cap, "%s", qr_text);
        qr_ready = 0;
    }
    unlock();
    return r;
}

// ---------- camara ----------
int cam_open(int back) {
    ensure_init();
    if (open_dev >= 0) cam_close();
    if (!tex) tex = vita2d_create_empty_texture(CAM_W, CAM_H);
    if (!tex) return -1;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.priority = SCE_CAMERA_PRIORITY_SHARE;
    info.format = SCE_CAMERA_FORMAT_ABGR;
    info.resolution = SCE_CAMERA_RESOLUTION_640_480;
    info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
    info.width = CAM_W;
    info.height = CAM_H;
    info.range = 1;
    info.sizeIBase = 4 * CAM_W * CAM_H;
    info.pitch = 0;   // 640 ya es multiplo de 8: la textura no tiene relleno
    info.pIBase = vita2d_texture_get_datap(tex);

    int dev = back ? SCE_CAMERA_DEVICE_BACK : SCE_CAMERA_DEVICE_FRONT;
    if (sceCameraOpen(dev, &info) < 0) return -1;
    if (sceCameraStart(dev) < 0) {
        sceCameraClose(dev);
        return -1;
    }
    open_dev = dev;
    frozen = 0;
    memset(&rd, 0, sizeof(rd));
    rd.size = sizeof(rd);
    return 0;
}

void cam_close(void) {
    if (open_dev < 0) return;
    sceCameraStop(open_dev);
    sceCameraClose(open_dev);
    open_dev = -1;
    scan_on = 0;
}

int cam_is_open(void) { return open_dev >= 0; }
int cam_is_back(void) { return open_dev == SCE_CAMERA_DEVICE_BACK; }
vita2d_texture *cam_texture(void) { return open_dev >= 0 || frozen ? tex : NULL; }
void cam_freeze(int on) { frozen = on; }

void cam_update(void) {
    if (open_dev < 0 || frozen) return;
    rd.size = sizeof(rd);
    rd.mode = 0;
    sceCameraRead(open_dev, &rd);

    // cada 3 cuadros (cuando el lector esta libre) mandamos una copia en grises al lector QR
    static int n = 0;
    if (scan_on && !gray_fresh && (++n % 3) == 0) {
        const unsigned int *px = vita2d_texture_get_datap(tex);
        unsigned int stride = vita2d_texture_get_stride(tex) / 4;
        lock();
        for (int y = 0; y < CAM_H; y++) {
            const unsigned int *row = px + y * stride;
            unsigned char *g = gray + y * CAM_W;
            for (int x = 0; x < CAM_W; x++) {
                unsigned int c = row[x];
                // luminancia aproximada: (R + 2G + B) / 4
                g[x] = (unsigned char)(((c & 0xFF) + ((c >> 7) & 0x1FE) + ((c >> 16) & 0xFF)) >> 2);
            }
        }
        gray_fresh = 1;
        unlock();
        sceKernelSignalSema(scan_sema, 1);
    }
}

int cam_snapshot_i420(unsigned char **out) {
    *out = NULL;
    if (!tex) return 0;
    int ysz = CAM_W * CAM_H, csz = ysz / 4;
    unsigned char *buf = malloc(ysz + 2 * csz);
    if (!buf) return 0;
    const unsigned int *px = vita2d_texture_get_datap(tex);
    unsigned int stride = vita2d_texture_get_stride(tex) / 4;
    unsigned char *Y = buf, *U = buf + ysz, *V = U + csz;
    for (int y = 0; y < CAM_H; y++) {
        for (int x = 0; x < CAM_W; x++) {
            unsigned int c = px[y * stride + x];
            int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
            Y[y * CAM_W + x] = (unsigned char)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            if (!(y & 1) && !(x & 1)) {
                int i = (y / 2) * (CAM_W / 2) + x / 2;
                U[i] = (unsigned char)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                V[i] = (unsigned char)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }
    *out = buf;
    return ysz + 2 * csz;
}
