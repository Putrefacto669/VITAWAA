#ifndef VITAWA_IME_H
#define VITAWA_IME_H

// Teclado en pantalla de la Vita (SceImeDialog), sin bloquear el render.
// Uso: ime_open(...) una vez; luego en cada frame, despues de dibujar,
// vita2d_common_dialog_update() y ime_poll(...) hasta que devuelva != 0.

enum { IME_KIND_TEXT = 0, IME_KIND_URL = 1, IME_KIND_NUMBER = 2 };

int  ime_open(const char *title, const char *initial, int kind, int max_len);
int  ime_is_open(void);
// 0 = sigue abierto, 1 = aceptado (texto en out), -1 = cancelado
int  ime_poll(char *out, int out_cap);

#endif
