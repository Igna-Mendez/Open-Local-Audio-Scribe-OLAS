/* Windows.h must come first so Moonshine's own headers see a clean macro
 * namespace. */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
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

#include "moonshine_engine.h"
#include "resource_plan.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace olas {

static std::mutex g_log_mutex;

/* OlasConfig: load transcription parameters from olas-win.conf.
 *
 * Tolerant parser: unknown keys and bad values warn, never fail, so a typo
 * cannot stop the app from starting. */

OlasConfig OlasConfig::load(const std::string &path,
                            std::vector<std::string> &warnings) {
    OlasConfig c;
    std::ifstream f(path);
    if (!f) return c;

    int lineno = 0;
    std::string line;

    auto trim = [](const std::string &s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };
    auto lower = [](std::string s) {
        for (char &ch : s) ch = (char)std::tolower((unsigned char)ch);
        return s;
    };

    while (std::getline(f, line)) {
        ++lineno;
        std::string l = trim(line);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        const size_t hash = l.find(" #");
        if (hash != std::string::npos) l = trim(l.substr(0, hash));

        if (l[0] == '[') {
            const size_t close = l.find(']');
            if (close == std::string::npos) {
                warnings.push_back("line " + std::to_string(lineno) +
                                   ": malformed section header");
                continue;
            }
            const std::string sec = lower(trim(l.substr(1, close - 1)));
            if (sec.rfind("bias", 0) == 0 ||
                sec.rfind("dictionary", 0) == 0 ||
                sec.rfind("terms", 0) == 0) {
                warnings.push_back(
                    "line " + std::to_string(lineno) + ": [" + sec +
                    "] ignored — use contexts/active-<lang>.txt instead.");
            }
            continue;
        }

        const size_t eq = l.find('=');
        if (eq == std::string::npos) {
            warnings.push_back("line " + std::to_string(lineno) +
                               ": expected key = value");
            continue;
        }
        const std::string key = lower(trim(l.substr(0, eq)));
        const std::string val = trim(l.substr(eq + 1));

        auto to_d = [&](double &out) {
            try { size_t u = 0; out = std::stod(val, &u);
                  return u == val.size(); }
            catch (...) { return false; }
        };
        auto to_i = [&](int &out) {
            try { size_t u = 0; out = std::stoi(val, &u);
                  return u == val.size(); }
            catch (...) { return false; }
        };

        if (key == "vad_threshold") {
            double d = 0.0;
            if (to_d(d) && d >= 0.0 && d <= 1.0) c.vad_threshold = d;
            else warnings.push_back("line " + std::to_string(lineno) +
                                    ": vad_threshold must be 0.0..1.0");
        } else if (key == "vad_max_segment_duration" || key == "max_segment") {
            int i = 0;
            if (to_i(i) && i >= 1 && i <= 30) c.vad_max_segment_duration = i;
            else warnings.push_back("line " + std::to_string(lineno) +
                                    ": vad_max_segment_duration must be 1..30");
        } else if (key == "transcription_interval") {
            double d = 0.0;
            if (to_d(d) && d >= 0.1 && d <= 5.0) c.transcription_interval = d;
            else warnings.push_back("line " + std::to_string(lineno) +
                                    ": transcription_interval must be 0.1..5.0");
        } else if (key == "max_keyterms" || key == "keyterms" ||
                   key == "context" || key == "boost" ||
                   key == "keyterm_boost" || key == "include" ||
                   key == "import" || key == "dictionary") {
            warnings.push_back(
                "line " + std::to_string(lineno) + ": '" + key +
                "' ignored — use contexts/active-<lang>.txt instead.");
        } else {
            warnings.push_back("line " + std::to_string(lineno) +
                               ": unknown key '" + key + "' (ignored)");
        }
    }
    return c;
}

static void print_error(const char *prefix, const char *detail) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    std::fprintf(stderr, "%s%s\n", prefix, detail ? detail : "unknown error");
    std::fflush(stderr);
}

static void verbose_logf(const char *fmt, ...) {
    if (!g_verbose || !g_verbose_log) return;
    std::lock_guard<std::mutex> lk(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_verbose_log, fmt, ap);
    va_end(ap);
    std::fflush(g_verbose_log);
}

// ---------- helpers ----------

std::vector<std::string> split_csv(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

bool parse_int(const char *s, int &out) {
    if (!s || !*s) return false;
    char *end = nullptr;
    errno = 0;
    long v = std::strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0' ||
        v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
}

bool parse_double(const char *s, double &out) {
    if (!s || !*s) return false;
    char *end = nullptr;
    errno = 0;
    double v = std::strtod(s, &end);
    if (errno == ERANGE || end == s || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool is_streaming_arch(int a) {
    return a >= ARCH_TINY_STREAMING && a <= ARCH_MEDIUM_STREAMING;
}
bool valid_arch(int a) {
    if (a == ARCH_BASE_STREAMING) return false;
    return a >= ARCH_TINY && a <= ARCH_MEDIUM_STREAMING;
}

bool is_supported_language(const std::string &lang) {
#ifdef OLAS_MOONSHINE_EXTENDED_LANGUAGES
    static const std::vector<std::string> s = {
        "en","es","ar","ja","ko","zh","vi","uk"
    };
#else
    static const std::vector<std::string> s = { "en", "es" };
#endif
    for (const auto &x : s) if (x == lang) return true;
    return false;
}

constexpr int MAX_KEYTERMS = 20;

/* Join non-comment non-blank lines of contexts/active-<lang>.txt with
 * commas, capped at MAX_KEYTERMS. Returns "" if the file is missing or
 * contains nothing usable. */
static std::string load_context_terms(const std::string &language) {
    const std::string path = "contexts\\active-" + language + ".txt";
    std::ifstream f(path);
    if (!f) return {};

    std::string terms;
    std::string line;
    int count = 0;

    while (std::getline(f, line) && count < MAX_KEYTERMS) {
        const size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        if (line[first] == '#') continue;

        const size_t last = line.find_last_not_of(" \t\r\n");
        line = line.substr(first, last - first + 1);
        if (line.empty()) continue;

        if (!terms.empty()) terms += ',';
        terms += line;
        ++count;
    }

    if (count > 0)
        std::fprintf(stderr, "loaded %d keyterms from %s\n", count, path.c_str());
    return terms;
}

static double frame_rms(const int16_t *p, int n) {
    if (!p || n <= 0) return 0.0;
    double acc = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(p[i]);
        acc += x * x;
    }
    return std::sqrt(acc / static_cast<double>(n));
}

// ---------- AudioQueue (lossless) ----------

class AudioQueue {
public:
    AudioQueue() = default;

    /* Lossless, non-blocking.
     *
     * push() must not block: the capture thread feeds every pane in turn, so
     * one model that falls behind would otherwise stall audio for all of
     * them. It must not silently discard either -- accuracy is the point of
     * this program. Backlog past the soft limit is warned about and counted
     * but every chunk is kept; the hard limit is only a memory guard. */
    bool push(std::shared_ptr<const std::vector<int16_t>> chunk) {
        if (!chunk || chunk->empty()) return true;

        std::lock_guard<std::mutex> lk(mutex_);
        if (closed_) return false;

        if (queue_.size() >= SOFT_BACKLOG_WARN_CHUNKS && !warned_) {
            warned_ = true;
            std::fprintf(stderr,
                "warning: audio backlog is %.1f s; inference is behind but "
                "nothing is being dropped\n",
                (double)queue_.size() * 0.050);
            verbose_logf("[queue] backlog warning: depth=%zu (~%.1f s)\n",
                         queue_.size(), (double)queue_.size() * 0.050);
        } else if (queue_.size() < SOFT_BACKLOG_WARN_CHUNKS / 2) {
            warned_ = false;
        }

        if (queue_.size() >= MAX_QUEUE_CHUNKS) {
            queue_.pop_front();
            ++dropped_;
            std::fprintf(stderr,
                "error: audio backlog exceeded %.1f s; dropping oldest chunk "
                "(total dropped: %llu)\n",
                (double)MAX_QUEUE_CHUNKS * 0.050,
                (unsigned long long)dropped_);
        }

        queue_.push_back(std::move(chunk));
        cv_.notify_one();
        return true;
    }

    bool pop(std::shared_ptr<const std::vector<int16_t>> &out) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    size_t depth() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.size();
    }

    uint64_t dropped() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return dropped_;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<const std::vector<int16_t>>> queue_;
    uint64_t dropped_ = 0;
    bool closed_ = false;
    bool warned_ = false;
};



// ---------- StatefulVad ----------

class StatefulVad {
public:
    struct Segment { std::vector<int16_t> audio; uint64_t start_sample = 0; };

    StatefulVad(double rms_threshold, int hangover_ms, int preroll_ms,
                int max_segment_ms)
        : rms_threshold_(rms_threshold),
          hangover_frames_(std::max(1, hangover_ms / VAD_FRAME_MS)),
          preroll_frames_(std::max(0, preroll_ms / VAD_FRAME_MS)),
          max_segment_samples_(SAMPLE_RATE / 1000 * max_segment_ms) {}

    void reset() {
        state_ = State::Silence; silence_count_ = 0;
        preroll_.clear(); segment_.clear();
        preroll_start_sample_ = 0; segment_start_sample_ = 0;
    }

    bool feed(const int16_t *frame, int n, uint64_t frame_start_sample,
              Segment &out) {
        if (!frame || n != VAD_FRAME_SAMPLES) return false;
        const double r = frame_rms(frame, n);

        if (state_ == State::Silence) {
            preroll_.emplace_back(frame, frame + n);
            preroll_start_sample_ = frame_start_sample;
            while (static_cast<int>(preroll_.size()) > preroll_frames_) {
                preroll_.pop_front();
                preroll_start_sample_ += VAD_FRAME_SAMPLES;
            }
            if (r >= rms_threshold_) {
                state_ = State::Speech; silence_count_ = 0; segment_.clear();
                segment_start_sample_ =
                    preroll_.empty() ? frame_start_sample : preroll_start_sample_;
                if (preroll_.empty())
                    segment_.insert(segment_.end(), frame, frame + n);
                else
                    for (const auto &f : preroll_)
                        segment_.insert(segment_.end(), f.begin(), f.end());
                preroll_.clear();
            }
            return false;
        }

        segment_.insert(segment_.end(), frame, frame + n);

        if (max_segment_samples_ > 0) {
            const uint64_t elapsed =
                (frame_start_sample + (uint64_t)n) - segment_start_sample_;
            if (elapsed >= (uint64_t)max_segment_samples_) {
                out.audio = std::move(segment_);
                out.start_sample = segment_start_sample_;
                segment_.clear(); preroll_.clear();
                state_ = State::Silence; silence_count_ = 0;
                return true;
            }
        }

        if (r < rms_threshold_) {
            ++silence_count_;
            if (silence_count_ >= hangover_frames_) {
                out.audio = std::move(segment_);
                out.start_sample = segment_start_sample_;
                segment_.clear(); preroll_.clear();
                state_ = State::Silence; silence_count_ = 0;
                return true;
            }
        } else silence_count_ = 0;
        return false;
    }

    bool flush(Segment &out) {
        if (state_ != State::Speech || segment_.empty()) return false;
        out.audio = std::move(segment_);
        out.start_sample = segment_start_sample_;
        segment_.clear(); preroll_.clear();
        state_ = State::Silence; silence_count_ = 0;
        return true;
    }

    bool in_speech() const { return state_ == State::Speech; }
    const std::vector<int16_t> &current_audio() const { return segment_; }
    uint64_t current_start_sample() const { return segment_start_sample_; }

private:
    enum class State { Silence, Speech };
    double rms_threshold_;
    int hangover_frames_, preroll_frames_;
    int max_segment_samples_ = 0;
    int silence_count_ = 0;
    State state_ = State::Silence;
    uint64_t preroll_start_sample_ = 0, segment_start_sample_ = 0;
    std::deque<std::vector<int16_t>> preroll_;
    std::vector<int16_t> segment_;
};

// ---------- PrintListener ----------

class PrintListener : public moonshine::TranscriptEventListener {
public:
    PrintListener(FILE *notes, TranscriptSink *sink, std::string language,
                  int slot_idx, std::chrono::system_clock::time_point session_start)
        : notes_(notes), sink_(sink), language_(std::move(language)),
          slot_idx_(slot_idx), session_start_(session_start) {}

    void set_stream_start(std::chrono::system_clock::time_point tp) {
        session_start_ = tp;
    }
    void set_segment_start(double rel_time) { current_start_ = rel_time; }
    void add_paused_ms(int64_t ms) { paused_ms_.fetch_add(ms); }

    void emit_partial(const std::string &text, double rel_time = 0.0) {
        if (text.empty() || !sink_) return;
        auto [pre, body] = split(text, rel_time);
        sink_->post({ slot_idx_, std::move(pre), std::move(body), false, false });
    }

    void emit_final(const std::string &text, double rel_time) {
        if (text.empty() || !sink_) return;
        auto [pre, body] = split(text, rel_time);

        if (notes_) {
            std::string full = pre + body + "\n";
            std::fwrite(full.data(), 1, full.size(), notes_);
            std::fflush(notes_);
        }
        sink_->post({ slot_idx_, std::move(pre), std::move(body), true, false });
    }

    void onLineTextChanged(const moonshine::LineTextChanged &e) override {
        verbose_logf("[%s] partial  text=\"%.80s\"\n",
                     language_.c_str(), e.line.text.c_str());
        // Use the line's own start time: for streaming workers there is no
        // per-segment VAD, so current_start_ (set by set_segment_start)
        // stays 0 and partial stamps would all read 00:00:00.
        emit_partial(e.line.text, e.line.startTime);
    }

    void onLineCompleted(const moonshine::LineCompleted &e) override {
        verbose_logf("[%s] latency=%d ms  text=\"%.80s\"\n",
                     language_.c_str(),
                     e.line.lastTranscriptionLatencyMs,
                     e.line.text.c_str());
        emit_final(e.line.text, e.line.startTime);
    }

    void onError(const moonshine::Error &e) override {
        if (sink_)
            sink_->post({ slot_idx_, {}, e.errorMessage, true, true });
    }

private:
    std::pair<std::string, std::string> split(const std::string &text, double rel) {
        double sec = rel;
        if (!std::isfinite(sec) || sec < 0.0) sec = 0.0;
        sec += static_cast<double>(paused_ms_.load()) / 1000.0;
        auto tp = session_start_ +
                  std::chrono::milliseconds((long long)(sec * 1000.0));
        std::time_t tt = std::chrono::system_clock::to_time_t(tp);
        struct tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char wall[16];
        std::strftime(wall, sizeof wall, "%H:%M:%S", &tmv);

        std::string pre;
        pre.reserve(32);
        pre += "["; pre += wall; pre += "] [";
        pre += language_; pre += "] ";
        return { std::move(pre), text };
    }

    FILE                                *notes_;
    TranscriptSink                      *sink_;
    std::string                          language_;
    int                                  slot_idx_;
    std::chrono::system_clock::time_point session_start_;
    double                               current_start_ = 0.0;
    std::atomic<int64_t>                 paused_ms_{0};
};

// ---------- StreamingWorker ----------

class StreamingWorker {
public:
    StreamingWorker(const std::string &model_path, int arch,
                    std::chrono::system_clock::time_point session_start,
                    FILE *notes, TranscriptSink *sink,
                    const std::string &language, int slot_idx,
                    bool spelling)
        : listener_(std::make_unique<PrintListener>(
              notes, sink, language, slot_idx, session_start)),
          spelling_(spelling) {
        // 0.6: parameters now come from olas-win.conf, defaulting to
        // Moonshine's documented values rather than the 0.5 tuning
        // (vad_threshold 0.55, vad_max_segment_duration 6,
        // transcription_interval 0.35). The over-aggressive VAD chopping
        // in particular was costing accuracy at segment boundaries.
        char interval_s[32], maxseg_s[32], vad_s[32];
        std::snprintf(interval_s, sizeof interval_s, "%.3f",
                      g_ocfg.transcription_interval);
        std::snprintf(maxseg_s, sizeof maxseg_s, "%d",
                      g_ocfg.vad_max_segment_duration);
        std::snprintf(vad_s, sizeof vad_s, "%.3f", g_ocfg.vad_threshold);

        moonshine::Options opts = {
            {"transcription_interval",   interval_s},
            {"return_audio_data",        "false"},
            {"vad_max_segment_duration", maxseg_s},
            {"vad_threshold",            vad_s},
        };

        // The SDK validates option keys against a strict whitelist; only
        // the documented keys survive. Keyterms are load-time only.
        const std::string keyterms = load_context_terms(language);
        if (!keyterms.empty())
            opts.emplace_back("keyterms", keyterms);

        if (spelling_) {
            opts.emplace_back("spelling_model_path",
                              model_path + "/spelling_cnn.ort");
        }
        transcriber_ = std::make_unique<moonshine::Transcriber>(
            model_path, to_model_arch(arch),
            g_ocfg.transcription_interval, "", opts);
        transcriber_->addListener(listener_.get());
        transcriber_->start();
    }
    ~StreamingWorker() {
        if (transcriber_) { try { transcriber_->stop(); } catch (...) {} }
    }

    void set_session_start(std::chrono::system_clock::time_point tp) {
        listener_->set_stream_start(tp);
    }
    void add_paused_ms(int64_t ms) { listener_->add_paused_ms(ms); }

    void feed(const std::vector<int16_t> &pcm) {
        if (!transcriber_ || pcm.empty()) return;
        fb_.resize(pcm.size());
        for (size_t i = 0; i < pcm.size(); ++i)
            fb_[i] = static_cast<float>(pcm[i]) / 32768.0f;
        try {
            transcriber_->addAudio(fb_, SAMPLE_RATE);
        } catch (const moonshine::MoonshineException &e) {
            print_error("[stream audio feed failed] ", e.what());
        }
    }

    void flush() {
        if (!transcriber_) return;
        try {
            uint32_t flags = spelling_ ? MOONSHINE_FLAG_SPELLING_MODE : 0u;
            transcriber_->updateTranscription(flags);
        }
        catch (const moonshine::MoonshineException &e) {
            print_error("[stream flush failed] ", e.what());
        }
    }
    void reset() {}
    bool spelling_enabled() const { return spelling_; }

private:
    static moonshine::ModelArch to_model_arch(int a) {
        switch (a) {
            case ARCH_TINY:             return moonshine::ModelArch::TINY;
            case ARCH_BASE:             return moonshine::ModelArch::BASE;
            case ARCH_TINY_STREAMING:   return moonshine::ModelArch::TINY_STREAMING;
            case ARCH_BASE_STREAMING:   return moonshine::ModelArch::BASE_STREAMING;
            case ARCH_SMALL_STREAMING:  return moonshine::ModelArch::SMALL_STREAMING;
            case ARCH_MEDIUM_STREAMING: return moonshine::ModelArch::MEDIUM_STREAMING;
            default: throw std::invalid_argument("bad arch");
        }
    }
    std::unique_ptr<moonshine::Transcriber> transcriber_;
    std::unique_ptr<PrintListener>          listener_;
    std::vector<float>                      fb_;
    bool                                    spelling_ = false;
};

// ---------- NonStreamingWorker ----------

class NonStreamingWorker {
public:
    NonStreamingWorker(const std::string &model_path, int arch,
                       std::chrono::system_clock::time_point session_start,
                       FILE *notes, TranscriptSink *sink,
                       double silence_rms,
                       const std::string &language, int slot_idx,
                       bool spelling)
        : listener_(std::make_unique<PrintListener>(
              notes, sink, language, slot_idx, session_start)),
          vad_(std::make_unique<StatefulVad>(
              silence_rms, VAD_HANGOVER_MS, VAD_PREROLL_MS, MAX_SEGMENT_MS)),
          spelling_(spelling) {
        const moonshine::ModelArch ma =
            (arch == ARCH_TINY) ? moonshine::ModelArch::TINY
                                : moonshine::ModelArch::BASE;
        moonshine::Options opts = {
            {"return_audio_data", "false"},
            {"vad_threshold",     "0.55"},
        };
        if (spelling_) {
            opts.emplace_back("spelling_model_path",
                              model_path + "/spelling_cnn.ort");
        }
        transcriber_ = std::make_unique<moonshine::Transcriber>(
            model_path, ma, 0.5, "", opts);
        inference_ = std::make_unique<InferenceThread>(
            transcriber_.get(), listener_.get(), &last_inference_ms_, spelling_);
    }

    void set_session_start(std::chrono::system_clock::time_point tp) {
        listener_->set_stream_start(tp);
    }
    void reset() {
        vad_->reset();
        pcm_rem_.clear();
        last_partial_at_ = 0;
        inference_->clear_pending_partial();
    }
    void add_paused_ms(int64_t ms) { listener_->add_paused_ms(ms); }

    void feed(const std::vector<int16_t> &pcm) {
        if (pcm.empty()) return;
        pcm_rem_.insert(pcm_rem_.end(), pcm.begin(), pcm.end());
        while (pcm_rem_.size() >= VAD_FRAME_SAMPLES) {
            const uint64_t frame_start = total_seen_;
            std::vector<int16_t> frame(pcm_rem_.begin(),
                                       pcm_rem_.begin() + VAD_FRAME_SAMPLES);
            pcm_rem_.erase(pcm_rem_.begin(),
                           pcm_rem_.begin() + VAD_FRAME_SAMPLES);
            total_seen_ += VAD_FRAME_SAMPLES;

            StatefulVad::Segment seg;
            if (vad_->feed(frame.data(), VAD_FRAME_SAMPLES, frame_start, seg)) {
                inference_->post_final(std::move(seg.audio), seg.start_sample);
                last_partial_at_ = 0;
                continue;
            }
            if (vad_->in_speech()) {
                const auto &audio = vad_->current_audio();
                const uint64_t start = vad_->current_start_sample();
                const uint64_t elapsed = total_seen_ - start;
                const uint64_t since   = total_seen_ - last_partial_at_;
                const bool enough = elapsed >=
                    (uint64_t)(SAMPLE_RATE * MIN_PARTIAL_AUDIO_MS / 1000);
                const int64_t last_ms = last_inference_ms_.load();
                const uint64_t min_gap = std::max<uint64_t>(
                    PARTIAL_INTERVAL_SAMPLES,
                    last_ms > 0 ? (uint64_t)last_ms * 32u : 0u);
                const bool due = last_partial_at_ == 0 || since >= min_gap;
                if (enough && due) {
                    const size_t win = std::min(audio.size(),
                        (size_t)MAX_PARTIAL_WINDOW_SAMPLES);
                    const uint64_t ws = start + (audio.size() - win);
                    std::vector<int16_t> w(audio.end() - win, audio.end());
                    inference_->post_partial(std::move(w), ws);
                    last_partial_at_ = total_seen_;
                }
            }
        }
    }

    void flush() {
        StatefulVad::Segment seg;
        if (vad_->flush(seg))
            inference_->post_final(std::move(seg.audio), seg.start_sample);
        inference_->drain();
    }

private:
    class InferenceThread {
    public:
        InferenceThread(moonshine::Transcriber *t, PrintListener *l,
                        std::atomic<int64_t> *timing_ms, bool spelling)
        : t_(t), l_(l), timing_ms_(timing_ms), spelling_(spelling) {
            th_ = std::thread([this] { run(); });
        }

        ~InferenceThread() {
            { std::lock_guard<std::mutex> lk(m_); closing_ = true; cv_.notify_all(); }
            if (th_.joinable()) th_.join();
        }

        void post_partial(std::vector<int16_t> &&a, uint64_t s) {
            std::lock_guard<std::mutex> lk(m_);
            pending_partial_ = Work{std::move(a), s};
            cv_.notify_one();
        }
        void post_final(std::vector<int16_t> &&a, uint64_t s) {
            std::lock_guard<std::mutex> lk(m_);
            finals_.push_back(Work{std::move(a), s});
            ++pending_finals_;
            cv_.notify_one();
        }
        void clear_pending_partial() {
            std::lock_guard<std::mutex> lk(m_);
            pending_partial_.reset();
        }
        void drain() {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return pending_finals_ == 0; });
        }

    private:
        struct Work { std::vector<int16_t> audio; uint64_t start_sample; };

        void run() {
            std::vector<float> fb;
            for (;;) {
                Work w; bool is_final;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [this] {
                        return closing_ || !finals_.empty() ||
                               pending_partial_.has_value();
                    });
                    if (closing_ && finals_.empty() &&
                        !pending_partial_.has_value()) return;
                    if (!finals_.empty()) {
                        w = std::move(finals_.front()); finals_.pop_front();
                        is_final = true;
                    } else {
                        w = std::move(*pending_partial_);
                        pending_partial_.reset();
                        is_final = false;
                    }
                }
                if (is_final) {
                    transcribe(w, fb, true);
                    std::lock_guard<std::mutex> lk(m_);
                    --pending_finals_;
                    cv_.notify_all();
                } else {
                    transcribe(w, fb, false);
                }
            }
        }

        void transcribe(const Work &w, std::vector<float> &fb, bool is_final) {
            if (w.audio.empty()) return;
            fb.resize(w.audio.size());
            for (size_t i = 0; i < w.audio.size(); ++i)
                fb[i] = static_cast<float>(w.audio[i]) / 32768.0f;

            const double rel = (double)w.start_sample / SAMPLE_RATE;
            l_->set_segment_start(rel);
            const auto t0 = std::chrono::steady_clock::now();
            try {
                uint32_t flags = spelling_ ? MOONSHINE_FLAG_SPELLING_MODE : 0u;
                const moonshine::Transcript t =
                    t_->transcribeWithoutStreaming(fb, SAMPLE_RATE, flags);

                std::string combined;
                double first_offset = 0.0;
                for (size_t i = 0; i < t.lines.size(); ++i) {
                    if (i == 0) first_offset = t.lines[i].startTime;
                    if (i) combined += ' ';
                    combined += t.lines[i].text;
                }
                const auto ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                if (timing_ms_) timing_ms_->store(ms);
                verbose_logf("[transcribe %s] audio=%.2fs  wall=%lldms  text=\"%.40s\"\n",
                             is_final ? "final" : "partial",
                             (double)w.audio.size() / SAMPLE_RATE,
                             (long long)ms,
                             combined.c_str());
                if (combined.empty()) return;

                if (is_final) l_->emit_final(combined, rel + first_offset);
                else          l_->emit_partial(combined);
            } catch (const moonshine::MoonshineException &e) {
                print_error("[transcription failed] ", e.what());
            } catch (const std::exception &e) {
                print_error("[transcription failed] ", e.what());
            }
        }

        moonshine::Transcriber    *t_;
        PrintListener             *l_;
        std::atomic<int64_t>      *timing_ms_ = nullptr;
        std::thread                th_;
        std::mutex                 m_;
        std::condition_variable    cv_;
        std::deque<Work>           finals_;
        int                        pending_finals_ = 0;
        std::optional<Work>        pending_partial_;
        bool                       closing_ = false;
        bool                       spelling_ = false;
    };

    std::unique_ptr<moonshine::Transcriber> transcriber_;
    std::unique_ptr<PrintListener>          listener_;
    std::unique_ptr<StatefulVad>            vad_;
    std::unique_ptr<InferenceThread>        inference_;
    std::atomic<int64_t>                    last_inference_ms_{0};
    uint64_t                                total_seen_ = 0;
    uint64_t                                last_partial_at_ = 0;
    std::vector<int16_t>                    pcm_rem_;
    bool                                    spelling_ = false;
};

// ===========================================================================
//  Engine::Impl
// ===========================================================================

struct Engine::Impl {
    struct Slot {
        LanguageConfig                     config;
        std::unique_ptr<StreamingWorker>   stream;
        std::unique_ptr<NonStreamingWorker> offline;
        std::unique_ptr<AudioQueue>        queue;
        std::thread                        thread;

        std::shared_ptr<std::atomic<bool>>    enabled;
        std::shared_ptr<std::atomic<bool>>    need_reset;
        std::shared_ptr<std::atomic<int64_t>> disabled_at_ns;

        // Core window this slot's worker (and its ORT pool) is confined to.
        int plan_first = 0;
        int plan_cores = 0;

        SlotCounters counters;

        void feed_pcm(const std::vector<int16_t> &pcm) {
            if (!enabled || !enabled->load()) return;
            if (stream)       stream->feed(pcm);
            else if (offline) offline->feed(pcm);
        }
        void set_session_start(std::chrono::system_clock::time_point tp) {
            if (stream)       stream->set_session_start(tp);
            if (offline)      offline->set_session_start(tp);
        }
        void flush() {
            if (stream)       stream->flush();
            else if (offline) offline->flush();
        }
        void add_paused_ms(int64_t ms) {
            if (stream)       stream->add_paused_ms(ms);
            if (offline)      offline->add_paused_ms(ms);
        }
        void reset() {
            if (stream)       stream->reset();
            else if (offline) offline->reset();
        }
    };

    std::vector<LanguageConfig>           configs;
    std::vector<std::unique_ptr<Slot>>    slots;
    ResourcePlan                          plan;
    TranscriptSink                       *sink;
    FILE                                 *notes;
    double                                silence_rms;
    std::chrono::system_clock::time_point session_start;

    explicit Impl(const std::vector<LanguageConfig> &cfgs,
                  TranscriptSink *snk, FILE *n, double rms,
                  std::chrono::system_clock::time_point ss)
        : configs(cfgs), sink(snk), notes(n),
          silence_rms(rms), session_start(ss) {}

    void start_slot(Slot &s, int idx) {
        if (is_streaming_arch(s.config.arch)) {
            s.stream = std::make_unique<StreamingWorker>(
                s.config.model_path, s.config.arch, session_start,
                notes, sink, s.config.language, idx, s.config.spelling);
        } else {
            s.offline = std::make_unique<NonStreamingWorker>(
                s.config.model_path, s.config.arch, session_start,
                notes, sink, silence_rms, s.config.language, idx,
                s.config.spelling);
        }
    }

    void start_thread(Slot &s) {
        s.queue = std::make_unique<AudioQueue>();
        AudioQueue *q = s.queue.get();
        s.thread = std::thread([&s, q] {
            // Put this worker on its model's cores. addAudio() runs here, so
            // this thread's affinity is what actually decides where the
            // decode work lands.
            if (s.plan_cores > 0)
                pin_current_thread(s.plan_first, s.plan_cores);

            std::shared_ptr<const std::vector<int16_t>> chunk;
            uint64_t seq = 0;
            while (q->pop(chunk)) {
                if (!chunk || chunk->empty()) continue;
                ++seq;
                s.counters.chunks_processed += 1;
                s.counters.audio_frames_total += chunk->size();

                if (g_verbose && (seq % 40) == 1) {
                    verbose_logf("[worker] chunk #%llu  queue_depth=%zu  pcm_frames=%zu\n",
                                 (unsigned long long)seq,
                                 q->depth(),
                                 chunk->size());
                }

                if (s.need_reset && s.need_reset->exchange(false))
                    s.reset();
                try {
                    const auto t0 = std::chrono::steady_clock::now();
                    s.feed_pcm(*chunk);
                    const auto dt = std::chrono::steady_clock::now() - t0;
                    s.counters.inference_ns_total +=
                        std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
                } catch (const moonshine::MoonshineException &e) {
                    print_error("[Moonshine processing failed] ", e.what());
                } catch (const std::exception &e) {
                    print_error("[processing failed] ", e.what());
                }
            }
        });
    }
};

// ---------- Engine ----------

Engine::Engine(const std::vector<LanguageConfig> &configs,
               TranscriptSink *sink, FILE *notes, double silence_rms,
               std::chrono::system_clock::time_point session_start)
    : impl_(std::make_unique<Impl>(configs, sink, notes, silence_rms,
                                   session_start)) {}

Engine::~Engine() { stop(); }

void Engine::start() {
    /* Plan the core budget before any transcriber exists: the ORT session
     * sizes its pool from the affinity of the thread that creates it, and
     * the env var must be right before the first session too. */
    std::vector<int> archs;
    archs.reserve(impl_->configs.size());
    for (const auto &c : impl_->configs) archs.push_back(c.arch);

    impl_->plan = plan_resources((int)impl_->configs.size(), archs,
                                 -1, -1);
    std::fprintf(stderr, "CPU: %s\n", impl_->plan.rationale.c_str());

    if (impl_->plan.force_single_thread) {
        _putenv_s("MOONSHINE_ORT_SINGLE_THREAD", "1");
        std::fprintf(stderr, "threads: single (MOONSHINE_ORT_SINGLE_THREAD=1)\n");
    } else {
        _putenv_s("MOONSHINE_ORT_SINGLE_THREAD", "");
        std::fprintf(stderr, "threads: ORT-managed pool, bounded by affinity\n");
    }

    impl_->slots.clear();
    impl_->slots.reserve(impl_->configs.size());
    int core_cursor = 0;
    for (size_t i = 0; i < impl_->configs.size(); ++i) {
        auto s = std::make_unique<Impl::Slot>();
        s->config          = impl_->configs[i];
        s->enabled         = std::make_shared<std::atomic<bool>>(true);
        s->need_reset      = std::make_shared<std::atomic<bool>>(false);
        s->disabled_at_ns  = std::make_shared<std::atomic<int64_t>>(-1);

        // Confine THIS thread to the model's own cores while its session is
        // built. Windows gives a new thread the affinity of the thread that
        // created it, so the ONNX Runtime worker threads this session spawns
        // inherit exactly this set. That is what keeps the two models off
        // each other's cores.
        const int want = (i < impl_->plan.model_cores.size())
                             ? impl_->plan.model_cores[i] : 0;
        s->plan_first  = core_cursor;
        s->plan_cores  = want;
        if (want > 0) {
            pin_current_thread(core_cursor, want);
            core_cursor += want;
        }

        impl_->start_slot(*s, (int)i);
        impl_->slots.push_back(std::move(s));
    }

    // Lift the restriction from this (the main) thread. Per-thread on
    // purpose: SetProcessAffinityMask would rewrite the masks of the ORT
    // threads just created and undo the split above.
    if (impl_->plan.pin_affinity)
        unpin_current_thread();

    for (auto &s : impl_->slots) impl_->start_thread(*s);
}

void Engine::stop() {
    if (!impl_) return;
    for (auto &s : impl_->slots) {
        if (s->queue) s->queue->close();
    }
    for (auto &s : impl_->slots) {
        if (s->thread.joinable()) s->thread.join();
    }
    for (auto &s : impl_->slots) {
        try { s->flush(); } catch (...) {}
    }
    impl_->slots.clear();
}

void Engine::flush() {
    for (auto &s : impl_->slots) {
        try { s->flush(); } catch (...) {}
    }
}

void Engine::feed(std::shared_ptr<const std::vector<int16_t>> pcm) {
    if (!pcm || pcm->empty()) return;
    for (auto &s : impl_->slots) {
        if (!s->queue) continue;
        if (s->enabled && !s->enabled->load()) continue;
        s->queue->push(pcm);
        s->counters.chunks_pushed += 1;
        const size_t d = s->queue->depth();
        s->counters.queue_depth = d;
        if (d > s->counters.queue_max_depth)
            s->counters.queue_max_depth = d;
    }
}

void Engine::toggle(int slot) {
    if (slot < 0 || slot >= (int)impl_->slots.size()) return;
    auto &s = *impl_->slots[slot];
    if (!s.enabled) return;

    const bool now = !s.enabled->load();
    s.enabled->store(now);

    const int64_t now_ns = std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    if (!now) {
        if (s.need_reset)     s.need_reset->store(true);
        if (s.disabled_at_ns) s.disabled_at_ns->store(now_ns);
    } else if (s.disabled_at_ns) {
        const int64_t was = s.disabled_at_ns->exchange(-1);
        if (was >= 0) {
            const int64_t ms = (now_ns - was) / 1000000;
            if (ms > 0) s.add_paused_ms(ms);
        }
    }
}

bool Engine::is_enabled(int slot) const {
    if (slot < 0 || slot >= (int)impl_->slots.size()) return false;
    auto &s = *impl_->slots[slot];
    return s.enabled && s.enabled->load();
}

size_t Engine::size() const { return impl_->slots.size(); }

Engine::Stats Engine::get_stats(int slot) const {
    Stats out{};
    if (slot < 0 || slot >= (int)impl_->slots.size()) return out;
    const auto &c = impl_->slots[slot]->counters;
    out.chunks_pushed      = c.chunks_pushed;
    out.chunks_processed   = c.chunks_processed;
    out.queue_depth        = c.queue_depth;
    out.queue_max_depth    = c.queue_max_depth;
    out.final_jobs_posted  = c.final_jobs_posted;
    out.final_jobs_dropped = c.final_jobs_dropped;
    const uint64_t ns  = c.inference_ns_total;
    const uint64_t frm = c.audio_frames_total;
    if (frm > 0) {
        const double audio_s = (double)frm / SAMPLE_RATE;
        const double wall_s  = (double)ns / 1e9;
        out.inference_rtf = wall_s / audio_s;
    } else {
        out.inference_rtf = 0.0;
    }
    return out;
}

} // namespace olas
