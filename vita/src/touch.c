#include "touch.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>

TouchState T;

#define TAP_SLOP 14       // px que se puede mover y seguir siendo toque
#define TAP_MS   450

static unsigned long long down_us = 0;
static float vhist[4];
static int vh = 0;

void touch_init(void) {
    memset(&T, 0, sizeof(T));
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
}

void touch_update(void) {
    SceTouchData d;
    memset(&d, 0, sizeof(d));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &d, 1);
    int now_down = d.reportNum > 0;
    int nx = now_down ? d.report[0].x / 2 : T.x;   // el panel mide 1920x1088
    int ny = now_down ? d.report[0].y / 2 : T.y;

    T.tapped = 0;
    T.released = 0;
    T.dx = T.dy = 0;

    if (now_down && !T.down) {
        T.down = 1;
        T.start_x = T.x = nx;
        T.start_y = T.y = ny;
        T.dragging = 0;
        down_us = sceKernelGetProcessTimeWide();
        vh = 0;
        memset(vhist, 0, sizeof(vhist));
    } else if (now_down) {
        T.dx = nx - T.x;
        T.dy = ny - T.y;
        T.x = nx;
        T.y = ny;
        vhist[vh++ % 4] = (float)T.dy;
        if (!T.dragging && (abs(T.x - T.start_x) > TAP_SLOP || abs(T.y - T.start_y) > TAP_SLOP)) T.dragging = 1;
    } else if (T.down) {
        T.down = 0;
        T.released = 1;
        unsigned long long held = (sceKernelGetProcessTimeWide() - down_us) / 1000;
        if (!T.dragging && held < TAP_MS) {
            T.tapped = 1;
            T.tap_x = T.x;
            T.tap_y = T.y;
        }
        float v = 0;
        int n = vh < 4 ? vh : 4;
        for (int i = 0; i < n; i++) v += vhist[i];
        T.vy = n ? v / n : 0;
    }
}

int tap_in(float x, float y, float w, float h) {
    return T.tapped && T.tap_x >= x && T.tap_x < x + w && T.tap_y >= y && T.tap_y < y + h;
}

int drag_started_in(float x, float y, float w, float h) {
    return T.dragging && T.start_x >= x && T.start_x < x + w && T.start_y >= y && T.start_y < y + h;
}

// ---------- zonas-boton ----------
#define MAX_ZONES 24
typedef struct { float x, y, w, h; unsigned int b; } Zone;
static Zone zones[MAX_ZONES], last[MAX_ZONES];
static int nz = 0, nlast = 0;

void touch_zones_reset(void) {
    memcpy(last, zones, sizeof(Zone) * nz);
    nlast = nz;
    nz = 0;
}

void touch_zone(float x, float y, float w, float h, unsigned int buttons) {
    if (nz >= MAX_ZONES) return;
    zones[nz++] = (Zone){x, y, w, h, buttons};
}

unsigned int touch_zone_buttons(void) {
    if (!T.tapped) return 0;
    for (int i = nlast - 1; i >= 0; i--) {
        Zone *z = &last[i];
        if (T.tap_x >= z->x && T.tap_x < z->x + z->w && T.tap_y >= z->y && T.tap_y < z->y + z->h) return z->b;
    }
    return 0;
}

// ---------- scroll ----------
void scroller_update(Scroller *s, float ax, float ay, float aw, float ah, float max, int invert) {
    if (max < 0) max = 0;
    int inside = T.start_x >= ax && T.start_x < ax + aw && T.start_y >= ay && T.start_y < ay + ah;
    if (T.down && T.dragging && inside) {
        s->grabbed = 1;
        s->target = -1;
        s->pos += invert ? T.dy : -T.dy;
        s->vel = 0;
    } else if (s->grabbed && T.released) {
        s->grabbed = 0;
        s->vel = invert ? T.vy : -T.vy;
    } else if (!T.down) {
        s->grabbed = 0;
    }

    if (s->target > max) s->target = max;
    if (!s->grabbed) {
        if (s->target >= 0) {
            // con los botones: se desliza suave hasta su destino
            float d = s->target - s->pos;
            s->pos += d * 0.35f;
            if (fabsf(d) < 0.5f) {
                s->pos = s->target;
                s->target = -1;
            }
        } else if (fabsf(s->vel) > 0.1f) {
            s->pos += s->vel;
            s->vel *= 0.94f;
        } else {
            s->vel = 0;
        }
    }
    // rebote suave en los bordes
    if (s->pos < 0) {
        s->pos = s->grabbed ? s->pos : s->pos * 0.6f;
        if (!s->grabbed && s->pos > -0.5f) s->pos = 0;
        s->vel = 0;
    }
    if (s->pos > max) {
        s->pos = s->grabbed ? s->pos : max + (s->pos - max) * 0.6f;
        if (!s->grabbed && s->pos - max < 0.5f) s->pos = max;
        s->vel = 0;
    }
}

void scroller_show(Scroller *s, float item_top, float item_h, float view_h) {
    float t = s->target >= 0 ? s->target : s->pos;
    if (item_top < t) t = item_top;
    if (item_top + item_h > t + view_h) t = item_top + item_h - view_h;
    if (t < 0) t = 0;
    if (fabsf(t - s->pos) > 0.5f) s->target = t;
}
