#include "gallery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>

#define THUMB_W 176
#define THUMB_H 128
#define THUMB_SLOTS 40

typedef struct { char path[200]; unsigned long long when; } Item;
static Item items[GAL_MAX];
static int count = 0;

typedef struct { int idx; vita2d_texture *t; unsigned int used; int failed; } Thumb;
static Thumb thumbs[THUMB_SLOTS];
static unsigned int tick = 0;
static int want = -1;   // miniatura pedida este frame

static unsigned long long stamp(const SceDateTime *d) {
    return ((((((unsigned long long)d->year * 13 + d->month) * 32 + d->day) * 24 + d->hour) * 60 + d->minute) * 60 + d->second);
}

static int is_image(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png") || !strcasecmp(dot, ".bmp"));
}

static void walk(const char *dir, int depth) {
    if (depth > 3 || count >= GAL_MAX) return;
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    while (count < GAL_MAX && sceIoDread(d, &e) > 0) {
        if (e.d_name[0] == '.') continue;
        char full[200];
        snprintf(full, sizeof(full), "%s/%s", dir, e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) walk(full, depth + 1);
        else if (is_image(e.d_name) && e.d_stat.st_size < 8 * 1024 * 1024) {
            snprintf(items[count].path, sizeof(items[count].path), "%s", full);
            items[count].when = stamp(&e.d_stat.st_mtime);
            count++;
        }
    }
    sceIoDclose(d);
}

static int by_date(const void *a, const void *b) {
    const Item *x = a, *y = b;
    return x->when < y->when ? 1 : x->when > y->when ? -1 : 0;
}

void gallery_free(void) {
    vita2d_wait_rendering_done();
    for (int i = 0; i < THUMB_SLOTS; i++) {
        if (thumbs[i].t) vita2d_free_texture(thumbs[i].t);
        memset(&thumbs[i], 0, sizeof(Thumb));
        thumbs[i].idx = -1;
    }
}

int gallery_scan(int folder) {
    gallery_free();
    count = 0;
    if (folder == GAL_SHOTS) walk("ux0:picture/SCREENSHOT", 0);
    else if (folder == GAL_CAMERA) {
        walk("ux0:picture/CAMERA", 0);
        walk("ux0:picture/PHOTO", 0);
    } else walk("ux0:picture", 0);
    qsort(items, count, sizeof(Item), by_date);
    return count;
}

int gallery_count(void) { return count; }
const char *gallery_path(int i) { return i >= 0 && i < count ? items[i].path : ""; }

// reduce la imagen grande a una miniatura (la grande se libera enseguida)
static vita2d_texture *make_thumb(vita2d_texture *big) {
    int bw = vita2d_texture_get_width(big), bh = vita2d_texture_get_height(big);
    vita2d_texture *t = vita2d_create_empty_texture(THUMB_W, THUMB_H);
    if (!t) return NULL;
    // recorte centrado tipo "cover"
    float s = (float)THUMB_W / bw > (float)THUMB_H / bh ? (float)THUMB_W / bw : (float)THUMB_H / bh;
    float sw = THUMB_W / s, sh = THUMB_H / s;
    float ox = (bw - sw) / 2, oy = (bh - sh) / 2;
    const unsigned int *src = vita2d_texture_get_datap(big);
    unsigned int ss = vita2d_texture_get_stride(big) / 4;
    unsigned int *dst = vita2d_texture_get_datap(t);
    unsigned int ds = vita2d_texture_get_stride(t) / 4;
    for (int y = 0; y < THUMB_H; y++) {
        int sy = (int)(oy + (y + 0.5f) / s);
        if (sy >= bh) sy = bh - 1;
        for (int x = 0; x < THUMB_W; x++) {
            int sx = (int)(ox + (x + 0.5f) / s);
            if (sx >= bw) sx = bw - 1;
            dst[y * ds + x] = src[sy * ss + sx] | 0xFF000000;
        }
    }
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return t;
}

vita2d_texture *gallery_thumb(int i) {
    for (int k = 0; k < THUMB_SLOTS; k++) {
        if (thumbs[k].idx == i && (thumbs[k].t || thumbs[k].failed)) {
            thumbs[k].used = tick;
            return thumbs[k].t;
        }
    }
    if (want < 0) want = i;
    return NULL;
}

void gallery_pump(void) {
    tick++;
    if (want < 0 || want >= count) {
        want = -1;
        return;
    }
    int i = want;
    want = -1;
    // hueco: libre o el menos usado
    int slot = 0;
    for (int k = 0; k < THUMB_SLOTS; k++) {
        if (thumbs[k].idx < 0 || (!thumbs[k].t && !thumbs[k].failed)) { slot = k; break; }
        if (thumbs[k].used < thumbs[slot].used) slot = k;
    }
    if (thumbs[slot].t) {
        vita2d_wait_rendering_done();
        vita2d_free_texture(thumbs[slot].t);
    }
    thumbs[slot].t = NULL;
    thumbs[slot].failed = 0;
    thumbs[slot].idx = i;
    thumbs[slot].used = tick;

    const char *p = items[i].path;
    const char *dot = strrchr(p, '.');
    vita2d_texture *big = NULL;
    if (!strcasecmp(dot, ".png")) big = vita2d_load_PNG_file(p);
    else if (!strcasecmp(dot, ".bmp")) big = vita2d_load_BMP_file(p);
    else big = vita2d_load_JPEG_file(p);
    if (big) {
        thumbs[slot].t = make_thumb(big);
        vita2d_free_texture(big);
    }
    if (!thumbs[slot].t) thumbs[slot].failed = 1;
}

int gallery_read(int i, unsigned char **out) {
    *out = NULL;
    FILE *f = fopen(gallery_path(i), "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 8 * 1024 * 1024) {
        fclose(f);
        return 0;
    }
    unsigned char *b = malloc(n);
    if (b && fread(b, 1, n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *out = b;
    return b ? (int)n : 0;
}
