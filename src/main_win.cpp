// main_win.cpp — OLAS for Windows: entry point, CLI, wiring.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <avrt.h>
#include "update_check.h"
#include "model_choice.h"

#include "moonshine_engine.h"
#include "transcript_sink.h"
#include "win32_ui.h"

extern "C" {
#include "capture.h"
}

using namespace olas;

// ---------------- config ----------------

static std::vector<LanguageConfig> g_configs;
static double g_silence_rms        = DEF_SILENCE_RMS;
static int    g_capture_chunk_ms   = DEFAULT_CAPTURE_CHUNK_MS;
static bool   g_no_update_check    = false;
static bool   g_show_stats         = false;

static FILE *g_notes   = nullptr;
static std::atomic<bool> g_running{true};

// Chosen on first run, or read from the saved file. Unset means the user has
// not been asked yet and the CLI did not specify an arch.
static EnglishModel g_english_choice = EnglishModel::Unset;

// ---------------- Win32 transcript sink ----------------

class Win32Sink : public TranscriptSink {
public:
    void post(TranscriptEvent ev) override {
        win32_ui_post_update(ev.slot,
                             ev.prefix.c_str(),
                             ev.body.c_str(),
                             ev.is_final ? 1 : 0,
                             ev.is_error ? 1 : 0);
    }
};

// ---------------- capture thread ----------------

struct CaptureArgs {
    Engine *engine = nullptr;
    int     chunk_samples = 0;
};

static void capture_loop(CaptureArgs *a) {
    // The capture thread must not be confined to the inference cores. Those
    // are deliberately narrow (a few CPUs); sharing them would let the decoder
    // starve the audio feed, which shows up as stutter and lag rather than as
    // dropped audio. Lift any inherited restriction and widen to the whole
    // machine.
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
        SetThreadAffinityMask(GetCurrentThread(), process_mask);

    // Register with the Multimedia Class Scheduler Service. MMCSS gives the
    // audio thread a guaranteed slice and ducking priority over normal work,
    // which is enough to keep the capture path glitch-free without needing
    // TIME_CRITICAL (which can starve the rest of the system).
    DWORD mmcss_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcss_index);
    if (!mmcss) {
        win32_ui_post_status("warning: MMCSS registration failed; audio may glitch under load");
    }

    std::vector<int16_t> pcm((size_t)a->chunk_samples);
    while (g_running.load()) {
        if (!capture_read_chunk(pcm.data())) {
            if (!g_running.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        a->engine->feed(std::make_shared<const std::vector<int16_t>>(pcm));
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

// ---------------- CLI helpers ----------------

static std::vector<std::string> argv_to_utf8(int &argc_out) {
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> out;
    if (!wargv) { argc_out = 0; return out; }
    out.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    LocalFree(wargv);
    argc_out = argc;
    return out;
}

static std::string default_model_dir(int arch, const std::string &lang) {
    switch (arch) {
        case ARCH_TINY_STREAMING:   return "models\\tiny-streaming-"   + lang;
        case ARCH_SMALL_STREAMING:  return "models\\small-streaming-"  + lang;
        case ARCH_MEDIUM_STREAMING: return "models\\medium-streaming-" + lang;
        case ARCH_TINY:             return "models\\tiny-" + lang;
        default:                    return "models\\base-" + lang;
    }
}

static void usage(const char *prog) {
    std::fprintf(stderr,
                 "olas_win - Open Local Audio Scribe for Windows\n\n"
                 "Usage: %s [options]\n\n"
                 "  -m, --model PATH[,PATH]    Model directory per language\n"
                 "  -a, --arch N[,N]           Architecture per language [4=SmallStreaming]\n"
                 "  -l, --language CODE[,CODE] Comma-separated language codes [en,es]\n"
                 "  -r, --rms THRESHOLD        Silence RMS threshold [%.0f]\n"
                 "  -q, --chunk-ms MS          Capture chunk in ms [%d]\n"
                 "  -v, --verbose              Verbose logging to %s\n"
                 "  -c, --config PATH          Transcription parameters [olas-win.conf]\n"
                 "  --spelling LANG[,LANG]     Enable spelling mode for listed languages\n"
                 "  --stats                    Print inference diagnostics on exit\n"
                 "  -h, --help                 Show this help\n\n"
                 "Architecture numbers:\n"
                 "  0=Tiny  1=Base  2=TinyStreaming  4=SmallStreaming  5=MediumStreaming\n"
                 "  (3=BaseStreaming is not supported by Moonshine and is rejected.)\n\n"
                 "Keyterms: put one term per line in contexts/active-<lang>.txt.\n"
                 "Moonshine docs warn that lists over 20 terms cause phantom words.\n",
                 prog, DEF_SILENCE_RMS, DEFAULT_CAPTURE_CHUNK_MS, VERBOSE_LOG_FILE);
}

// ---------------- UI callbacks ----------------

static Engine *g_engine = nullptr;

static void on_toggle(int slot) {
    if (!g_engine) return;
    g_engine->toggle(slot);
    win32_ui_set_pane_enabled(slot, g_engine->is_enabled(slot) ? 1 : 0);
}

static void on_device(int device_index) {
    // Restart capture on the new device. Audio is dropped briefly.
    capture_stop();
    capture_start(device_index);
}

// ---------------- main ----------------

int main() {
    if (!std::getenv("MOONSHINE_ORT_SINGLE_THREAD"))
        _putenv_s("MOONSHINE_ORT_SINGLE_THREAD", "1");

    int argc = 0;
    std::vector<std::string> argv = argv_to_utf8(argc);

    std::vector<std::string> languages, models, spelling_langs;
    std::vector<int>         arches;
    std::string config_path = OlasConfig::default_path();

    for (int i = 1; i < argc; ++i) {
        const std::string &a = argv[i];
        auto need = [&](const char *name) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", name);
                return false;
            }
            return true;
        };
        if (a == "-m" || a == "--model") {
            if (!need("--model")) return 1;
            models = split_csv(argv[++i]);
        } else if (a == "-l" || a == "--language") {
            if (!need("--language")) return 1;
            languages = split_csv(argv[++i]);
        } else if (a == "-a" || a == "--arch") {
            if (!need("--arch")) return 1;
            arches.clear();
            for (auto &tok : split_csv(argv[++i])) {
                int v;
                if (!parse_int(tok.c_str(), v) || !valid_arch(v)) {
                    std::fprintf(stderr, "invalid --arch: %s\n", tok.c_str());
                    return 1;
                }
                arches.push_back(v);
            }
        } else if (a == "-v" || a == "--verbose") {
            g_verbose = true;
        } else if (a == "-r" || a == "--rms") {
            if (!need("--rms")) return 1;
            double v;
            if (!parse_double(argv[++i].c_str(), v) || v < 0.0) {
                std::fprintf(stderr, "invalid --rms\n"); return 1;
            }
            g_silence_rms = v;
        } else if (a == "-q" || a == "--chunk-ms") {
            if (!need("--chunk-ms")) return 1;
            int v;
            if (!parse_int(argv[++i].c_str(), v) || v < 20 || v > 1000) {
                std::fprintf(stderr, "invalid --chunk-ms\n"); return 1;
            }
            g_capture_chunk_ms = v;
        } else if (a == "--no-update-check") {
            g_no_update_check = true;
        } else if (a == "--spelling") {
            if (!need("--spelling")) return 1;
            spelling_langs = split_csv(argv[++i]);
        } else if (a == "-c" || a == "--config") {
            if (!need("--config")) return 1;
            config_path = argv[++i];
        } else if (a == "--stats") {
            g_show_stats = true;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0].c_str()); return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(argv[0].c_str()); return 1;
        }
    }

    if (languages.empty()) { languages.push_back("en"); languages.push_back("es"); }

    for (const auto &l : languages) {
        if (!is_supported_language(l)) {
            std::fprintf(stderr, "unsupported language: %s\n", l.c_str());
            return 1;
        }
    }
    if (languages.size() > 2) {
        std::fprintf(stderr, "olas_win supports at most two languages\n");
        return 1;
    }

    // English has two models. Ask once on first run and remember the answer;
    // the Options menu can change it later, which needs a restart.
    bool english_active = false;
    for (const auto &l : languages) if (l == "en") english_active = true;

    if (english_active && arches.empty()) {
        EnglishModel choice = load_english_model();
        if (choice == EnglishModel::Unset) {
            const int r = MessageBoxW(nullptr,
                L"Choose the ENGLISH model.\n\n"
                L"  Normal mode - Medium Streaming (English)\n"
                L"                Heavier, more accurate. Uses 3 cores for "
                L"English and 1 for Spanish.\n\n"
                L"  Potato mode - Small Streaming (English)\n"
                L"                Ultralight: 1 core per language, 2 total.\n"
                L"                Lower CPU, slightly less accurate.\n\n"
                L"Spanish always uses Small Streaming.\n\n"
                L"Use Normal mode (Medium English)?  "
                L"No = Potato mode (Small English).",
                L"OLAS 1.1 - choose the English model",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
            choice = (r == IDYES) ? EnglishModel::Medium
                                  : EnglishModel::Small;
            save_english_model(choice);
            MessageBoxW(nullptr,
                L"Model loaded. You can change it later from "
                L"Options > English model.",
                L"OLAS 1.1", MB_OK | MB_ICONINFORMATION);
        }
        g_english_choice = choice;
        std::fprintf(stderr, "english model: %s (from %s)\n",
                     english_model_name(choice), model_choice_path().c_str());
    }

    if (arches.empty()) {
        // English uses the saved/asked-for choice, other languages fall back
        // to Small Streaming; English is the only language with two models.
        for (size_t i = 0; i < languages.size(); ++i) {
            if (languages[i] == "en" && g_english_choice != EnglishModel::Unset)
                arches.push_back(english_model_arch(g_english_choice));
            else if (languages[i] == "en")
                arches.push_back(ARCH_MEDIUM_STREAMING);
            else
                arches.push_back(ARCH_SMALL_STREAMING);
        }
    } else if (arches.size() == 1 && languages.size() > 1)
        arches.assign(languages.size(), arches.front());
    else if (arches.size() != languages.size()) {
        std::fprintf(stderr, "--arch count mismatch\n"); return 1;
    }

    if (models.empty()) {
        models.reserve(languages.size());
        for (size_t i = 0; i < languages.size(); ++i)
            models.push_back(default_model_dir(arches[i], languages[i]));
    } else if (models.size() == 1 && languages.size() > 1) {
        models.assign(languages.size(), models.front());
    } else if (models.size() != languages.size()) {
        std::fprintf(stderr, "--model count mismatch\n"); return 1;
    }

    for (size_t i = 0; i < languages.size(); ++i) {
        LanguageConfig c;
        c.language   = languages[i];
        c.model_path = models[i];
        c.arch       = arches[i];
        c.spelling   = std::find(spelling_langs.begin(), spelling_langs.end(),
                                 languages[i]) != spelling_langs.end();
        g_configs.push_back(std::move(c));
    }

    {
        std::vector<std::string> warnings;
        g_ocfg = OlasConfig::load(config_path, warnings);
        for (const auto &w : warnings)
            std::fprintf(stderr, "config: %s\n", w.c_str());
        std::fprintf(stderr,
            "config: %s  vad=%.2f max_seg=%ds interval=%.2fs\n",
            config_path.c_str(), g_ocfg.vad_threshold,
            g_ocfg.vad_max_segment_duration,
            g_ocfg.transcription_interval);
    }

    // Notes file: next to the exe (matches Linux behaviour).
    g_notes = std::fopen(NOTES_FILE, "w");
    if (!g_notes) {
        MessageBoxW(nullptr, L"Could not create the notes file.",
                    L"OLAS", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (g_verbose) {
        g_verbose_log = std::fopen(VERBOSE_LOG_FILE, "w");
        if (g_verbose_log) {
            std::fprintf(g_verbose_log, "OLAS verbose log\nlanguages:");
            for (auto &l : languages) std::fprintf(g_verbose_log, " %s", l.c_str());
            std::fprintf(g_verbose_log, "\narches:");
            for (auto &a : arches) std::fprintf(g_verbose_log, " %d", a);
            std::fprintf(g_verbose_log, "\nmodels:");
            for (auto &m : models) std::fprintf(g_verbose_log, " %s", m.c_str());
            std::fprintf(g_verbose_log, "\nspelling:");
            for (auto &l : languages) {
                const bool on = std::find(spelling_langs.begin(),
                                          spelling_langs.end(), l) != spelling_langs.end();
                std::fprintf(g_verbose_log, " %s=%s", l.c_str(), on ? "on" : "off");
            }
            std::fprintf(g_verbose_log, "\n\n");
            std::fflush(g_verbose_log);
        }
    }

    // Init capture (miniaudio / WASAPI).
    if (!capture_init()) {
        MessageBoxW(nullptr,
            L"Could not initialize audio (miniaudio / WASAPI).",
            L"OLAS", MB_OK | MB_ICONERROR);
        std::fclose(g_notes); return 1;
    }
    capture_set_chunk_samples(SAMPLE_RATE * g_capture_chunk_ms / 1000);

    // UI (owns the sink).
    Win32Sink sink;
    if (!win32_ui_init(languages)) {
        MessageBoxW(nullptr, L"Could not create the main window.",
                    L"OLAS", MB_OK | MB_ICONERROR);
        capture_uninit();
        std::fclose(g_notes); return 1;
    }
    win32_ui_populate_devices(0);

    // Engine.
    auto session_start = std::chrono::system_clock::now();
    Engine engine(g_configs, &sink, g_notes, g_silence_rms, session_start);
    g_engine = &engine;

    try {
        engine.start();
    } catch (const std::exception &e) {
        char msg[512];
        std::snprintf(msg, sizeof msg, "Failed to initialise Moonshine: %s", e.what());
        MessageBoxA(nullptr, msg, "OLAS", MB_OK | MB_ICONERROR);
        win32_ui_shutdown();
        capture_uninit();
        std::fclose(g_notes); return 1;
    }

    // Start capture.
    capture_start(0);

    // Capture thread.
    CaptureArgs cargs;
    cargs.engine = &engine;
    cargs.chunk_samples = SAMPLE_RATE * g_capture_chunk_ms / 1000;
    std::thread cap_thread(capture_loop, &cargs);

    if (!g_no_update_check && std::strcmp(OLAS_UPDATE_REPO, "") != 0) {
        std::thread([]{
            std::this_thread::sleep_for(std::chrono::seconds(2));
            olas::UpdateInfo info = olas::check_for_updates(OLAS_UPDATE_REPO, 5000);
            if (info.completed && info.outdated) {
                win32_ui_show_update_prompt(
                    info.tag_name.c_str(), info.release_name.c_str(),
                    olas::build_git_branch(), info.html_url.c_str());
            }
        }).detach();
    }

    // Message loop (blocks until the main window closes).
    win32_ui_run(on_toggle, on_device);

    // Shutdown.
    g_running.store(false);
    capture_stop();
    if (cap_thread.joinable()) cap_thread.join();

    // Read the counters before stopping: stop() clears the slots, so
    // get_stats() would return zeros afterwards.
    std::vector<Engine::Stats> stats;
    if (g_show_stats) {
        for (size_t i = 0; i < languages.size() && i < engine.size(); ++i)
            stats.push_back(engine.get_stats((int)i));
    }

    engine.stop();
    g_engine = nullptr;

    win32_ui_shutdown();
    capture_uninit();

    if (g_show_stats) {
        for (size_t i = 0; i < languages.size() && i < stats.size(); ++i) {
            const auto st = stats[i];
            std::fprintf(stderr, "\n=== Slot %zu (%s) ===\n", i, languages[i].c_str());
            std::fprintf(stderr, "  chunks_pushed      : %llu\n",
                         (unsigned long long)st.chunks_pushed);
            std::fprintf(stderr, "  chunks_processed   : %llu\n",
                         (unsigned long long)st.chunks_processed);
            std::fprintf(stderr, "  queue_depth        : %llu\n",
                         (unsigned long long)st.queue_depth);
            std::fprintf(stderr, "  queue_max_depth    : %llu\n",
                         (unsigned long long)st.queue_max_depth);
            std::fprintf(stderr, "  inference_rtf      : %.3f\n", st.inference_rtf);
        }
    }

    if (g_verbose_log) { std::fclose(g_verbose_log); g_verbose_log = nullptr; }
    if (g_notes) { std::fclose(g_notes); g_notes = nullptr; }

    return 0;
}
