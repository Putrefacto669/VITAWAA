#pragma once
#include <stdbool.h>

// Reproductor basado en SceAvPlayer (decodificador H.264/AAC por hardware).
bool video_open(const char *path, bool loop);
void video_close(void);
bool video_active(void);         // true mientras haya algo cargado
bool video_ended(void);
void video_toggle_pause(void);
bool video_paused(void);
float video_progress(void);       // 0..1
float video_duration(void);       // segundos
float video_position(void);
// Dibuja el cuadro actual ajustado al rectángulo (letterbox).
void video_draw(float x, float y, float w, float h);
bool video_has_video(void);
