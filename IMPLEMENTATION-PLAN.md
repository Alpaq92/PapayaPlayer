# PapayaPlayer: implementation plan

Built from the decisions in [TECH-STACK.md](TECH-STACK.md) and [CODECS.md](CODECS.md)
(2026-10-26). Phases are ordered so that the riskiest unknowns are settled first and
every phase ends with something that plays.

**Sizes** are relative (S = days, M = 1–2 weeks, L = several weeks for one developer)
and are rough guesses, not estimates.

**Rules for every phase**
- Each new demuxer, parser or reader gets a **libFuzzer target and unit tests in the same
  change**, not at the end.
- Every library goes in through a **wrapper** (`demux/<name>/`, `decode/sw/<name>/`) and our
  CMake; patch files only as a last resort (TECH-STACK.md policy).
- CI must stay green on Windows, macOS, Linux, FreeBSD, NetBSD and OpenBSD, shared and static (`PAPAYA_SHARED_LIBS`).

---

## Phase 0: foundations and risk spikes

**Goal:** an empty project that builds and tests on all three platforms, and answers to
the questions that could change the plan.

### 0.1 Repository and build (M)
- `git init`; layout:
  ```
  papaya/  core/ io/ demux/ decode/{os,sw}/ render/ audio/ subs/ vis/ data/ net/ platform/
  app/     Reaktor UI
  third_party/  git submodules        patches/  (empty, last resort)
  rust/    Cargo workspace for crates  tools/    scripts (licenses, notices)
  tests/   unit/ fuzz/ conformance/ playback/
  ```
- CMake with clang everywhere (clang-cl on Windows), C11, warnings as errors for our code.
- `PAPAYA_SHARED_LIBS` option (default `ON`) and rpath / install-name handling.
- Corrosion + a "hello" Rust crate with a C ABI, built shared and static.
- Reaktor as a submodule, started with a GPU renderer.

### 0.2 Tooling (S)
- greatest + CTest; first unit test.
- License script over `third_party/*/LICENSE*` → `THIRD-PARTY-NOTICES`; `cargo-deny` allow-list.
- clang-tidy config; CodeQL.
- GitHub Actions matrix: {Windows, macOS ARM, Linux, FreeBSD, NetBSD, OpenBSD} × {shared, static} + ASan/UBSan job. BSDs run in VMs on Linux runners.

### 0.3 Risk spikes (each S–M; results recorded in TECH-STACK.md)
| Spike | Question | If it fails |
|---|---|---|
| **S1** libxaac on Windows (clang-cl) | Do Ittiam libraries build on Windows? | Find out what breaks before S2 |
| **S2** libavc, libhevc, libmpeg2 on Windows | Same, plus asm/intrinsics | hevc.js for HEVC; C fallbacks instead of asm; forced-include shims |
| **S3** NV12 + P010 texture in Reaktor, HDR10 output | Does the SDL renderer path work for video and HDR on D3D11 / Metal / OpenGL-Vulkan? | Separate render pass or SDL_GPU for video |
| **S4** SDL GPU render state + SDL_shadercross | Can we run our HLSL shaders inside the renderer? | Separate pass |
| **S5** Reaktor CSS | Gradients, outlined rounded bars, pill buttons? | Extend Reaktor or simplify the design |
| **S6** miniaudio non-PCM (IEC 61937) output | Passthrough through miniaudio? | Small own passthrough backend later |
| **S7** cros-codecs C ABI | Can we call its parsers from C? | Chromium parser port for the hardware path |
| **S8** BSD baseline | Do SDL3, Reaktor, Rust (Corrosion), miniaudio and the Ittiam libs build and run on FreeBSD, NetBSD and OpenBSD? | Fix in our CMake / wrappers; report upstream |

**Exit:** green CI on all platforms; spike results written down; plan adjusted if needed.

---

## Phase 1: core pipeline (first playback)

**Goal:** play WebM/MKV with AV1 or VP9 video and Opus/Vorbis/FLAC audio, in sync,
with seeking. Software decoding only. Chosen first because libwebm, dav1d, libvpx
and the audio libraries need no porting.

| Item | Size |
|---|---|
| `io/`: file source with read/seek callbacks (network-ready interface) | S |
| `core/`: packet and frame structs, ref-counted frame pool, bounded queues (duration + bytes) | M |
| `core/`: threads (demux, video decode, audio decode), state machine (open, play, pause, seek, stop, end) | M |
| `core/`: master clock (audio device position − latency; system clock fallback), frame show/drop | M |
| `demux/`: registry + probe; **libwebm** wrapper (C ABI over mkvparser) | M |
| `decode/sw/`: registry; **dav1d**, **libvpx**, **libopus**, **libvorbis** + libogg, **libFLAC** wrappers | M |
| `audio/`: **miniaudio** output, built-in resampler, our downmix | S |
| `render/`: SDL streaming texture (NV12 / I420 / P010), aspect ratio, bilinear scaling | S |
| Seeking: keyframe seek + exact seek (decode and drop) | S |
| Minimal test UI: open file, play/pause, seek bar | S |

**Exit:** WebM/MKV test files play with correct A/V sync and seek on all three platforms;
TSan clean on the core; fuzz target for the libwebm wrapper.

---

## Phase 2: mainstream formats and HDR

**Goal:** the files most people have: MP4 with H.264/HEVC + AAC, MP3/FLAC/Ogg/WAV music,
HDR10 video.

| Item | Size |
|---|---|
| **MP4 demuxer: port Media3 `mp4/`** (Mp4Extractor, FragmentedMp4Extractor, BoxParser) to C | **L** |
| `decode/sw/`: **libavc**, **libhevc** (10-bit via `u4_enable_hbd`), **libxaac** wrappers | M |
| Audio-only demuxers: MP3, ADTS, FLAC, WAV, Ogg (own or Android extractors) + **minimp3** / **dr_wav** / **dr_flac** | M |
| Gapless audio: encoder delay / padding (LAME, iTunSMPB, Opus pre-skip) | S |
| MPEG-TS / PS demuxer: decide Android ATSParser vs our own, then build it | M–L |
| HLSL + SDL_shadercross build step; YUV→RGB shader with correct matrices and ranges | M |
| HDR10: P010 + BT.2020/PQ output on HDR screens; **BT.2390 tone mapping** on SDR screens | M |
| Colour metadata from containers and bitstreams (VUI, SEI, `colr`/`mdcv`/`clli`, MKV Colour) | S |

**Exit:** MP4 (normal + fragmented), TS and the music formats play; HDR10 correct on HDR
and SDR screens; conformance runs for H.264 / HEVC / AV1 in CI.

---

## Phase 3: player UI and visualiser

**Goal:** the PapayaPlayer look from TECH-STACK.md "UI direction".

| Item | Size |
|---|---|
| Mockups of the main screens (video, audio, playlist, settings, visualiser) to agree the design before building | S |
| Video layout (minimal and accent styles): seek bar, play, ±10 s, time, volume, PiP, settings, fullscreen, **previous / next** | M |
| Audio layout: round play button, `<` `>`, title, time, progress, ±15 s, volume, speed, playlist | M |
| Settings menu (tracks, speed, subtitles), keyboard shortcuts, drag-and-drop, command-line paths | M |
| **cavacore** + our `fftw3.h` shim over **KISS FFT** | S |
| Visualiser styles (bars, wave, blocks), placement, colour sources incl. **album-art colours**, theme presets | M |

**Exit:** usable day-to-day player for local files.

---

## Phase 4: subtitles and more formats

| Item | Size |
|---|---|
| Subtitle pipeline: cue timing against the clock, track selection, Reaktor rendering, MKV embedded fonts | M |
| Ports from Media3: **SRT, WebVTT, SSA/ASS (basic), PGS** (+ window/composition), **VobSub**, **DVB**, **CEA-608/708** | L |
| Standalone VobSub `.idx/.sub` reader (modelled on emk `vobsub`) | S |
| SRT encoding detection (compact_enc_det) + conversion to UTF-8 | S |
| Ports from Media3: **AVI** (+ OpenDML), **FLV** | M |
| `decode/sw/`: **libmpeg2**, Android **m4v_h263**, **ALAC**, **libwavpack**, Android **AMR** | M |

**Exit:** DVD rips, old AVI/DivX files, Blu-ray PGS subtitles and broadcast captions work.

---

## Phase 5: hardware decoding and deinterlacing

| Item | Size |
|---|---|
| Hardware backend interface, automatic fallback to software, user setting | S |
| **Windows: Media Foundation** (H.264, HEVC with extension, VP9, AV1, VC-1, WMV, WMA), DXGI zero-copy | L |
| **macOS: VideoToolbox** (H.264, HEVC, ProRes, VP9, AV1 on M3+) + CVPixelBuffer / Metal zero-copy | M |
| **Linux and BSDs: VA-API** with **cros-codecs** parsers (H.264, HEVC, VP8, VP9, AV1), EGL / Vulkan zero-copy | L |
| Deinterlacing: D3D11 video processor, VA-API post-processing, HLSL bob / linear fallback | M |
| Hardware frames in the frame pool (D3D11 texture, CVPixelBuffer, VA surface) | M |

**Exit:** 4K HEVC 10-bit plays with low CPU where hardware decoding exists; failures fall back cleanly.

---

## Phase 6: audio/video extras and data

| Item | Size |
|---|---|
| Playback speed with **signalsmith-stretch** | S |
| Loudness: ReplayGain / R128 tags + **libebur128** background measurement, cached | M |
| Tag readers: ID3v1/v2, Vorbis comments, MP4 atoms, MKV tags, APEv2; cover art (stb_image) | M |
| **SQLite**: history, resume positions | S |
| **settings.toml** via tomlc17 (+ our writer) | S |
| Playlists: **M3U/M3U8, PLS, XSPF** (expat), **CUE** | M |
| Bicubic / Lanczos scaling shaders (quality setting) | S |

---

## Phase 7: OS integration

| Item | Size |
|---|---|
| Media keys + "now playing": **SMTC** (Windows), **MPNowPlayingInfoCenter** (macOS), **MPRIS** with runtime libdbus (Linux, BSDs) | M |
| Single instance (named pipe / local socket; native on macOS), replace or enqueue | S |
| File associations: in-app settings page (installer part in phase 10) | S |
| Prevent sleep, Windows taskbar progress + buttons, Jump List / Dock recent files | S |

---

## Phase 8: networking (required for v1)

| Item | Size |
|---|---|
| `net/`: **libcurl** source behind the `io/` callbacks, **mbedTLS** as its TLS backend on every platform (bundled), OS certificate stores loaded per platform; range requests, redirects, buffering UI | M |
| Progressive HTTP files and Icecast / SHOUTcast radio (ICY metadata) | S |
| **HLS** port from Media3 `exoplayer_hls`: playlists, segments (TS + fMP4), AES-128, bitrate switching | L |
| **DASH** port from Media3 `exoplayer_dash`, manifests through expat | L |
| **RTSP/RTP** port from Media3 `exoplayer_rtsp` | L |
| Check Media3 module contents first (still unverified) | S |

| Optional: **Chromecast sender**, our own C client (TECH-STACK.md, Chromecast): mDNS discovery, Cast v2 over curl + mbedTLS, Default Media Receiver, built-in HTTP server for the file, MKV remuxed to fragmented MP4, Cast button + device picker | M |

**Exit:** public HLS / DASH test streams and an RTSP camera play.

---

## Phase 9: UI refinement

**Goal:** turn the working UI from phase 3, plus everything phases 4–8 added to it
(subtitle and track menus, hardware-decoding settings, playlists, library data, "Open
URL", streaming status), into a consistent, polished product. Driven by feedback from
the 0.3 testers.

| Item | Size |
|---|---|
| Collect and triage tester feedback from 0.3 / 0.5; UI issue list | S |
| Full design pass: high-fidelity mockups of every screen and dialog, light and dark themes | M |
| Consistency: spacing, typography, one icon set (permissive, e.g. Lucide ISC or Material Symbols Apache-2.0, unverified), hover / focus / pressed states | M |
| Motion: auto-hiding controls in fullscreen, transitions, seek-bar thumbnail previews | M |
| Window sizes: compact / mini-player mode, HiDPI and fractional scaling on every platform, multi-monitor | M |
| Visualiser polish: theme gallery, album-art colours, performance at 4K / 144 Hz | S |
| Accessibility: full keyboard navigation, visible focus, contrast checks, screen-reader labels (depends on Reaktor) | M |
| Localisation: string extraction, translation files, right-to-left layout check | M |
| Per-platform touches: macOS menu bar and shortcuts, Windows title bar / snap layouts, Linux / BSD desktop integration | S |
| UI performance: idle CPU near zero, no dropped video frames while controls animate | S |
| Usability round with testers on the refined build | S |

**Exit:** UI review checklist passes on all six platforms; testers' top issues closed.

---

## Phase 10: hardening and release

| Item | Size |
|---|---|
| Long fuzzing runs (nightly), OSS-Fuzz application | S + ongoing |
| Full conformance suites in CI; playback corpus (open, sync, seek checks) | M |
| Crash reporting, logging, settings migration | S |
| **NSIS** installer (file associations, uninstaller) | S |
| **Flatpak / Flathub, AppImage, .deb / .rpm, AUR** | M |
| **FreeBSD ports / pkg, OpenBSD ports, NetBSD pkgsrc**, release tarball | M |
| macOS universal app bundle (unsigned for now) | S |
| **WinSparkle + Sparkle** with one signed appcast | S |
| Decide: Windows code signing, macOS signing + notarisation | — |

---

## Phase 11: extended codecs (after v1 unless pulled forward)

| Item | Size |
|---|---|
| **Provenance spot-checks** of every ⚠️ crate/library against FFmpeg before adoption | M |
| AC-3: **pschatzmann/codec-ac3** or **PasyDev/ac3psy** wrapper | S |
| Rust through Corrosion: **OpenJOC** (E-AC-3), **oxideav-dts** (DTS core), **truehdd** (TrueHD), **oxideav-prores** (ProRes), **oxideav-ac4** (AC-4), **filmcraft-dnx** (DNxHD/HR, once its source is public) | M each |
| Our own test-file campaign for each (no official conformance) | M |
| VC-1 hardware path on Linux: Intel UMC parser → VA-API | M |
| **Passthrough** (IEC 61937) to AV receivers, exclusive-mode output | M |
| D3D11VA backend (HEVC without Microsoft's extension), NVDEC | L |
| **DVD menus**: oxideav-dvd (provenance check, C ABI, VOB into our MPEG-PS path, SPU menus through Reaktor) | M |
| **Blu-ray**: oxideav-bluray + our Interactive Graphics menu layer (libhdmv as reference) | L |

---

## Releases

| Release | Contents |
|---|---|
| **0.1** (internal) | Phase 0 + 1: WebM/MKV playback |
| **0.2** | + Phase 2: MP4, TS, music formats, HDR |
| **0.3** (first testers) | + Phase 3 + 4: real UI, visualiser, subtitles, AVI/FLV |
| **0.5** | + Phase 5 + 6 + 7: hardware decoding, extras, library data, OS integration |
| **1.0** | + Phase 8 + 9 + 10: networking (HTTP, HLS, DASH, RTSP), UI refinement, packaging, auto-update |
| **1.x** | Phase 11: Dolby / DTS / TrueHD / ProRes / AC-4 decoders, passthrough, D3D11VA, DVD and Blu-ray menus |

## Dependencies between phases

```
0 ─► 1 ─► 2 ─┬─► 3 ─► 4 ─────────────────────┐
             ├─► 5 (needs 2's frame/colour path) ├─► 9 UI refinement ─► 10 ─► 1.0
             ├─► 6 ─► 7                        │
             └─► 8 (needs 2's TS + fMP4) ──────┘
                                  11 (any time after 2; after 1.0 by default)
```

Phases 3–8 can overlap once phase 2 is done. The MP4 port (phase 2) and the Media3
subtitle/container ports (phase 4) are the largest pure-porting jobs and can start early
in parallel if there is a second developer.

## Biggest risks

1. **Ittiam libraries on Windows** (spikes S1/S2). Backup: hevc.js for HEVC; OS decoders.
2. **Media3 porting volume** (MP4, HLS, DASH, RTSP, subtitles). Backup: ship v1 with fewer
   streaming features; port in order of use.
3. **Reaktor rendering limits** for video, HDR and the design (S3–S5). Backup: separate
   render pass; extend Reaktor.
4. **Provenance of AI-written codecs** (phase 11). Backup: OS decoders and passthrough.
5. **Zero-copy hardware interop** with SDL's renderer (phase 5). Backup: copy-back path
   (slower, still works).
