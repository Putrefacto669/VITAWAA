#ifndef VITAWA_NET_H
#define VITAWA_NET_H

// HTTP(S) sobre libcurl (antes SceHttp, que no sabe de certificados modernos).
// `slot` (opcional) guarda la peticion en curso para poder cortarla desde otro
// hilo con net_abort(slot). Usa un slot distinto por hilo.

int  net_init(void);
void net_term(void);

// POST binario. Reserva *out (malloc, termina en '\0') segun lo que llegue, hasta max_len.
// Devuelve el codigo HTTP o < 0 si fallo la red.
int  net_post(const char *url, const void *body, int body_len, char **out, int *out_len,
              int max_len, unsigned int recv_timeout_s, volatile int *slot);

void net_abort(volatile int *slot);
void net_reset(void);   // tras salir de la suspensión

// Escapa un valor para ponerlo en la query (?jid=...)
void url_enc(const char *in, char *out, int cap);

#endif
