#ifndef VITAWA_TEX_H
#define VITAWA_TEX_H

// Cache de imagenes que vienen del bridge (fotos, stickers, estados, emojis).
// Un hilo descarga los PNG y el hilo principal crea las texturas (tex_pump),
// porque la GPU se maneja mejor desde un solo hilo.

#include <vita2d.h>

void tex_init(void);
// path = ruta del bridge con su query, ej. "/v1/emoji?n=1f44d&s=40".
// Devuelve NULL mientras carga (y la pide si no la tiene).
vita2d_texture *tex_get(const char *path);
// Igual, pero dice si fallo definitivamente (para no dibujar el "cargando").
int tex_failed(const char *path);
// Llamar una vez por frame, antes de vita2d_start_drawing().
void tex_pump(void);
// Olvida todo (por ejemplo al cambiar de bridge).
void tex_reset(void);

#endif
