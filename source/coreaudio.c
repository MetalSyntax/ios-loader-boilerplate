// AudioUnit RemoteIO + AudioSession de iOS sobre sceAudioOut.
//
// FMOD Ex (estático en el juego) mezcla todo y solo necesita una salida: crea un RemoteIO, le
// pone formato y render callback, y lo arranca. Acá un hilo pide bloques al callback (con
// call_guest6: es código del juego) y los manda a un puerto BGM de la Vita.
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>

#include "imports.h"
#include "log.h"
#include "macho.h"

#define kAudioUnitProperty_StreamFormat 8
#define kAudioUnitProperty_SetRenderCallback 23
#define kAudioFormatFlagIsFloat (1 << 0)
#define kAudioFormatFlagIsNonInterleaved (1 << 5)
#define kAudioFormatFlagsFixed824 (24 << 7) // kLinearPCMFormatFlagsSampleFractionShift
#define FOURCC(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))

#define GRAIN 512

typedef struct {
    double mSampleRate;
    uint32_t mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame,
        mChannelsPerFrame, mBitsPerChannel, mReserved;
} asbd;

typedef struct {
    uint32_t mNumberBuffers;
    struct {
        uint32_t mNumberChannels, mDataByteSize;
        void *mData;
    } mBuffers[2];
} buffer_list;

typedef struct {
    asbd fmt;
    void *proc, *refcon;
    SceUID thread;
    volatile int running;
} audio_unit;

static int ok(void) { return 0; }

static int au_FindNext(void *prev, const void *desc) { return prev ? 0 : 1; } // un solo componente

static int au_InstanceNew(void *comp, audio_unit **out) {
    audio_unit *u = calloc(1, sizeof(*u));
    // Default de RemoteIO: 44,1 kHz, s16 estéreo intercalado.
    u->fmt = (asbd){44100, FOURCC('l', 'p', 'c', 'm'), 0xC, 4, 1, 4, 2, 16, 0};
    *out = u;
    return 0;
}

static int au_SetProperty(audio_unit *u, uint32_t id, uint32_t scope, uint32_t elem, const void *data, uint32_t size) {
    if (id == kAudioUnitProperty_StreamFormat) {
        memcpy(&u->fmt, data, sizeof(asbd));
        l_info("AudioUnit: %d Hz, %u canales, %u bits, flags 0x%X", (int)u->fmt.mSampleRate,
               (unsigned)u->fmt.mChannelsPerFrame, (unsigned)u->fmt.mBitsPerChannel, (unsigned)u->fmt.mFormatFlags);
    } else if (id == kAudioUnitProperty_SetRenderCallback) {
        u->proc = ((void *const *)data)[0];
        u->refcon = ((void *const *)data)[1];
    }
    return 0;
}

static int au_GetProperty(audio_unit *u, uint32_t id, uint32_t scope, uint32_t elem, void *data, uint32_t *size) {
    if (id == kAudioUnitProperty_StreamFormat) {
        memcpy(data, &u->fmt, sizeof(asbd));
        *size = sizeof(asbd);
        return 0;
    }
    memset(data, 0, *size);
    return 0;
}

static int16_t to_s16(const audio_unit *u, const uint8_t *p) {
    uint32_t fl = u->fmt.mFormatFlags;
    if (fl & kAudioFormatFlagIsFloat) {
        float f = *(const float *)p * 32767.0f;
        return f > 32767 ? 32767 : f < -32768 ? -32768 : (int16_t)f;
    }
    if (u->fmt.mBitsPerChannel == 32) { // 8.24 (canónico de AudioUnit) o s32
        int32_t v = *(const int32_t *)p >> ((fl & kAudioFormatFlagsFixed824) ? 9 : 16);
        return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    return *(const int16_t *)p;
}

static int audio_thread(SceSize args, void *argp) {
    audio_unit *u = *(audio_unit **)argp;
    int rate = (int)u->fmt.mSampleRate, ch = u->fmt.mChannelsPerFrame;
    int bytes = u->fmt.mBitsPerChannel / 8;
    int nonint = (u->fmt.mFormatFlags & kAudioFormatFlagIsNonInterleaved) && ch == 2;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, rate, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        l_error("sceAudioOutOpenPort(%d Hz) = 0x%08X", rate, port);
        return 0;
    }
    uint8_t *raw = malloc(GRAIN * 2 * 4);
    int16_t out[GRAIN * 2];
    uint8_t ts[64] = {0}; // AudioTimeStamp
    uint32_t flags = 0;
    double sample_time = 0;
    buffer_list bl;
    while (u->running) {
        if (nonint)
            bl = (buffer_list){2, {{1, GRAIN * bytes, raw}, {1, GRAIN * bytes, raw + GRAIN * 4}}};
        else
            bl = (buffer_list){1, {{ch, GRAIN * ch * bytes, raw}}};
        memcpy(ts, &sample_time, 8);
        ((uint32_t *)ts)[14] = 1; // mFlags = kAudioTimeStampSampleTimeValid
        flags = 0;
        call_guest6(u->proc, (uint32_t)u->refcon, (uint32_t)&flags, (uint32_t)ts, 0, GRAIN, (uint32_t)&bl);
        sample_time += GRAIN;
        for (int i = 0; i < GRAIN; i++) {
            if (nonint) {
                out[i * 2] = to_s16(u, raw + i * bytes);
                out[i * 2 + 1] = to_s16(u, raw + GRAIN * 4 + i * bytes);
            } else {
                out[i * 2] = to_s16(u, raw + i * ch * bytes);
                out[i * 2 + 1] = ch == 2 ? to_s16(u, raw + (i * 2 + 1) * bytes) : out[i * 2];
            }
        }
        sceAudioOutOutput(port, out);
    }
    sceAudioOutReleasePort(port);
    free(raw);
    return 0;
}

static int au_Start(audio_unit *u) {
    if (u->running || !u->proc)
        return 0;
    u->running = 1;
    u->thread = sceKernelCreateThread("coreaudio", audio_thread, 0x10000100, 256 * 1024, 0, 0, NULL);
    sceKernelStartThread(u->thread, sizeof(u), &u);
    return 0;
}

static int au_Stop(audio_unit *u) {
    if (u->running) {
        u->running = 0;
        sceKernelWaitThreadEnd(u->thread, NULL, NULL);
        sceKernelDeleteThread(u->thread);
    }
    return 0;
}

static int au_Dispose(audio_unit *u) {
    au_Stop(u);
    free(u);
    return 0;
}

static int au_Render(void) { return -1; } // entrada (micrófono): no hay

// AudioSession: lo que FMOD consulta para elegir formato.
static int as_GetProperty(uint32_t id, uint32_t *size, void *data) {
    memset(data, 0, *size);
    switch (id) {
    case FOURCC('c', 'h', 's', 'r'): // CurrentHardwareSampleRate (Float64)
        *(double *)data = 44100.0;
        break;
    case FOURCC('c', 'h', 'o', 'c'): // CurrentHardwareOutputNumberChannels
        *(uint32_t *)data = 2;
        break;
    case FOURCC('c', 'h', 'b', 'd'): // CurrentHardwareIOBufferDuration (Float32)
        *(float *)data = (float)GRAIN / 44100.0f;
        break;
    case FOURCC('c', 'h', 'o', 'l'): // CurrentHardwareOutputLatency (Float32)
        *(float *)data = 0.012f;
        break;
    default:
        l_debug("AudioSessionGetProperty('%c%c%c%c') -> 0", id >> 24, (id >> 16) & 0xFF, (id >> 8) & 0xFF, id & 0xFF);
    }
    return 0;
}

#define F(name, fn) {"_" #name, (uintptr_t)(fn)}

const import_entry coreaudio_imports[] = {
    F(AudioComponentFindNext, au_FindNext), F(AudioComponentInstanceNew, au_InstanceNew),
    F(AudioComponentInstanceDispose, au_Dispose), F(AudioUnitSetProperty, au_SetProperty),
    F(AudioUnitGetProperty, au_GetProperty), F(AudioUnitInitialize, ok), F(AudioUnitUninitialize, ok),
    F(AudioOutputUnitStart, au_Start), F(AudioOutputUnitStop, au_Stop), F(AudioUnitRender, au_Render),
    F(AudioSessionInitialize, ok), F(AudioSessionSetActive, ok), F(AudioSessionSetProperty, ok),
    F(AudioSessionGetProperty, as_GetProperty), F(AudioSessionAddPropertyListener, ok),
    // llega por __nl_symbol_ptr (puntero a función): necesita código, no un bloque de datos
    F(AudioSessionRemovePropertyListenerWithUserData, ok),
};
const int coreaudio_imports_count = sizeof(coreaudio_imports) / sizeof(coreaudio_imports[0]);
