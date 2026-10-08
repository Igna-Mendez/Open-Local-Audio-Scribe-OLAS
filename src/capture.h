/*
 * capture — miniaudio WASAPI loopback capture for OLAS.
 *
 * Opens the *render* endpoint of the selected output device in WASAPI
 * loopback mode (ma_device_type_loopback) and delivers 16 kHz mono s16
 * frames (miniaudio resamples and downmixes) into a chunk-sized ring.
 *
 * The ring is loss-detecting: if the writer would overwrite unread frames,
 * the write is skipped and an atomic overrun counter is incremented. The
 * reader can query the counter via capture_get_stats(). Capture never
 * blocks waiting for the reader.
 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
    #endif

    /* Opaque snapshot of capture-level counters. All values are cumulative
     * since capture_init(). */
    typedef struct {
        uint64_t frames_received;
        uint64_t ring_overruns;
        uint64_t conversion_failures;
        int      ring_frames_capacity;
    } capture_stats_t;

    int  capture_init(void);
    void capture_set_chunk_samples(int n);
    int  capture_start(int device_index);
    void capture_stop(void);
    void capture_uninit(void);

    int  capture_device_count(void);
    const char *capture_device_name(int index);
    int  capture_device_is_default(int index);

    int  capture_read_chunk(int16_t *out);
    int  capture_read_available(void);
    int  capture_is_started(void);

    void capture_get_stats(capture_stats_t *out);
    void capture_reset_stats(void);

    #ifdef __cplusplus
}
#endif

#endif /* CAPTURE_H */