#include "audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <psp2/audioout.h>
#include <psp2/audioin.h>
#include <psp2/kernel/threadmgr.h>

#define OUT_RATE  48000
#define IN_RATE   16000
#define GRAIN     512
#define IN_GRAIN  256

static SceKernelLwMutexWork mtx;
static void lock(void)   { sceKernelLockLwMutex(&mtx, 1, NULL); }
static void unlock(void) { sceKernelUnlockLwMutex(&mtx, 1); }

static int sounds_on = 1;
static int out_port = -1;

// ---------- mu-law ----------
static short mulaw_table[256];
static void build_mulaw(void) {
    for (int i = 0; i < 256; i++) {
        int u = ~i & 0xFF;
        int sign = u & 0x80, exponent = (u >> 4) & 7, mantissa = u & 15;
        int s = ((mantissa << 3) + 0x84) << exponent;
        s -= 0x84;
        mulaw_table[i] = (short)(sign ? -s : s);
    }
}
static unsigned char lin_to_mulaw(int sample) {
    const int BIAS = 0x84, CLIP = 32635;
    int sign = (sample >> 8) & 0x80;
    if (sign) sample = -sample;
    if (sample > CLIP) sample = CLIP;
    sample += BIAS;
    int exponent = 7;
    for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; exponent--, mask >>= 1);
    int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return (unsigned char)(~(sign | (exponent << 4) | mantissa) & 0xFF);
}

// ---------- sonidos de interfaz (sintetizados) ----------
typedef struct { short *pcm; int len; } Clip;
static Clip clips[SFX_COUNT];

typedef struct { const Clip *clip; int pos; } Voice;
#define MAX_SFX 4
static Voice sfx[MAX_SFX];

// tono con envolvente suave; f1 -> f2 en `ms` milisegundos
static void synth(Clip *c, float f1, float f2, int ms, float vol, int shape) {
    c->len = OUT_RATE * ms / 1000;
    c->pcm = malloc(sizeof(short) * c->len);
    float phase = 0;
    for (int i = 0; i < c->len; i++) {
        float t = (float)i / c->len;
        float f = f1 + (f2 - f1) * t;
        phase += 2 * 3.14159265f * f / OUT_RATE;
        float env = (t < 0.08f ? t / 0.08f : 1.0f) * (1 - t) * (1 - t);
        float s = shape ? (sinf(phase) + 0.25f * sinf(2 * phase)) / 1.25f : sinf(phase);
        c->pcm[i] = (short)(s * env * vol * 32767);
    }
}
static void synth_two(Clip *c, float fa, float fb, int ms, float vol) {
    // dos notas seguidas (aviso de mensaje)
    Clip a, b;
    synth(&a, fa, fa, ms / 2, vol, 1);
    synth(&b, fb, fb, ms / 2, vol, 1);
    c->len = a.len + b.len;
    c->pcm = malloc(sizeof(short) * c->len);
    memcpy(c->pcm, a.pcm, sizeof(short) * a.len);
    memcpy(c->pcm + a.len, b.pcm, sizeof(short) * b.len);
    free(a.pcm);
    free(b.pcm);
}

static void build_clips(void) {
    synth(&clips[SFX_MOVE], 1800, 1800, 18, 0.10f, 0);
    synth(&clips[SFX_SELECT], 1100, 1500, 45, 0.16f, 1);
    synth(&clips[SFX_BACK], 900, 600, 50, 0.14f, 1);
    synth(&clips[SFX_SEND], 700, 1600, 120, 0.16f, 0);
    synth_two(&clips[SFX_RECEIVE], 880, 1320, 220, 0.18f);
    synth(&clips[SFX_ERROR], 300, 220, 160, 0.18f, 1);
    synth(&clips[SFX_REC_START], 1320, 1320, 90, 0.16f, 1);
    synth(&clips[SFX_REC_STOP], 990, 990, 90, 0.16f, 1);
}

void audio_set_sounds(int on) { sounds_on = on; }

void audio_sfx(int which) {
    if (!sounds_on || which < 0 || which >= SFX_COUNT || out_port < 0) return;
    lock();
    int slot = 0;
    for (int i = 0; i < MAX_SFX; i++) {
        if (!sfx[i].clip) { slot = i; break; }
        if (sfx[i].pos > sfx[slot].pos) slot = i;   // reemplaza el mas avanzado
    }
    sfx[slot].clip = &clips[which];
    sfx[slot].pos = 0;
    unlock();
}

// ---------- reproductor de notas de voz ----------
static short *voice_pcm = NULL;   // 16 kHz
static int voice_len = 0;
static double voice_pos = 0;       // en muestras de 16 kHz
static int voice_playing = 0;
static char voice_id[48];
static char loading_id[48];

void audio_play_mulaw(const char *id, const unsigned char *mulaw, int len) {
    short *pcm = malloc(sizeof(short) * (len > 0 ? len : 1));
    if (!pcm) return;
    for (int i = 0; i < len; i++) pcm[i] = mulaw_table[mulaw[i]];
    lock();
    free(voice_pcm);
    voice_pcm = pcm;
    voice_len = len;
    voice_pos = 0;
    voice_playing = 1;
    snprintf(voice_id, sizeof(voice_id), "%s", id);
    if (!strcmp(loading_id, id)) loading_id[0] = '\0';
    unlock();
}

void audio_toggle_pause(void) {
    lock();
    if (voice_pcm) {
        if (voice_pos >= voice_len) voice_pos = 0;
        voice_playing = !voice_playing;
    }
    unlock();
}

void audio_stop(void) {
    lock();
    voice_playing = 0;
    voice_pos = 0;
    unlock();
}

void audio_state(char *id, int id_cap, float *progress, float *total_s, int *playing) {
    lock();
    if (id) snprintf(id, id_cap, "%s", voice_pcm ? voice_id : "");
    if (progress) *progress = voice_len ? (float)(voice_pos / voice_len) : 0;
    if (total_s) *total_s = voice_len / (float)IN_RATE;
    if (playing) *playing = voice_playing;
    unlock();
}

void audio_set_loading(const char *id) {
    lock();
    snprintf(loading_id, sizeof(loading_id), "%s", id ? id : "");
    unlock();
}

int audio_is_loading(const char *id) {
    lock();
    int r = loading_id[0] && !strcmp(loading_id, id);
    unlock();
    return r;
}

// ---------- grabadora ----------
static short *rec_buf = NULL;
static int rec_len = 0, rec_cap = 0;
static volatile int rec_on = 0;
static volatile int rec_stop = 0;
static SceUID rec_thread = -1;
#define LEVELS 64
static float rec_levels[LEVELS];
static int rec_level_pos = 0;

static int rec_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    int port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, IN_GRAIN, IN_RATE, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (port < 0) {
        rec_on = 0;
        return sceKernelExitDeleteThread(0);
    }
    short block[IN_GRAIN];
    int acc_n = 0;
    float acc = 0;
    while (!rec_stop) {
        if (sceAudioInInput(port, block) < 0) break;
        lock();
        if (rec_len + IN_GRAIN <= rec_cap) {
            memcpy(rec_buf + rec_len, block, sizeof(block));
            rec_len += IN_GRAIN;
        } else {
            rec_stop = 1;   // llegamos al maximo
        }
        for (int i = 0; i < IN_GRAIN; i++) acc += (float)block[i] * block[i];
        acc_n += IN_GRAIN;
        if (acc_n >= IN_RATE / 20) {   // un nivel cada 50 ms
            float rms = sqrtf(acc / acc_n) / 8000.0f;
            rec_levels[rec_level_pos++ % LEVELS] = rms > 1 ? 1 : rms;
            acc = 0;
            acc_n = 0;
        }
        unlock();
    }
    sceAudioInReleasePort(port);
    rec_on = 0;
    return sceKernelExitDeleteThread(0);
}

int audio_rec_start(void) {
    if (rec_on) return -1;
    lock();
    voice_playing = 0;   // nada de sonar mientras grabas
    rec_cap = IN_RATE * REC_MAX_SECONDS;
    if (!rec_buf) rec_buf = malloc(sizeof(short) * rec_cap);
    rec_len = 0;
    rec_level_pos = 0;
    memset(rec_levels, 0, sizeof(rec_levels));
    unlock();
    if (!rec_buf) return -1;
    rec_stop = 0;
    rec_on = 1;
    rec_thread = sceKernelCreateThread("vitawa_rec", rec_main, 0x10000100, 0x4000, 0, 0, NULL);
    if (rec_thread < 0) {
        rec_on = 0;
        return -1;
    }
    sceKernelStartThread(rec_thread, 0, NULL);
    return 0;
}

static void rec_wait(void) {
    rec_stop = 1;
    for (int i = 0; i < 100 && rec_on; i++) sceKernelDelayThread(10 * 1000);
}

void audio_rec_cancel(void) {
    rec_wait();
    lock();
    rec_len = 0;
    unlock();
}

int audio_rec_finish(unsigned char **mulaw) {
    *mulaw = NULL;
    rec_wait();
    lock();
    int n = rec_len;
    if (n < IN_RATE / 2) {   // menos de medio segundo: no vale la pena
        rec_len = 0;
        unlock();
        return 0;
    }
    unsigned char *out = malloc(n);
    if (out) for (int i = 0; i < n; i++) out[i] = lin_to_mulaw(rec_buf[i]);
    rec_len = 0;
    unlock();
    *mulaw = out;
    return out ? n : 0;
}

int audio_rec_active(void) { return rec_on; }

float audio_rec_seconds(void) {
    lock();
    float s = rec_len / (float)IN_RATE;
    unlock();
    return s;
}

float audio_rec_level(void) {
    lock();
    float l = rec_level_pos ? rec_levels[(rec_level_pos - 1) % LEVELS] : 0;
    unlock();
    return l;
}

void audio_rec_levels(float *out, int n) {
    lock();
    for (int i = 0; i < n; i++) {
        int k = rec_level_pos - n + i;
        out[i] = k >= 0 ? rec_levels[k % LEVELS] : 0;
    }
    unlock();
}

// ---------- mezclador ----------
static int out_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    static short buf[GRAIN];
    static int mix[GRAIN];
    for (;;) {
        memset(mix, 0, sizeof(mix));
        lock();
        for (int v = 0; v < MAX_SFX; v++) {
            if (!sfx[v].clip) continue;
            const Clip *c = sfx[v].clip;
            for (int i = 0; i < GRAIN && sfx[v].pos < c->len; i++) mix[i] += c->pcm[sfx[v].pos++];
            if (sfx[v].pos >= c->len) sfx[v].clip = NULL;
        }
        if (voice_playing && voice_pcm && !rec_on) {
            // 16 kHz -> 48 kHz con interpolacion lineal
            double step = (double)IN_RATE / OUT_RATE;
            for (int i = 0; i < GRAIN; i++) {
                int k = (int)voice_pos;
                if (k + 1 >= voice_len) {
                    voice_playing = 0;
                    voice_pos = voice_len;
                    break;
                }
                double f = voice_pos - k;
                mix[i] += (int)(voice_pcm[k] * (1 - f) + voice_pcm[k + 1] * f);
                voice_pos += step;
            }
        }
        unlock();
        for (int i = 0; i < GRAIN; i++) buf[i] = (short)(mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i]);
        sceAudioOutOutput(out_port, buf);   // bloquea hasta que toca: marca el ritmo
    }
    return 0;
}

void audio_init(void) {
    sceKernelCreateLwMutex(&mtx, "vitawa_audio", 0, 0, NULL);
    build_mulaw();
    build_clips();
    out_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_MONO);
    if (out_port < 0) return;
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(out_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    SceUID th = sceKernelCreateThread("vitawa_audio", out_main, 0x10000100 - 10, 0x4000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
