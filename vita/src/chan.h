#ifndef VITAWA_CHAN_H
#define VITAWA_CHAN_H

// Canal cifrado con el bridge (XChaCha20-Poly1305 con Monocypher).
// Todas las peticiones van como POST <url>/x con la ruta real adentro.

enum {
    CHAN_ERR_NET   = -1,   // no hay conexion con el bridge
    CHAN_ERR_KEY   = -2,   // el bridge no reconoce la llave (hay que emparejar otra vez)
    CHAN_ERR_DATA  = -3,   // respuesta corrupta
    CHAN_ERR_NOCFG = -4,   // la Vita no esta emparejada
};

// Configura a que bridge hablar. key = 32 bytes.
void chan_set(const char *url, const unsigned char *key);
int  chan_ready(void);

// Hace una peticion. *out (malloc, termina en '\0') trae el cuerpo de la respuesta.
// Devuelve el codigo de la ruta (200, 404, 409...) o un CHAN_ERR_*.
int  chan_call(const char *method, const char *path, const void *body, int body_len,
               char **out, int *out_len, int max_out, unsigned int timeout_s, volatile int *slot);

// Decodifica el texto del QR de emparejamiento: "VWA1 <url> <llave base64url>"
int  chan_parse_pairing(const char *text, char *url, int url_cap, unsigned char key[32]);
void b64url_encode(const unsigned char *in, int len, char *out, int cap);
int  b64url_decode(const char *in, unsigned char *out, int cap);

// Hora unix en ms (UTC) y diferencia de la zona horaria de la Vita en minutos
long long unix_ms(void);
int  tz_minutes(void);

#endif
