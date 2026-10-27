#ifndef VITAWA_CAMERA_H
#define VITAWA_CAMERA_H

// Camara de la Vita: vista previa en una textura, foto en I420 para el bridge
// y lector de codigos QR (quirc) para emparejar.

#include <vita2d.h>

#define CAM_W 640
#define CAM_H 480

int  cam_open(int back);            // 0 si abrio
void cam_close(void);
int  cam_is_open(void);
int  cam_is_back(void);
void cam_update(void);              // llamar una vez por frame
vita2d_texture *cam_texture(void);  // la vista previa (ABGR, la camara escribe directo)

// Copia el cuadro actual como I420 (Y, luego U y V a un cuarto). malloc; devuelve bytes.
int  cam_snapshot_i420(unsigned char **out);
// Congela/descongela la vista (para "revisar la foto")
void cam_freeze(int on);

// Lector QR: se activa con cam_scan(1). Cuando encuentra texto que empieza con `prefix`
// lo deja en cam_qr_result() hasta que lo leas.
void cam_scan(int on, const char *prefix);
int  cam_qr_result(char *out, int cap);
// 0 sin datos, 1 buscando, 2 ve un QR pero borroso, 3 lee un QR que no es de VitaWA
int  cam_scan_hint(void);

#endif
