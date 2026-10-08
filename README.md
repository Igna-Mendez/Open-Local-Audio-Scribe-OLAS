# OLAS — Open Local Audio Scribe (Windows)

Real-time local speech-to-text for Windows. Two language panes side by side,
one model each, running entirely on your machine. No cloud, no telemetry, no
account, no API keys, no network at runtime.

Built for meetings, interviews, lectures and interpretation — the situations
where a transcript needs to keep up with people talking, and where the audio
should never leave the machine. Captures what the machine is *playing* via
WASAPI loopback, so it works on calls, videos and streams without a virtual
cable.

**Accuracy is the design goal.** Where a trade-off exists between staying
current and keeping every word, this program keeps the word.

> **Note on how this was built.** OLAS was heavily vibe-coded: roughly 95% of
> the code was written by different agentic AI models, with a human directing
> the design, testing on real hardware and deciding what shipped. The
> measurements in [PATCHNOTES.md](PATCHNOTES.md) exist because the AI-written
> parts got things confidently wrong more than once, and only measurement
> caught it. Treat the code accordingly: it works, but it has not had a
> conventional human review.

---

## What it does

- **Two languages at once** — English and Spanish, each in its own pane with
  independent Start/Stop, collapse and decouple-to-window controls
- **Two modes** — Normal (more accurate) and Potato (ultralight), chosen on
  first run and changeable from Options
- **Focus mode** — hides the toolbar and pane headers so the transcript fills
  the window
- **Live transcript file** — written line by line to `olas-moonshine-notes.txt`
- **Options popup** — capture device, English model, zoom, timestamps,
  light/dark theme, auto-scroll
- **Update check** — asks GitHub whether a newer release exists
- **Diagnostics** — `-v` writes per-line latency to `olas-debug.log`, `--stats`
  prints inference diagnostics on exit

## Modes

Which mode runs is decided by the **English** model. Spanish is always Small
Streaming: no Medium Spanish model exists.

| mode | English | Spanish | cores | character |
| ---- | ------- | ------- | ----- | --------- |
| **Normal** (default) | Medium Streaming | Small Streaming | 3 + 1 | more accurate |
| **Potato** | Small Streaming | Small Streaming | 1 + 1 | ultralight, lower CPU, slightly less accurate |

The English model is chosen on first launch and remembered in `olas-model.txt`
beside the executable. To change it later: **Options → English model**, then
restart.

## Download and run

1. Open the
   [Releases page](https://github.com/Igna-Mendez/Open-Local-Audio-Scribe-OLAS/releases).
2. Download the latest `OLAS-win64-1.1.x.zip`.
3. Extract it anywhere.
4. Double-click **`OLAS.bat`**.

The zip contains everything: the executable, the runtime DLL, and all three
models. Nothing else to install.

**Requirements**

- Windows 10 21H2 or newer (Windows 11 recommended)
- x64 CPU with AVX2 — Intel Haswell (2013) or AMD Excavator (2015) and newer
- 4+ logical CPU threads recommended
- ~800 MB free disk

No GPU is used or required.

## How it works

```
WASAPI loopback (miniaudio)
        |  16 kHz mono s16, 50 ms chunks
        v
capture.c ring buffer
        |  capture_read_chunk()
        v
capture thread
        |  Engine::feed() -> AudioQueue per language (lossless, non-blocking)
        v
worker thread per language
        |  Moonshine Transcriber (streaming)
        v
listener -> Win32 RichEdit, and to olas-moonshine-notes.txt
```

Two models run in parallel, each processing the same audio stream. Both
transcribe everything; you read the pane for the language being spoken. This is
simpler and more robust than language detection, at the cost of the non-target
pane producing nonsense — a known limitation, discussed in
[PATCHNOTES.md](PATCHNOTES.md).

Each language runs on its own worker thread with its own Transcriber, on its
own CPU core set. The core budget and per-model split are derived from the
detected hardware at startup.

### Source layout

```
src/
    main_win.cpp         entry point, CLI, capture thread
    moonshine_engine.*   Engine, worker slots, Streaming/NonStreaming workers
    resource_plan.*      CPU detection, per-model core split, affinity
    model_choice.*       persisted English model selection
    win32_ui.*           panes, toolbar overlay, buttons, popup menu
    capture.c / .h       WASAPI loopback + ring buffer
    miniaudio.h          vendored miniaudio 0.11
    update_check.*       GitHub release check
    transcript_sink.h    engine <-> UI abstraction
tools/
    fetch-streaming-models.ps1
cmake/
    copy-models.cmake    bundles models into the release zip
```

## Configuration

`olas-win.conf` holds the transcription parameters:

```ini
[general]
vad_threshold = 0.5             # speech/silence threshold
vad_max_segment_duration = 12   # longest single line, seconds
transcription_interval = 1.0    # how often the decoder re-runs
```

Delete it and built-in defaults apply. Bad values and unknown keys warn on
stderr; they never stop the program starting.

Threading, affinity and the model choice are not configured here — they come
from the hardware probe and from `olas-model.txt`.

## Command line

| Flag | Description | Default |
|---|---|---|
| `-l, --language CODE[,CODE]` | Language codes | `en,es` |
| `-m, --model PATH[,PATH]` | Model directory per language | resolved from the arch |
| `-a, --arch N[,N]` | 0=Tiny, 1=Base, 2=TinyStreaming, 4=SmallStreaming, 5=MediumStreaming | from `olas-model.txt` |
| `-q, --chunk-ms MS` | Capture chunk (20..1000) | `50` |
| `-v, --verbose` | Write `olas-debug.log` | off |
| `-c, --config PATH` | Transcription parameters file | `olas-win.conf` |
| `--no-update-check` | Skip the release check | off |
| `--stats` | Print inference diagnostics on exit | off |
| `-h, --help` | Show help | |

`3=BaseStreaming` is rejected — declared in Moonshine's C API for
forward-compatibility but not supported.

## Built with

- **[Moonshine Voice](https://github.com/moonshine-ai/moonshine)** — the
  speech-to-text models and C++ runtime. Small and Medium Streaming
  architectures, MIT licensed.
- **Win32 / RichEdit** — the user interface, with a custom button class for
  theme-aware controls.
- **[miniaudio](https://github.com/mackron/miniaudio)** — WASAPI loopback
  capture, vendored single-header.
- **ONNX Runtime** — inference, built as part of Moonshine's CMake project.

## Building from source

Prerequisites: Visual Studio 2022 Build Tools with "Desktop development with
C++", CMake ≥ 3.20, Git, PowerShell 5.1+, and optionally Ninja.

```bat
git clone https://github.com/Igna-Mendez/Open-Local-Audio-Scribe-OLAS.git
cd Open-Local-Audio-Scribe-OLAS
powershell -ExecutionPolicy Bypass -File setup.ps1

mkdir build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_MODE=bundled ..
cmake --build . --config Release -j
```

`setup.ps1` checks the toolchain, fetches all three models into `models\`, and
prints the exact configure command. The first configure takes 5–15 minutes: it
fetches the Moonshine sources and builds ONNX Runtime from them.

To produce a release zip, including the models:

```bat
cmake --build . --config Release --target zip -j
```

Creates `build\dist\OLAS-win64-<version>.zip`.

Cross-compiling from Linux with mingw-w64 is also supported via
`mingw-toolchain.cmake`.

## Reporting bugs

Open an issue at
<https://github.com/Igna-Mendez/Open-Local-Audio-Scribe-OLAS/issues> with your
Windows version and CPU model, the output of `olas_win.exe -l en,es -v --stats`,
which mode you are running, and what you expected versus what happened.

## License

MIT. Moonshine and miniaudio are both MIT; see `LICENSE` for the full text.
