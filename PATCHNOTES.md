# OLAS (Windows) — patch notes and research notes

Changelog and the measurements behind the version 1.1 decisions. For what the
program is and how to use it, see [README.md](README.md).

Most measurements were taken on the Linux build of the same engine (Ryzen
5600X, real speech); the Windows build shares the logic and the reasoning.

---

## 1.1

### Threading: let ONNX Runtime manage its own pool

`MOONSHINE_ORT_SINGLE_THREAD` is a **boolean, not a thread count**. Verified by
reading `/proc/<pid>/task` while running:

| value set | ORT worker threads |
| --------- | ------------------ |
| `1` | 1 |
| `2` | 1 |
| `4` | 1 |
| `8` | 1 |
| unset | 26 |

Any value forces exactly one thread; it cannot be used to request N threads.
The only real choice is *set* or *unset*, and the pool must then be bounded
with CPU affinity.

Measured on real speech (Medium Streaming, 44 s, three runs each, variance
under 2%):

| mode | RTF |
| ---- | --- |
| variable set (1 thread) | 0.733 / 0.724 / 0.718 → **0.725** |
| variable unset | 0.467 / 0.462 / 0.449 → **0.459** |

RTF is `decode_wall / audio_duration`; below 1.0 keeps up with a live
microphone. Unsetting the variable is **~37% faster**, so 1.1 leaves it unset
by default.

> An earlier note in this project concluded from the same runs that "1 thread
> is fastest and 2/4/8 are equal or slower". That was wrong: every one of those
> runs was single-threaded, so nothing was being compared.

### Per-model core split

The two models get separate core sets so their ORT pools do not contend.
Measured with Medium+Small over one shared 4-core mask, the slowest model sat
at RTF ~0.95 — close enough to 1.0 to be fragile. Split, it drops to ~0.81.

The split is applied at session creation, because that is when ONNX Runtime
sizes its pool, and the process widens back afterwards so capture and UI are
not confined to one model's cores.

Dual Small, 1+1 against 2+2, 44 s of speech, three runs each:

| split | mean RTF | worst |
| ----- | -------- | ----- |
| **1 + 1** (Potato) | **0.566** | **0.597** |
| 2 + 2 | 0.574 | 0.650 |

1+1 is marginally faster and noticeably steadier (the 2+2 pairs diverge, one
instance ~0.50 and the other ~0.65), for half the CPU. Hence Potato mode.

### The audio queue is now lossless and non-blocking

Two properties, pulling opposite ways:

- **Never block.** The original design blocked the producer once the queue
  filled. Because the capture thread feeds every pane in turn, one slow model
  froze the capture thread and starved *both* panes; the backlog then grew
  without bound and the transcript drifted permanently behind.
- **Never discard.** Dropping the oldest chunk keeps a caption live but deletes
  speech, which is not acceptable here.

The 1.1 queue therefore: never blocks, never drops, warns once when the backlog
passes 10 s, and keeps a 60 s hard ceiling purely as a memory guard (any
discard is logged loudly and counted in `--stats`).

### `transcription_interval` 0.5 → 1.0

The main CPU dial, and it does **not** change the transcript. Measured on real
speech with Medium, 44 s of audio: 69 s of wall clock at 0.5, 58 s at 2.0, for
identical output. Higher costs less CPU and updates the screen less often.

### Normal and Potato modes

English ships two models, so the app has two modes:

| mode | English | Spanish | cores |
| ---- | ------- | ------- | ----- |
| Normal (default) | Medium Streaming | Small Streaming | 3 + 1 |
| Potato | Small Streaming | Small Streaming | 1 + 1 |

The English model is chosen on first run and remembered in `olas-model.txt`
beside the executable; **Options → English model** changes it later and asks
for a restart. The build ships all three models so either mode works offline.

### Resource plan at startup

The detected core count decides the thread budget and the affinity mask, rather
than a fixed value.

### Update check fixed

Three separate bugs:

- The repository was `Igna-Mendez/Olas`, not the release repository.
- It queried `/commits/main` — a branch tip, which moves on every commit, so it
  reported an update for work that was never published. It now queries
  `/releases/latest` and compares release tags, showing the release name and
  version.
- The dialog showed mojibake: `wsprintfW(buf, L"%.12s", sha.c_str())` passed a
  narrow `char*` to a wide `%s`, so the bytes were reinterpreted as UTF-16. The
  strings are now converted explicitly and the message built from wide text.

### UI fixes

- **Text padding.** The RichEdit clipped text to its client rect, so on a
  maximised window the first line touched the top edge. It now gets inner
  padding via `EM_SETRECTNP`, re-applied on every resize.
- **Status bar removed.** Its "listening…" text did not blend with the panes.
  Transient notices go to stderr; the one actionable message (only one pane can
  be decoupled) is a dialog.
- **Lighter feel.** The banner was a 36 px saturated slab containing one centred
  word; it is now a 26 px left-aligned heading. The palette was softened and the
  dark-mode caption follows the window background rather than the accent
  colour.

### Build fixes

**Moonshine is now consumed as a prebuilt SDK, not built from source.**

CMake used to `FetchContent` the Moonshine git repository and build it, pinned
to a tag. That broke in two separate ways:

- The `v0.0.67`–`v0.0.69` tags reference
  `cpp-annote/src/community1_ort_embedded.cpp`, a file those tags do not
  contain, so CMake fails at generate time with *"Cannot find source file"* and
  *"No SOURCES given to target: moonshine"*.
- From `v0.1.0` the repository was restructured and the public headers
  (`moonshine-cpp.h`, `moonshine-c-api.h`) left the source tree entirely. No
  `v0.1.x` tag has them anywhere.

So there is no usable tag: the old ones cannot configure, the new ones have no
headers. A survey of the tags:

| tag | `core/moonshine-cpp.h` | broken `community1` ref |
| --- | ---------------------- | ----------------------- |
| v0.0.60 | no | no |
| v0.0.65 | **yes** | no |
| v0.0.67 | no | yes |
| v0.0.68 | no | yes |
| v0.0.69 | no | yes |
| v0.1.x | no | no |

The build now downloads the released Windows SDK instead:

```
https://github.com/moonshine-ai/moonshine/releases/latest/download/\
    moonshine-voice-windows-x86_64.tar.gz
```

That archive carries `include/` (both headers), `lib/moonshine.lib` and the
other import libraries, and `lib/onnxruntime.dll`. Because the URL ends in
`/releases/latest/` it follows the newest Moonshine release — no pinning, and
no source-tree archaeology. The Linux build has always worked this way.

Side benefits: configure dropped from 5–15 minutes (building ONNX Runtime from
source) to under 2 minutes, and `ONNXRUNTIME_MODE` is gone since there is
nothing to choose.

Override with `-DMOONSHINE_VERSION=<tag>` to pin, or `-DMOONSHINE_SDK_DIR=<dir>`
to use an SDK already on disk.

The SDK is kept current: the fetched release is recorded in
`moonshine-sdk/.sdk-version`, each configure asks GitHub for the latest, and a
different tag triggers a re-download. `-DMOONSHINE_REFRESH=1` forces it, and
deleting `moonshine-sdk/` does the same. Offline or rate-limited, the cached
copy is kept rather than failing the configure.

As of this writing the current release is **v0.1.5** (2026-08-24).

**Other build fixes**

- `capture.c` used `atomic_uint64_t`, which MSVC defines but mingw-w64's
  `<stdatomic.h>` does not. CMake supports both toolchains, so the type is now
  named explicitly under `__MINGW32__`. Verified: the whole tree cross-compiles
  with `x86_64-w64-mingw32`.
- `setup.ps1` was written for a prebuilt-binary install and downloaded the old
  **non-streaming** `base-*` models, which 1.1 never loads. It now checks the
  toolchain and delegates model fetching to `fetch-streaming-models.ps1`.
- That script had a PS 7-only ternary while declaring `#Requires -Version 5.1`;
  PS 5.1 has no ternary operator, so it would have thrown. Removed.
- `tools/download-models.bat` deleted: it fetched the obsolete non-streaming
  models.
- PowerShell scripts are blocked by the default execution policy; run them as
  `powershell -ExecutionPolicy Bypass -File <script>.ps1`.

### Benchmarking pitfalls hit along the way

Recorded so they are not repeated:

- **Synthetic tones prove nothing.** A tone-based benchmark reported RTF 0.17;
  it was also producing `lines=0`. The model decoded nothing.
- **Warm-up matters.** A cold ONNX Runtime session reported RTF 1.19 for a model
  that runs at 0.73 warm.
- **Check the thread count, don't infer it.** The "8 threads is slower"
  conclusion came from a variable that ignores 8.

### Readability — unsolved

Each pane hears all audio, so the pane for the language nobody is speaking
still emits text. Two approaches were tried and both dropped:

- **Auto-muting the quiet pane.** Rejected before implementation: muting risks
  hiding real text, and the point of this program is not losing any.
- **Dimming the pane that is not producing.** Built, tested, and removed. The
  signal it relied on — that wrong-language output is short fragments with
  little text per line — does not hold. Spanish transcribing English produces
  normal-length, plausible-looking words ("la Clem", "Texas or Rollery"), so
  both panes look equally healthy to any measure of text *shape*.

Conclusion: **text statistics cannot separate a good transcript from a
fluent-looking bad one.** A real solution needs the language of the audio to be
known, which means language identification — a model in the middle of the
pipeline and the compute cost that comes with it. That trade-off has not been
made yet.

### No GPU path

- Moonshine's library parses only `cpu`, `coreml`, `nnapi`. Requesting `cuda` or
  any other provider is accepted at construction and then silently ignored.
- Moonshine's documentation agrees: *"Unset means CPU-only (recommended)."*

---

## 0.6

- Transcription parameters moved into `olas-win.conf` instead of being
  hard-coded, matching the Linux build.
- `vad_threshold` 0.55 → 0.5, `vad_max_segment_duration` 6 → 12,
  `transcription_interval` 0.35 → 0.5.

## 0.5

- First public source release.
