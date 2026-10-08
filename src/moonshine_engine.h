#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  pragma push_macro("ERROR")
#  pragma push_macro("DELETE")
#  pragma push_macro("IN")
#  pragma push_macro("OUT")
#  pragma push_macro("OPTIONAL")
#  pragma push_macro("small")
#  pragma push_macro("near")
#  pragma push_macro("far")
#  pragma push_macro("interface")
#  pragma push_macro("GetObject")
#  pragma push_macro("CreateFile")
#  pragma push_macro("LoadImage")
#  undef ERROR
#  undef DELETE
#  undef IN
#  undef OUT
#  undef OPTIONAL
#  undef small
#  undef near
#  undef far
#  undef interface
#  undef GetObject
#  undef CreateFile
#  undef LoadImage
#endif

#include "moonshine-cpp.h"

#ifdef _WIN32
#  pragma pop_macro("ERROR")
#  pragma pop_macro("DELETE")
#  pragma pop_macro("IN")
#  pragma pop_macro("OUT")
#  pragma pop_macro("OPTIONAL")
#  pragma pop_macro("small")
#  pragma pop_macro("near")
#  pragma pop_macro("far")
#  pragma pop_macro("interface")
#  pragma pop_macro("GetObject")
#  pragma pop_macro("CreateFile")
#  pragma pop_macro("LoadImage")
#endif

#include "transcript_sink.h"

namespace olas {

// ---------- config (mirrors olas-gtk.cpp) ----------
constexpr int SAMPLE_RATE          = 16000;
constexpr int VAD_FRAME_MS         = 20;
constexpr int VAD_FRAME_SAMPLES    = SAMPLE_RATE / 1000 * VAD_FRAME_MS;
constexpr int VAD_HANGOVER_MS      = 350;
constexpr int VAD_PREROLL_MS       = 300;

constexpr int PARTIAL_INTERVAL_MS       = 500;
constexpr int PARTIAL_INTERVAL_SAMPLES  = SAMPLE_RATE / 1000 * PARTIAL_INTERVAL_MS;
constexpr int MIN_PARTIAL_AUDIO_MS      = 300;
constexpr int MAX_PARTIAL_WINDOW_MS     = 2500;
constexpr int MAX_PARTIAL_WINDOW_SAMPLES= SAMPLE_RATE / 1000 * MAX_PARTIAL_WINDOW_MS;

constexpr int MAX_SEGMENT_MS       = 8000;
constexpr int MAX_SEGMENT_SAMPLES  = SAMPLE_RATE / 1000 * MAX_SEGMENT_MS;
constexpr size_t MAX_FINAL_BACKLOG = 16;

constexpr int DEFAULT_CAPTURE_CHUNK_MS = 50;

/* Audio backlog limits, not discard points: past the soft limit the backlog
 * is reported but nothing is dropped. The hard limit is a memory guard. */
constexpr size_t SOFT_BACKLOG_WARN_CHUNKS = 200;   // ~10 s of 50 ms chunks
constexpr size_t MAX_QUEUE_CHUNKS = 1200;          // ~60 s, guard only

constexpr int ARCH_TINY             = 0;
constexpr int ARCH_BASE             = 1;
constexpr int ARCH_TINY_STREAMING   = 2;
constexpr int ARCH_BASE_STREAMING   = 3;
constexpr int ARCH_SMALL_STREAMING  = 4;
constexpr int ARCH_MEDIUM_STREAMING = 5;

constexpr double DEF_SILENCE_RMS    = 100.0;
constexpr const char *NOTES_FILE    = "olas-moonshine-notes.txt";
constexpr const char *VERBOSE_LOG_FILE = "olas-debug.log";

struct LanguageConfig {
    std::string language;
    std::string model_path;
    int         arch = ARCH_SMALL_STREAMING;
    bool        spelling = false;
};

inline bool  g_verbose     = false;
inline FILE *g_verbose_log = nullptr;

/* Runtime tuning, loaded from olas-win.conf. Defaults are Moonshine's
 * documented values. See olas-win.conf for documented keys.
 * Domain dictionary terms are loaded from contexts/active-<lang>.txt, not
 * from the config file. */
struct OlasConfig {
    // Moonshine documented defaults, except transcription_interval: the
    // dominant CPU lever, and it does not change the transcript (measured on
    // Linux: 44 s of audio took 69 s of wall clock at 0.5 vs 58 s at 2.0, for
    // identical output). 1.0 is ~a third less work than 0.5.
    double vad_threshold = 0.5;
    int    vad_max_segment_duration = 15;
    double transcription_interval = 1.0;

    static std::string default_path() {
        if (const char *e = std::getenv("OLAS_CONFIG")) return e;
        return "olas-win.conf";
    }

    // Tolerant parser: unknown keys and bad values produce warnings, never
    // a hard failure, so a typo in the config cannot stop the app starting.
    static OlasConfig load(const std::string &path,
                           std::vector<std::string> &warnings);
};

/* Set once by main() before Engine is constructed; read by the workers. */
inline OlasConfig g_ocfg;

/* Written only by the slot's worker thread, read after join(). Plain
 * integers — no atomicity needed because the join() synchronizes. */
struct SlotCounters {
    uint64_t chunks_pushed{0};
    uint64_t chunks_processed{0};
    uint64_t queue_depth{0};
    uint64_t queue_max_depth{0};
    uint64_t final_jobs_posted{0};
    uint64_t final_jobs_dropped{0};
    uint64_t inference_ns_total{0};
    uint64_t audio_frames_total{0};
};

bool is_streaming_arch(int a);
bool valid_arch(int a);
bool is_supported_language(const std::string &lang);
std::vector<std::string> split_csv(const std::string &s);
bool parse_int(const char *s, int &out);
bool parse_double(const char *s, double &out);

class Engine {
public:
    Engine(const std::vector<LanguageConfig> &configs,
           TranscriptSink                     *sink,
           FILE                               *notes,
           double                              silence_rms,
           std::chrono::system_clock::time_point session_start);
    ~Engine();

    Engine(const Engine &)            = delete;
    Engine &operator=(const Engine &) = delete;

    void start();
    void stop();
    void flush();

    void feed(std::shared_ptr<const std::vector<int16_t>> pcm);

    void toggle(int slot);
    bool is_enabled(int slot) const;
    size_t size() const;

    struct Stats {
        uint64_t chunks_pushed;
        uint64_t chunks_processed;
        uint64_t queue_depth;
        uint64_t queue_max_depth;
        uint64_t final_jobs_posted;
        uint64_t final_jobs_dropped;
        double   inference_rtf;
    };
    Stats get_stats(int slot) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace olas