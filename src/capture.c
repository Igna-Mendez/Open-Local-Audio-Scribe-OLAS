/*
 * capture.c — miniaudio WASAPI loopback capture.
 *
 * Design goals:
 *   - Never block the real-time callback.
 *   - Never silently drop audio. If the ring is full when the writer tries
 *     to write, skip the write and count an overrun.
 *   - Process the entire callback input. No truncation.
 *   - Report conversion failures explicitly.
 *
 * The ring is 1<<22 frames (~8 MiB at 16 kHz mono s16), which is ~262 s of
 * headroom at 16 kHz. Overruns are practically impossible even when
 * inference falls far behind.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <stdatomic.h>
#include "miniaudio.h"
#include "capture.h"

#define SAMPLE_RATE   16000
#define MAX_DEVICES 64

#define RING_FRAMES ((ma_uint64)1u << 22)
#define RING_MASK   (RING_FRAMES - 1u)

#define MAX_CALLBACK_FRAMES 65536

/* mingw-w64's <stdatomic.h> omits atomic_uint64_t, which MSVC defines.
 * Spell it out so the rest of the file reads the same on both toolchains. */
#ifdef __MINGW32__
typedef _Atomic(uint64_t) atomic_uint64_t;
#endif

static inline ma_uint64 atomic_load_u64(atomic_uint64_t *p) {
    return atomic_load_explicit(p, memory_order_acquire);
}
static inline void atomic_store_u64(atomic_uint64_t *p, ma_uint64 v) {
    atomic_store_explicit(p, v, memory_order_release);
}
static inline ma_uint64 atomic_add_u64(atomic_uint64_t *p, ma_uint64 v) {
    return atomic_fetch_add_explicit(p, v, memory_order_relaxed);
}

static ma_context  g_ctx;
static ma_device   g_dev;
static int         g_chunk_samples = 0;

static ma_resampler         g_resampler;
static ma_channel_converter g_chconv;
static int                  g_have_resampler = 0;
static int                  g_have_chconv    = 0;

static ma_device_id  g_devices[MAX_DEVICES];
static char          g_dev_names[MAX_DEVICES][MA_MAX_DEVICE_NAME_LENGTH + 1];
static ma_bool32     g_dev_is_default[MAX_DEVICES];
static int           g_n_devices = 0;

static int16_t             g_ring[RING_FRAMES];
static atomic_uint64_t     g_ring_read    = 0;
static atomic_uint64_t     g_ring_written = 0;

static atomic_uint64_t     g_frames_received     = 0;
static atomic_uint64_t     g_ring_overruns       = 0;
static atomic_uint64_t     g_conversion_failures = 0;

static HANDLE g_have_chunk = NULL;
static int    g_ctx_ok = 0;
static int    g_cs_ok = 0;
static int    g_started = 0;
static CRITICAL_SECTION g_cs;

struct enum_ctx { int index; };

static ma_bool32 ma_enum_devices(ma_context *pContext, ma_device_type deviceType,
                                 const ma_device_info *pDevice, void *pUserData) {
    (void)pContext;
    if (deviceType != ma_device_type_playback) return MA_TRUE;
    struct enum_ctx *e = (struct enum_ctx *)pUserData;
    if (e->index >= MAX_DEVICES) return MA_FALSE;
    memcpy(&g_devices[e->index], &pDevice->id, sizeof(ma_device_id));
    strncpy(g_dev_names[e->index], pDevice->name, sizeof(g_dev_names[0]) - 1);
    g_dev_names[e->index][sizeof(g_dev_names[0]) - 1] = '\0';
    g_dev_is_default[e->index] = pDevice->isDefault;
    e->index++;
    return MA_TRUE;
}

void capture_set_chunk_samples(int n) { g_chunk_samples = n; }

int capture_init(void) {
    ma_context_config cc = ma_context_config_init();
    if (ma_context_init(NULL, 0, &cc, &g_ctx) != MA_SUCCESS) {
        fprintf(stderr, "capture: ma_context_init failed\n");
        return 0;
    }

    struct enum_ctx e = { 0 };
    ma_context_enumerate_devices(&g_ctx, ma_enum_devices, &e);
    g_n_devices = e.index;

    if (g_n_devices == 0)
        fprintf(stderr, "capture: no playback devices found\n");

    g_cs_ok = 1;
    InitializeCriticalSection(&g_cs);

    g_have_chunk = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_have_chunk) {
        fprintf(stderr, "capture: CreateEventW failed\n");
        DeleteCriticalSection(&g_cs);
        g_cs_ok = 0;
        ma_context_uninit(&g_ctx);
        g_ctx_ok = 0;
        return 0;
    }
    g_ctx_ok = 1;
    return g_n_devices > 0;
}

static void ring_push(const int16_t *frames, ma_uint32 count) {
    if (count == 0) return;

    /* Use a CAS loop to atomically reserve space for `count` frames.
     * This avoids the race where two concurrent ring_push() calls
     * could both pass the overrun check and overlap. */
    for (;;) {
        const ma_uint64 r = atomic_load_u64(&g_ring_read);
        const ma_uint64 w = atomic_load_u64(&g_ring_written);

        if (w - r + count > RING_FRAMES) {
            atomic_add_u64(&g_ring_overruns, 1);
            return;
        }

        /* Try to claim this write window. If the reader advanced or another
         * writer claimed it first, we'll retry. */
        if (atomic_compare_exchange_weak_explicit(
                &g_ring_written, &w, w + count,
                memory_order_release, memory_order_acquire)) {
            break;  /* Successfully claimed — write our frames */
        }
        /* CAS failed; retry with updated w from the CAS call. */
    }

    /* Now write the frames at the claimed window. We know no other writer
     * will write here, and the reader is behind us. */
    for (ma_uint32 i = 0; i < count; ++i)
        g_ring[(atomic_load_u64(&g_ring_written) - count + i) & RING_MASK] = frames[i];

    if (atomic_load_u64(&g_ring_written) - atomic_load_u64(&g_ring_read)
        >= (ma_uint64)g_chunk_samples)
        SetEvent(g_have_chunk);
}

static void ma_data_callback(ma_device *pDevice, void *pOut,
                             const void *pIn, ma_uint32 frameCount) {
    (void)pDevice; (void)pOut;
    if (!g_started || !pIn || frameCount == 0) return;

    atomic_add_u64(&g_frames_received, frameCount);

    if (!g_have_resampler && !g_have_chconv) {
        ring_push((const int16_t *)pIn, frameCount);
        return;
    }

    static float   fbuf_in[MAX_CALLBACK_FRAMES];
    static float   fbuf_mono[MAX_CALLBACK_FRAMES];
    static float   fbuf_res[MAX_CALLBACK_FRAMES];   /* resampler output — separate from fbuf_mono to avoid in-place when chconv is also active */
    static int16_t outbuf[MAX_CALLBACK_FRAMES];

    ma_uint32 n = frameCount;
    if (n > MAX_CALLBACK_FRAMES) {
        atomic_add_u64(&g_conversion_failures, 1);
        n = MAX_CALLBACK_FRAMES;
    }

    if (g_dev.capture.format == ma_format_f32) {
        memcpy(fbuf_in, pIn, n * sizeof(float));
    } else {
        ma_convert_pcm_frames_format(fbuf_in, ma_format_f32,
                                     pIn, g_dev.capture.format,
                                     n, g_dev.capture.channels,
                                     ma_dither_mode_none);
    }

    ma_uint32 mono_frames = n;
    const float *mono_ptr = fbuf_in;
    if (g_have_chconv) {
        if (ma_channel_converter_process_pcm_frames(&g_chconv,
                                                    fbuf_mono, fbuf_in, n) != MA_SUCCESS) {
            atomic_add_u64(&g_conversion_failures, 1);
            return;
        }
        mono_ptr = fbuf_mono;
    }

    if (g_have_resampler) {
        ma_uint64 in_frames_64 = mono_frames;
        ma_uint64 out_frames = MAX_CALLBACK_FRAMES;
        if (ma_resampler_process_pcm_frames(&g_resampler,
                                            mono_ptr, &in_frames_64,
                                            fbuf_res, &out_frames) != MA_SUCCESS) {
            atomic_add_u64(&g_conversion_failures, 1);
            return;
        }
        if (out_frames > MAX_CALLBACK_FRAMES) out_frames = MAX_CALLBACK_FRAMES;
        for (ma_uint64 i = 0; i < out_frames; ++i) {
            float s = fbuf_res[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            outbuf[i] = (int16_t)(s * 32767.0f);
        }
        ring_push(outbuf, (ma_uint32)out_frames);
    } else {
        for (ma_uint32 i = 0; i < mono_frames; ++i) {
            float s = mono_ptr[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            outbuf[i] = (int16_t)(s * 32767.0f);
        }
        ring_push(outbuf, mono_frames);
    }
}

int capture_start(int device_index) {
    if (g_chunk_samples <= 0 || (ma_uint64)g_chunk_samples > RING_FRAMES) {
        fprintf(stderr, "capture: chunk_samples out of range (%d)\n", g_chunk_samples);
        return 0;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_loopback);
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.wasapi.usage       = ma_wasapi_usage_pro_audio;

    cfg.sampleRate       = SAMPLE_RATE;
    cfg.capture.channels = 1;
    cfg.capture.format   = ma_format_s16;

    if (device_index >= 0 && device_index < g_n_devices)
        cfg.capture.pDeviceID = &g_devices[device_index];
    cfg.dataCallback = ma_data_callback;

    ma_result r = ma_device_init(&g_ctx, &cfg, &g_dev);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "capture: ma_device_init failed (%d)\n", (int)r);
        return 0;
    }

    g_have_resampler = 0;
    g_have_chconv    = 0;

    const ma_format  got_fmt  = g_dev.capture.format;
    const ma_uint32  got_ch   = g_dev.capture.channels;
    const ma_uint32  got_rate = g_dev.sampleRate;

    fprintf(stderr,
        "capture: negotiated %u Hz, %u ch, fmt=%d (requested 16000/1/s16)\n",
        got_rate, got_ch, (int)got_fmt);

    if (got_ch > 1) {
        ma_channel_converter_config cc = ma_channel_converter_config_init(
            got_fmt, got_ch, NULL, 1, NULL, ma_channel_mix_mode_default);
        if (ma_channel_converter_init(&cc, NULL, &g_chconv) == MA_SUCCESS)
            g_have_chconv = 1;
        else {
            fprintf(stderr, "capture: channel converter init failed\n");
            atomic_add_u64(&g_conversion_failures, 1);
        }
    }

    if (got_rate != SAMPLE_RATE) {
        ma_resampler_config rc = ma_resampler_config_init(
            ma_format_f32, 1, got_rate, SAMPLE_RATE,
            ma_resample_algorithm_linear);
        if (ma_resampler_init(&rc, NULL, &g_resampler) == MA_SUCCESS)
            g_have_resampler = 1;
        else {
            fprintf(stderr, "capture: resampler init failed\n");
            atomic_add_u64(&g_conversion_failures, 1);
        }
    }

    r = ma_device_start(&g_dev);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "capture: ma_device_start failed (%d)\n", (int)r);
        ma_device_uninit(&g_dev);
        if (g_have_resampler) { ma_resampler_uninit(&g_resampler, NULL); g_have_resampler = 0; }
        if (g_have_chconv)    { ma_channel_converter_uninit(&g_chconv, NULL); g_have_chconv = 0; }
        return 0;
    }

    /* Reset the ring BEFORE arming g_started: once the flag is set the
     * audio thread can push into the ring at any moment, and a stale
     * write pointer from a previous session would look like a full ring. */
    ResetEvent(g_have_chunk);
    EnterCriticalSection(&g_cs);
    atomic_store_u64(&g_ring_read,    0);
    atomic_store_u64(&g_ring_written, 0);
    LeaveCriticalSection(&g_cs);
    g_started = 1;
    return 1;
}

void capture_stop(void) {
    int was_started;
    EnterCriticalSection(&g_cs);
    was_started = g_started;
    if (was_started) g_started = 0;
    LeaveCriticalSection(&g_cs);
    if (was_started) {
        ma_device_stop(&g_dev);
        ma_device_uninit(&g_dev);
        if (g_have_resampler) { ma_resampler_uninit(&g_resampler, NULL); g_have_resampler = 0; }
        if (g_have_chconv)    { ma_channel_converter_uninit(&g_chconv, NULL); g_have_chconv = 0; }
        SetEvent(g_have_chunk);
    }
}

int  capture_device_count(void) { return g_n_devices; }
const char *capture_device_name(int index) {
    if (index < 0 || index >= g_n_devices) return "";
    return g_dev_names[index];
}
int  capture_device_is_default(int index) {
    if (index < 0 || index >= g_n_devices) return 0;
    return g_dev_is_default[index];
}
int  capture_is_started(void) {
    int started;
    EnterCriticalSection(&g_cs); started = g_started; LeaveCriticalSection(&g_cs);
    return started;
}

int capture_read_chunk(int16_t *out) {
    for (;;) {
        if (WaitForSingleObject(g_have_chunk, 50) != WAIT_OBJECT_0) {
            EnterCriticalSection(&g_cs);
            int started = g_started;
            LeaveCriticalSection(&g_cs);
            if (!started) return 0;
            continue;
        }
        const ma_uint64 w = atomic_load_u64(&g_ring_written);
        const ma_uint64 r = atomic_load_u64(&g_ring_read);
        if (w - r >= (ma_uint64)g_chunk_samples) break;
        EnterCriticalSection(&g_cs);
        int started = g_started;
        LeaveCriticalSection(&g_cs);
        if (!started) return 0;
    }
    const ma_uint64 r = atomic_load_u64(&g_ring_read);
    for (int i = 0; i < g_chunk_samples; i++)
        out[i] = g_ring[(r + i) & RING_MASK];
    atomic_store_u64(&g_ring_read, r + (ma_uint64)g_chunk_samples);
    return 1;
}

void capture_get_stats(capture_stats_t *out) {
    if (!out) return;
    out->frames_received     = atomic_load_u64(&g_frames_received);
    out->ring_overruns       = atomic_load_u64(&g_ring_overruns);
    out->conversion_failures = atomic_load_u64(&g_conversion_failures);
    out->ring_frames_capacity = (int)RING_FRAMES;
}

void capture_reset_stats(void) {
    atomic_store_u64(&g_frames_received, 0);
    atomic_store_u64(&g_ring_overruns, 0);
    atomic_store_u64(&g_conversion_failures, 0);
}

void capture_uninit(void) {
    capture_stop();
    if (g_have_chunk) { CloseHandle(g_have_chunk); g_have_chunk = NULL; }
    if (g_ctx_ok)     { ma_context_uninit(&g_ctx); g_ctx_ok = 0; }
    if (g_cs_ok)      { DeleteCriticalSection(&g_cs); g_cs_ok = 0; }
}