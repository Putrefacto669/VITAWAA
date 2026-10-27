// ═════════════════════════════════════════════════════════════════════════════
//  video.c — Video y notas de voz con SceAvPlayer.
//
//  El truco clave: AvPlayer entrega cada cuadro en formato YVU420P2 dentro de
//  memoria que NOSOTROS le damos (allocateTexture). Si esa memoria está mapeada
//  para la GPU, envolvemos el puntero en una SceGxmTexture con CSC (conversión
//  de color por hardware) y vita2d la dibuja como cualquier textura: cero copias,
//  cero conversión YUV→RGB en CPU.
//
//  El audio se saca en un hilo aparte hacia SceAudioOut (48 kHz estéreo — el
//  bridge transcodifica todo a ese formato).
// ═════════════════════════════════════════════════════════════════════════════
#include "video.h"
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define MAX_FRAME_BLOCKS 16

static SceAvPlayerHandle player = -1;
static vita2d_texture frameTex;
static bool hasFrame = false;
static bool paused = false;
static bool ended = false;
static bool looping = false;
static bool sawVideo = false;
static int vw = 0, vh = 0;
static uint64_t durationMs = 0;

static int audioPort = -1;
static pthread_t audioThread;
static volatile bool audioRun = false;

typedef struct { void *ptr; SceUID uid; } Block;
static Block blocks[MAX_FRAME_BLOCKS];

// ── memoria genérica ─────────────────────────────────────────────────────────
static void *av_alloc(void *arg, uint32_t align, uint32_t size) { (void)arg; return memalign(align, size); }
static void av_free(void *arg, void *p) { (void)arg; free(p); }

// ── memoria de cuadros: CDRAM mapeada a la GPU ───────────────────────────────
static void *av_alloc_frame(void *arg, uint32_t align, uint32_t size) {
	(void)arg; (void)align;
	size = ALIGN(size, 256 * 1024);
	SceUID uid = sceKernelAllocMemBlock("vwa_frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
	if (uid < 0) return NULL;
	void *p = NULL;
	sceKernelGetMemBlockBase(uid, &p);
	if (sceGxmMapMemory(p, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
		sceKernelFreeMemBlock(uid);
		return NULL;
	}
	for (int i = 0; i < MAX_FRAME_BLOCKS; i++)
		if (!blocks[i].ptr) { blocks[i].ptr = p; blocks[i].uid = uid; return p; }
	sceGxmUnmapMemory(p);
	sceKernelFreeMemBlock(uid);
	return NULL;
}

static void av_free_frame(void *arg, void *p) {
	(void)arg;
	for (int i = 0; i < MAX_FRAME_BLOCKS; i++)
		if (blocks[i].ptr == p) {
			sceGxmUnmapMemory(p);
			sceKernelFreeMemBlock(blocks[i].uid);
			blocks[i].ptr = NULL;
			return;
		}
}

// ── audio ────────────────────────────────────────────────────────────────────
static void *audio_loop(void *arg) {
	(void)arg;
	int curRate = 48000, curMode = SCE_AUDIO_OUT_MODE_STEREO;
	while (audioRun) {
		if (player < 0 || paused || audioPort < 0) { sceKernelDelayThread(5000); continue; }
		SceAvPlayerFrameInfo fi;
		memset(&fi, 0, sizeof fi);
		if (sceAvPlayerGetAudioData(player, &fi)) {
			int mode = fi.details.audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
			int rate = fi.details.audio.sampleRate ? (int)fi.details.audio.sampleRate : 48000;
			if (rate != curRate || mode != curMode) {
				sceAudioOutSetConfig(audioPort, -1, rate, mode);
				curRate = rate; curMode = mode;
			}
			sceAudioOutOutput(audioPort, fi.pData); // bloquea al ritmo del DAC → sincroniza solo
		} else {
			sceKernelDelayThread(2000);
		}
	}
	return NULL;
}

// ── API ──────────────────────────────────────────────────────────────────────
bool video_open(const char *path, bool loop) {
	video_close();
	static bool modLoaded = false;
	if (!modLoaded) { sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER); modLoaded = true; }

	SceAvPlayerInitData init;
	memset(&init, 0, sizeof init);
	init.memoryReplacement.allocate = av_alloc;
	init.memoryReplacement.deallocate = av_free;
	init.memoryReplacement.allocateTexture = av_alloc_frame;
	init.memoryReplacement.deallocateTexture = av_free_frame;
	init.basePriority = 0xA0;
	init.numOutputVideoFrameBuffers = 2;
	init.autoStart = SCE_TRUE;
	init.debugLevel = 0;

	player = sceAvPlayerInit(&init);
	if (player < 0) { player = -1; return false; }
	if (sceAvPlayerAddSource(player, path) < 0) { video_close(); return false; }
	looping = loop;
	sceAvPlayerSetLooping(player, loop ? SCE_TRUE : SCE_FALSE);

	paused = ended = hasFrame = sawVideo = false;
	vw = vh = 0;
	durationMs = 0;

	audioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
	if (audioPort < 0) audioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
	audioRun = true;
	pthread_create(&audioThread, NULL, audio_loop, NULL);
	return true;
}

void video_close(void) {
	if (player < 0) return;
	audioRun = false;
	pthread_join(audioThread, NULL);
	if (audioPort >= 0) { sceAudioOutReleasePort(audioPort); audioPort = -1; }
	vita2d_wait_rendering_done(); // la GPU puede seguir leyendo el último cuadro
	sceAvPlayerStop(player);
	sceAvPlayerClose(player);
	player = -1;
	hasFrame = false;
}

bool video_active(void) { return player >= 0; }

bool video_ended(void) {
	if (player < 0) return true;
	if (!looping && !sceAvPlayerIsActive(player)) ended = true;
	return ended;
}

void video_toggle_pause(void) {
	if (player < 0) return;
	if (paused) sceAvPlayerResume(player); else sceAvPlayerPause(player);
	paused = !paused;
}
bool video_paused(void) { return paused; }

float video_duration(void) {
	if (player < 0) return 0;
	if (!durationMs) {
		SceAvPlayerStreamInfo si;
		for (uint32_t i = 0; i < 2; i++)
			if (sceAvPlayerGetStreamInfo(player, i, &si) == 0 && si.duration > durationMs) durationMs = si.duration;
	}
	return durationMs / 1000.0f;
}
float video_position(void) { return player >= 0 ? sceAvPlayerCurrentTime(player) / 1000.0f : 0; }
float video_progress(void) {
	float d = video_duration();
	return d > 0 ? video_position() / d : 0;
}
bool video_has_video(void) { return sawVideo; }

void video_draw(float x, float y, float w, float h) {
	if (player < 0) return;
	SceAvPlayerFrameInfo fi;
	memset(&fi, 0, sizeof fi);
	if (!paused && sceAvPlayerGetVideoData(player, &fi)) {
		vw = fi.details.video.width;
		vh = fi.details.video.height;
		// Envolver el cuadro (YVU420 planar, 2 planos) en una textura GXM con CSC por hardware
		sceGxmTextureInitLinear(&frameTex.gxm_tex, fi.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, vw, vh, 0);
		sceGxmTextureSetMinFilter(&frameTex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
		sceGxmTextureSetMagFilter(&frameTex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
		hasFrame = true;
		sawVideo = true;
	}
	if (!hasFrame || vw == 0) return;
	float sc = w / vw;
	if (vh * sc > h) sc = h / vh;
	float dw = vw * sc, dh = vh * sc;
	vita2d_draw_texture_scale(&frameTex, x + (w - dw) / 2, y + (h - dh) / 2, sc, sc);
}
