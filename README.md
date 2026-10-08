# OLAS — Open Local Audio Scribe — Windows — version 1.1

Real-time local speech-to-text for Windows, powered by Moonshine C++ and WASAPI
loopback capture. Two language panes side by side, one model each, running
entirely on your machine. No cloud, no telemetry, no network at runtime. Native
Win32 UI.

Defaults to **Normal mode**: Medium Streaming English and Small Streaming
Spanish. The build ships all three models (Medium English, Small English, Small
Spanish) and you choose between the two modes.

**Accuracy is the design goal.** Where a trade-off exists between staying
current and keeping every word, this program keeps the word.

---

## Modes

Which mode runs is decided by the **English** model. Spanish is always Small
Streaming: no Medium Spanish model exists.

| mode | English | Spanish | cores | character |
| ---- | ------- | ------- | ----- | --------- |
| **Normal** (default) | Medium Streaming | Small Streaming | 3 + 1 | more accurate |
| **Potato** | Small Streaming | Small Streaming | 1 + 1 | ultralight, lower CPU, slightly less accurate |

On first launch OLAS asks which English model to load and explains both modes.
The answer is saved to `olas-model.txt` beside the executable, so it is asked
only once. To change it later: **Options → English model**, then restart.

---

## Research and development notes

Everything below was measured on the Linux build of the same engine (Ryzen
5600X, real speech); the Windows build shares the logic and the reasoning.
Where an earlier conclusion was wrong, it is recorded as wrong — the
corrections matter more than the original guess.

### 1. `MOONSHINE_ORT_SINGLE_THREAD` is a boolean, not a thread count

The obvious-looking knob does not do what its name suggests. Verified by
reading `/proc/<pid>/task` while running:

| value set | ORT worker threads |
| --------- | ------------------ |
| `1` | 1 |
| `2` | 1 |
| `4` | 1 |
| `8` | 1 |
| unset | 26 |

Any value forces exactly one thread. It **cannot** be used to request N
threads. The only real choice is *set* (one thread) or *unset* (ONNX Runtime
sizes its own pool), and the pool must then be bounded with CPU affinity.

*Earlier in this project it was concluded from these same runs that "1 thread
is fastest and 2/4/8 are equal or slower". That was wrong: every one of those
runs was single-threaded, so nothing was being compared.*

### 2. Multi-threaded inference is meaningfully faster

Medium Streaming, real speech (44 s), three runs each, variance under 2%:

| mode | RTF |
| ---- | --- |
| `MOONSHINE_ORT_SINGLE_THREAD=1` | 0.733 / 0.724 / 0.718 → **0.725** |
| variable unset | 0.467 / 0.462 / 0.449 → **0.459** |

RTF is `decode_wall / audio_duration`; below 1.0 keeps up with a live
microphone. Unsetting the variable is **~37% faster**, so 1.1 leaves it unset
by default and bounds the cost with an affinity budget instead.

### 3. Per-model core split

The two models get separate core sets so their ORT pools do not contend.
Measured with Medium+Small over one shared 4-core mask, the slowest model sat
at RTF ~0.95 — close enough to 1.0 to be fragile. Split, it drops to ~0.81.

The split is applied at session creation, because that is when ONNX Runtime
sizes its pool, and the process widens back afterwards so capture and UI are
not confined to one model's cores.

Dual Small, 1+1 against 2+2, 44 s of speech, three runs each:

| split | mean RTF | worst |
| ----- | -------- | ----- |
| **1 + 1** (potato) | **0.566** | **0.597** |
| 2 + 2 | 0.574 | 0.650 |

1+1 is marginally faster and noticeably steadier (the 2+2 pairs diverge, one
instance ~0.50 and the other ~0.65), for half the CPU. Hence potato mode.

### 4. There is no usable GPU path

- `libmoonshine.so` parses only `cpu`, `coreml`, `nnapi`. Requesting `cuda`,
  `rocm`, `openvino`, `dml` or `tensorrt` is accepted at construction and then
  **silently ignored**.
- Tested through the real inference path on a host with no usable GPU: every
  provider name produced **identical output and no error** — none of them ran
  on a GPU.
- Moonshine's own documentation confirms it: *"Unset means CPU-only
  (recommended)"*.

GPU support has been removed from the code.

### 5. The audio queue must be lossless and must never block

Two properties, pulling opposite ways:

- **Never block.** The original design blocked the producer once the queue
  filled. Because the capture thread feeds every pane in turn, one slow model
  froze the capture thread and starved *both* panes; the backlog then grew
  without bound and the transcript drifted permanently behind.
- **Never discard.** Dropping the oldest chunk keeps a caption live but deletes
  speech, which is unacceptable here.

The 1.1 queue therefore: never blocks, never drops, warns once when the backlog
passes 10 s, and keeps a 60 s hard ceiling purely as a memory guard.

### 6. `transcription_interval` is the main CPU dial, and it is free

It does **not** change the transcript. Measured on real speech with Medium,
44 s of audio: 69 s of wall clock at 0.5, 58 s at 2.0, for identical output.
Higher costs less CPU and updates the screen less often. Default is 1.0.

### 7. Benchmarking pitfalls hit along the way

Recorded so they are not repeated:

- **Synthetic tones prove nothing.** A tone-based benchmark reported RTF 0.17;
  it was also producing `lines=0`. The model decoded nothing.
- **Warm-up matters.** A cold ONNX Runtime session reported RTF 1.19 for a
  model that runs at 0.73 warm.
- **Check the thread count, don't infer it.** The "8 threads is slower"
  conclusion came from a variable that ignores 8.

### 8. Readability — unsolved

Each pane hears all audio, so the pane for the language nobody is speaking
still emits text. Two approaches were tried and both dropped:

**Auto-muting the quiet pane.** Rejected before implementation: muting risks
hiding real text, and the point of this program is not losing any.

**Dimming the pane that is not producing.** Built, tested, and removed. The
signal it relied on — that wrong-language output is short fragments with little
text per line — does not hold. Spanish transcribing English produces
normal-length, plausible-looking words ("la Clem", "Texas or Rollery"), so both
panes look equally healthy to any measure of text *shape*.

The conclusion: **text statistics cannot separate a good transcript from a
fluent-looking bad one.** A real solution needs the language of the audio to be
known, which means language identification — a model in the middle of the
pipeline and the compute cost that comes with it. That trade-off has not been
made yet.

---

## Download and run

1. Open the
   [Releases page](https://github.com/Igna-Mendez/Open-Local-Audio-Scribe-OLAS/releases).
2. Download the latest `OLAS-win64-1.1.x.zip`.
3. Extract it anywhere.
4. Double-click **`OLAS.bat`**.

First launch takes a few seconds while the models load, and asks which English
model you want. Play any audio on the machine and captions start within a
second or two of speech.

**Requirements**

- Windows 10 21H2 or newer (Windows 11 recommended)
- x64 CPU with AVX2 — Intel Haswell (2013) or AMD Excavator (2015) and newer
- 4+ logical CPU threads recommended
- ~800 MB free disk (three models)
- No GPU required

**What's in the zip**

```
OLAS-win64/
    OLAS.bat              <- double-click this
    olas_win.exe
    onnxruntime.dll
    models/
        medium-streaming-en/
        small-streaming-en/
        small-streaming-es/
    README.md
    LICENSE
```

`olas-model.txt` is written beside the exe on first run to remember your mode.

---

## Using the app

**Pane header** (one per language):

- **Stop / Start** — pauses or resumes that language. While stopped, the pane
  uses no CPU.
- **▾ / ▸** — collapse the pane to a narrow strip.
- **Decouple / Recouple** — detach the pane into its own OS window.
- **Follow** — re-engage auto-scroll after scrolling up to read history.

**Toolbar**

- **Options ▾** — capture device, English model (Normal / Potato), zoom,
  timestamps, appearance, auto-scroll.
- **Restore** — rebuilds a pane whose window got into a bad state.
- **Focus** — hides the toolbar and pane headers so the transcript fills the
  window.

---

## Command-line options

| Flag | Description | Default |
|---|---|---|
| `-l, --language CODE[,CODE]` | Language codes | `en,es` |
| `-m, --model PATH[,PATH]` | Model directory per language | `models\<arch>-<lang>` |
| `-a, --arch N[,N]` | 0=Tiny, 1=Base, 2=TinyStreaming, 4=SmallStreaming, 5=MediumStreaming | from `olas-model.txt`; Medium first run |
| `-q, --chunk-ms MS` | Capture chunk (20..1000) | `50` |
| `-v, --verbose` | Write `olas-debug.log` | off |
| `-c, --config PATH` | Transcription parameters file | `olas-win.conf` |
| `--no-update-check` | Skip the release check | off |
| `--stats` | Print inference diagnostics on exit | off |
| `-h, --help` | Show help | |

`3=BaseStreaming` is rejected — declared in Moonshine's C API for
forward-compatibility but not supported.

**Examples**

```bat
olas_win.exe -l en,es                :: default: Normal mode, asks on first run
olas_win.exe -l en,es -a 4,4 ^
  -m "models\small-streaming-en,models\small-streaming-es"   :: Potato mode
olas_win.exe -l en                   :: English only, lower CPU
olas_win.exe -l en,es -v             :: verbose diagnostics
```

---

## Transcription parameters

The VAD and re-decode parameters are read from **`olas-win.conf`** (next to the
exe). Bad values and unknown keys produce a warning on stderr; they never stop
the app starting. At startup the resolved configuration is printed:

```
config: olas-win.conf  vad=0.50 max_seg=12s interval=1.00s
```

| Setting | Default | Why |
| --- | --- | --- |
| `vad_threshold` | 0.5 | Above the default chops conversational turns into fragments |
| `vad_max_segment_duration` | 12 s | 6 s split an interpreter's sentence in half; Moonshine's own default is 15 |
| `transcription_interval` | 1.0 | The dominant CPU lever, and it does not change the transcript |

Thread count, CPU affinity and the model choice are **not** set here: they are
derived at startup from the detected hardware and from `olas-model.txt`.

---

## Models

| pane | Normal mode | Potato mode |
| ---- | ----------- | ----------- |
| English | Medium Streaming (more accurate) | Small Streaming (lighter) |
| Spanish | Small Streaming | Small Streaming |

The release ships all three. For source builds:

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch-streaming-models.ps1
```

The script probes known versions and downloads 8 files per model. If it 404s,
Moonshine has published a newer dated folder; the script's `$KnownVersions`
list is where you add it.

---

## Update check

On start OLAS asks GitHub whether a newer **release** exists. If one does, a
prompt shows the release name and version with a button that opens its page.

It queries `https://api.github.com/repos/<owner>/<repo>/releases/latest`, not a
branch tip: a branch tip moves on every commit, so comparing against it would
report an update for work that was never published. The repository is set by
`OLAS_UPDATE_REPO` in `CMakeLists.txt`; leave it empty to disable the check
entirely, or pass `--no-update-check` at run time.

---

## Building from source

**Prerequisites**

- Visual Studio 2022 Build Tools with "Desktop development with C++"
- CMake >= 3.20
- Git
- PowerShell 5.1+
- Ninja (optional, faster — `winget install Ninja-build.Ninja`)

No vcpkg. ONNX Runtime is built as part of Moonshine's CMake project, and the
Moonshine sources are pulled via `FetchContent` on first configure.

**Build**

Run `setup.ps1` first. It checks the toolchain, fetches all three models into
`models\`, and prints the exact configure command:

```bat
git clone https://github.com/Igna-Mendez/Open-Local-Audio-Scribe-OLAS.git
cd Open-Local-Audio-Scribe-OLAS
powershell -ExecutionPolicy Bypass -File setup.ps1

mkdir build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_MODE=bundled ..
cmake --build . --config Release -j
```

Produces `build\olas_win.exe`. First configure takes 5–15 minutes: it fetches
the Moonshine sources and builds ONNX Runtime from them. Subsequent builds are
seconds.

`setup.ps1` is idempotent — re-run it any time to repair or top up. It is only
needed for source builds; the release zip already contains the models.

Options: `-Force` (re-download), `-SkipModels`, `-Languages en,es`.

**Build a release zip**

The `zip` target packages the exe, `onnxruntime.dll`, the models, the launcher
and the docs into one archive, so an end user needs nothing beyond the zip.
Run `setup.ps1` first so the models are present:

```bat
cmake --build . --config Release --target zip -j
```

Creates `build\dist\OLAS-win64-<version>.zip`. If `models\` is empty the
package still builds, with a warning — but it will not be runnable.

**Cross-compile (Linux to Windows)**

```sh
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake
cmake --build build-mingw
```

Debug builds are rejected at configure time — 5× slower and never useful for
testing transcription quality.

---

## Architecture

```
WASAPI loopback (miniaudio)
        |  16 kHz mono s16, 50 ms chunks
        v
capture.c ring buffer
        |  capture_read_chunk()
        v
main_win.cpp capture thread
        |  Engine::feed() -> per-slot AudioQueue (lossless, non-blocking)
        v
Worker thread (one per language)
        |  Moonshine Transcriber (streaming)
        v
PrintListener -> TranscriptSink (Win32Sink)
        |-- PostMessage -> Win32 RichEdit
        |-- fwrite -> olas-moonshine-notes.txt
```

Each language runs on its own worker thread with its own `Transcriber`, on its
own CPU core set. The engine handles the shared capture feed, per-slot pause
accounting, and graceful shutdown.

**Source layout**

```
src/
    main_win.cpp         entry point, CLI, capture thread
    moonshine_engine.*   Engine, worker slots, Streaming/NonStreaming workers
    resource_plan.*      CPU detection, per-model core split, affinity
    model_choice.*       persisted English model selection (Normal/Potato)
    win32_ui.*           panes, toolbar overlay, buttons, popup menu
    capture.c / .h       WASAPI loopback + ring buffer
    miniaudio.h          vendored miniaudio 0.11
    miniaudio_impl.c
    update_check.*       GitHub release check
    transcript_sink.h    engine <-> UI abstraction
tools/
    fetch-streaming-models.ps1
```

---

## Reporting bugs

Open an issue at
<https://github.com/Igna-Mendez/Open-Local-Audio-Scribe-OLAS/issues> with:

- Windows version and CPU model
- `olas_win.exe -l en,es -v --stats` output (`olas-debug.log` has the latency
  numbers that make failure modes obvious)
- Which mode you are running
- What you expected vs. what happened

---

## License

MIT. Moonshine and miniaudio are both MIT; see `LICENSE` for the full text.
