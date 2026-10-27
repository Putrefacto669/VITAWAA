#ifndef VITAWA_GALLERY_H
#define VITAWA_GALLERY_H

// Galeria: imagenes de ux0:picture (capturas de pantalla y fotos de la camara).

#include <vita2d.h>

#define GAL_MAX 300

enum { GAL_SHOTS, GAL_CAMERA, GAL_ALL };

int  gallery_scan(int folder);                 // devuelve cuantas encontro
int  gallery_count(void);
const char *gallery_path(int i);
vita2d_texture *gallery_thumb(int i);          // NULL mientras carga (carga 1 por frame)
void gallery_pump(void);                       // llamar una vez por frame
void gallery_free(void);
// Lee el archivo completo (malloc). Devuelve bytes o 0.
int  gallery_read(int i, unsigned char **out);

#endif
