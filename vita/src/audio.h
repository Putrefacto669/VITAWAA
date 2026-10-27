#ifndef VITAWA_AUDIO_H
#define VITAWA_AUDIO_H

// Audio de VitaWA:
//  - sonidos cortos de la interfaz (generados al arrancar, no son archivos)
//  - reproductor de notas de voz (mu-law 16 kHz que manda el bridge)
//  - grabadora con el microfono de la Vita (16 kHz mono)

enum { SFX_MOVE, SFX_SELECT, SFX_BACK, SFX_SEND, SFX_RECEIVE, SFX_ERROR, SFX_REC_START, SFX_REC_STOP, SFX_COUNT };

void audio_init(void);
void audio_sfx(int which);
void audio_set_sounds(int on);

// Notas de voz
void audio_play_mulaw(const char *id, const unsigned char *mulaw, int len);   // copia los datos
void audio_toggle_pause(void);
void audio_stop(void);
// id del audio cargado ("" si ninguno), progreso 0..1, segundos totales y si esta sonando
void audio_state(char *id, int id_cap, float *progress, float *total_s, int *playing);
// marca que se esta descargando este id (para dibujar "cargando")
void audio_set_loading(const char *id);
int  audio_is_loading(const char *id);

// Grabadora
int   audio_rec_start(void);                 // 0 si arranco
void  audio_rec_cancel(void);
// Termina y entrega mu-law 16 kHz (malloc). Devuelve los bytes, o 0 si fue muy corta.
int   audio_rec_finish(unsigned char **mulaw);
int   audio_rec_active(void);
float audio_rec_seconds(void);
float audio_rec_level(void);                 // 0..1 nivel reciente (para la onda)
void  audio_rec_levels(float *out, int n);   // ultimos n niveles

#define REC_MAX_SECONDS 120

#endif
