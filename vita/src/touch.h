#ifndef VITAWA_TOUCH_H
#define VITAWA_TOUCH_H

// Pantalla tactil delantera: toques, arrastres con inercia y "botones" tactiles.
// Las ayudas de abajo (✕ Abrir, ○ Volver...) se registran como zonas tactiles:
// tocarlas equivale a apretar ese boton.

typedef struct {
    int down;            // dedo en la pantalla
    int x, y;            // posicion actual (960x544)
    int start_x, start_y;
    int dragging;        // ya se movio lo suficiente para ser arrastre
    int dx, dy;          // movimiento desde el frame anterior
    int tapped;          // se solto sin moverse: es un toque (solo este frame)
    int tap_x, tap_y;
    int released;        // se solto este frame
    float vy;            // velocidad vertical al soltar (px/frame)
} TouchState;

extern TouchState T;

void touch_init(void);
void touch_update(void);

int  tap_in(float x, float y, float w, float h);        // toque dentro del rect este frame
int  drag_started_in(float x, float y, float w, float h); // el arrastre empezo en el rect

// Zonas que actuan como botones (se registran al dibujar, se leen el frame siguiente)
void touch_zones_reset(void);
void touch_zone(float x, float y, float w, float h, unsigned int buttons);
unsigned int touch_zone_buttons(void);

// Scroll con inercia
typedef struct {
    float pos, vel;      // pos: px desplazados
    float target;        // a donde ir con los botones (-1 = libre)
    int   grabbed;
} Scroller;

// area: zona de la pantalla que se arrastra. max: desplazamiento maximo.
// invert: 1 si arrastrar hacia abajo debe aumentar pos (listas pegadas abajo, como el chat).
void scroller_update(Scroller *s, float ax, float ay, float aw, float ah, float max, int invert);
void scroller_show(Scroller *s, float item_top, float item_h, float view_h);  // asegura que el item se vea

#endif
