# PapayaPlayer: tech stack decisions

Decisions made step by step. Codec choices are in [CODECS.md](CODECS.md);
research and evidence in [BACKEND-RESEARCH.md](BACKEND-RESEARCH.md).

## Platforms (decided 2026-10-26)

**Windows, macOS, Linux, FreeBSD, NetBSD and OpenBSD**, all at the same level:
built and tested in CI on every change and released with every version. Where a BSD
lacks OS support (e.g. hardware decoding), the feature falls back to software.

| Area | BSD notes |
|---|---|
| Compiler | clang (base on FreeBSD / OpenBSD; from pkgsrc on NetBSD, whose default is GCC) |
| Rust | Available on all three, at lower Rust support tiers (OpenBSD lowest) (unverified) |
| SDL3, Reaktor | SDL3 BSD support and Reaktor on BSD to be checked (phase 0 spike) |
| Audio | miniaudio backends: OSS (FreeBSD), sndio (OpenBSD), audio(4) (NetBSD) (unverified) |
| Hardware decoding | VA-API where available (FreeBSD; OpenBSD / NetBSD weaker, unverified); otherwise software |
| TLS | **mbedTLS**, bundled, as on every platform (see step 10) |
| Media keys | MPRIS over D-Bus, as on Linux |
| Packaging | FreeBSD ports / pkg, OpenBSD ports, NetBSD pkgsrc, plus a release tarball |
| CI | BSD virtual machines inside GitHub Actions (e.g. cross-platform-actions, unverified) |

## Step 1: language and build (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| C standard | **C11** | `stdatomic.h`, `_Static_assert`, `alignas`. Threads through SDL3, not `threads.h`. Reaktor stays C99 |
| Compiler | **Clang everywhere** | clang-cl on Windows, Apple clang on macOS, clang on Linux |
| Build system | **CMake** | dav1d (Meson) and libvpx (configure) built through ExternalProject or our own CMake files |
| Rust | **Yes, link Rust crates** | For E-AC-3, DTS core, TrueHD and ProRes (see CODECS.md). C ABIs; shared or static per `PAPAYA_SHARED_LIBS` (step 2) |

## Step 2: dependencies (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| Bringing in C/C++ libraries | **Git submodules** | `third_party/<name>`, each pinned to a commit. The project folder isn't a git repository yet; it needs `git init` first |
| Patches to third-party code | **Wrapper first; patch files only as a last resort** | See the policy below |
| Linking | **Shared libraries by default; CMake option for static** | `PAPAYA_SHARED_LIBS` (default `ON`) sets `BUILD_SHARED_LIBS` for every third-party library and our own modules; `OFF` builds them all as static libraries linked into one executable. Shared builds on Windows need exported symbols: the Ittiam libraries have no export macros, so use `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` or `.def` files. Rust crates declare both `cdylib` and `staticlib`. macOS needs `@rpath` install names, Linux `$ORIGIN` rpath |
| Rust in CMake | **Corrosion** (MIT) | Imports each crate as a CMake target |
| License enforcement | **cargo-deny + our own script** | cargo-deny allow-list for every Rust crate and sub-dependency; our script checks `third_party/*/LICENSE*` and generates `THIRD-PARTY-NOTICES`. Both in CI |

### Third-party code policy: wrapper first

1. **Wrapper first.** Every library gets a wrapper in our code (`demux/<name>/`,
   `decode/sw/<name>/`) that adapts it to our packet and frame formats and works
   around what it lacks (e.g. HDR metadata from the container, SEI parsed ourselves).
2. **Build fixes in our CMake**, never in the library's files: compile definitions,
   flags, forced-include headers (`-include` / `/FI`), shim headers earlier on the
   include path, C fallbacks instead of asm files, our own CMake targets for
   libraries without CMake, `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` / `.def` for exports.
3. **Patch files only as a last resort** (a bug inside decoding logic, an unreleased
   security fix, something the API doesn't expose): `patches/<name>/*.patch`,
   applied by CMake, each with a pull request sent upstream and removed once merged.

## Step 3: video output (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| Renderer | **SDL renderer, native backend** | Video as an NV12/P010 SDL texture on Reaktor's renderer: D3D11 on Windows, Metal on macOS, OpenGL or Vulkan on Linux. Reaktor's default software renderer can't be used for video or HDR, so the player always starts Reaktor with a GPU renderer. Zero-copy follows SDL's `test/testffmpeg.c` |
| HDR | **HDR10 output + SDR tone mapping** | SDL3 HDR output colorspace on HDR screens (P010, BT.2020/PQ); our own BT.2390 tone-mapping shader on SDR screens |
| Shaders | **HLSL + SDL_shadercross** (zlib) | Compiled at build time to DXIL/DXBC, SPIR-V and MSL. Hooked into the renderer through SDL's GPU render state (SDL 3.4+, unverified) or a separate pass |

## Step 4: audio output (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| Output library | **miniaudio** (public domain / MIT-0) | WASAPI shared + exclusive, CoreAudio, ALSA / PulseAudio / PipeWire. Its device latency feeds the master clock. Reaktor's SDL stays built without audio |
| Resampler | **miniaudio's built-in resampler** | Swap for speexdsp (BSD) or r8brain (MIT) later if listening tests need it |
| Passthrough (AC-3 / E-AC-3 / DTS / TrueHD over HDMI or S/PDIF) | **Later; designed for now** | First version decodes to PCM. The audio path lets a packet bypass the decoder and be wrapped in IEC 61937 for an exclusive-mode device. Whether miniaudio can open a device with a non-PCM (IEC 61937) format is unverified; if not, passthrough gets its own small WASAPI / CoreAudio / ALSA backend |

## Step 5: player core (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| Threads | **Thread per stage** | Demux, video decode, audio decode, plus miniaudio's callback thread and Reaktor's UI thread. Bounded queues between stages. Decode threads call `reaktor_wake()` when a frame is ready |
| Master clock | **Audio, falling back to the system clock** | Device play position minus output latency. Video frames shown, repeated or dropped against it. Video-only files use the monotonic system clock. A display-clock (vsync) mode can be added later as an option |
| Queue limits | **By duration and bytes** | Packet queues: a few seconds or N MB, whichever first. Frame queues: 3–8 video frames, ~100–300 ms of audio. Exact numbers tuned later |
| Frame memory | **Ref-counted frame pool** | Our frame struct (planes, strides, pixel format, colour info, timestamps), reference counted, reused from a per-stream pool. Hardware frames (D3D11 texture, CVPixelBuffer, VA surface) wrapped in the same struct for zero-copy |

## Step 6: hardware decoding (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| Windows | **Media Foundation first, D3D11VA later** | MF is frame-level and also covers VC-1 / WMV / WMA. MF HEVC needs Microsoft's HEVC Video Extension; D3D11VA later removes that need |
| macOS | **VideoToolbox** | Only option. Frame-level |
| Linux, BSDs | **VA-API first** | Intel / AMD through Mesa, NVIDIA through nvidia-vaapi-driver. NVDEC can come later |
| Parsers for slice-level APIs | **cros-codecs** (ChromeOS, Rust) | Checked 2026-10-26: BSD-3-Clause, last commit 2026-04-04, not archived. Codecs: AV1, H.264, H.265, VP8, VP9 (no MPEG-2 or VC-1: VC-1 uses Intel's MIT parser, see CODECS.md). Backends: VA-API and V4L2 only, so a D3D11VA backend is ours to write later. C ABI and provenance still to check |
| Default | **Hardware on, automatic fallback** | Falls back to software if hardware can't take the stream (profile, size) or fails mid-stream. Setting to turn it off |

## Step 7: audio and video extras (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| Playback speed (pitch preserved) | **signalsmith-stretch** | Checked: MIT, C++ header library, active (2026-10-22). Depends on Signalsmith Linear (FFT); optional faster FFT backends (Accelerate, IPP, PFFFT) |
| Loudness | **ReplayGain / R128 tags + libebur128** | Checked: MIT, C, last commit 2023-06. Measures untagged files in the background, results cached. Off / track / album modes |
| Deinterlacing | **OS hardware deinterlacer + our own shader** | D3D11 video processor, VA-API post-processing; HLSL fallback starting with bob/linear, motion-adaptive later. yadif / bwdif excluded (FFmpeg code) |
| Scaling | **Bilinear now, bicubic / Lanczos later** | Bilinear from SDL textures; our own HLSL filters later as a quality setting |
| HDR tone mapping | BT.2390 shader | Decided in step 3 |
| Downmix 5.1 / 7.1 → stereo | Our own, standard coefficients | |

## Step 8: data (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| History, resume positions, future library | **SQLite** | Public domain, amalgamation (one C file) |
| Settings | **TOML** | `settings.toml`, read with **tomlc17** (MIT, active; replaces the obsolete tomlc99). Writing the file back is our own small writer unless tomlc17 can emit TOML (unverified) |
| Tags (our own parsers) | **ID3v1/v2, Vorbis comments, MP4 atoms, MKV tags, APEv2** | Including cover art (APIC, METADATA_BLOCK_PICTURE, `covr`) and ReplayGain / R128 tags. TagLib excluded (LGPL / MPL) |
| Playlists | **M3U / M3U8, PLS, XSPF, CUE** | XSPF through **expat** (MIT, active). CUE splits one audio file into tracks |

## Step 9: OS integration (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| Media keys, "now playing" | **Windows, macOS and Linux in the first version** | Windows: System Media Transport Controls (small C++/WinRT file). macOS: MPNowPlayingInfoCenter + MPRemoteCommandCenter (small Objective-C file). Linux: MPRIS over D-Bus, libdbus loaded at run time (system library; AFL-2.1 / GPL dual, unverified) |
| File associations | **Installer + in-app toggle** | Windows ProgIDs / Default apps, macOS Info.plist document types, Linux .desktop MIME types; settings page to choose which types to claim |
| Instances | **Single instance** | New files go to the running window (replace or enqueue, per setting). Named pipe / local socket; native on macOS |
| Extras | **Prevent sleep, Windows taskbar, drag-and-drop + "Open with", OS recent files** | Video keeps the screen awake; audio keeps only the system awake. Taskbar progress + thumbnail buttons. Jump List / Dock "Open Recent" |

## Step 10: networking (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| Scope of the first version | **HTTP(S), HLS, DASH and RTSP/RTP** | Minimum for v1. Our I/O layer uses read/seek callbacks so demuxers don't care where data comes from |
| HTTP | **libcurl** (curl license, MIT-style) | Redirects, proxies, HTTP/2, range requests |
| TLS | **mbedTLS** (Apache-2.0) on all six platforms, bundled, as curl's TLS backend (changed 2026-10-27) | One TLS library and one code path everywhere. Replaces the earlier mix of Schannel, Apple TLS, OpenSSL 3 and system LibreSSL/OpenSSL: curl dropped Apple's Secure Transport in 2025 (no TLS 1.3), so macOS needed a bundled library anyway. Costs: we ship a release for every mbedTLS security fix, and load each OS's certificate store ourselves (Windows store, macOS keychain, the distro / BSD CA bundle), since mbedTLS reads none of them; how curl does this with mbedTLS on each OS is unverified. Also provides AES-128 for encrypted HLS |
| HLS, DASH, RTSP | **Port** Media3 `exoplayer_hls`, `exoplayer_dash`, `exoplayer_rtsp` (Java, Apache-2.0) | Modules confirmed to exist; contents not yet checked. DASH manifests (XML) through expat. HLS segments use the MPEG-TS and fragmented-MP4 demuxers |

## Chromecast (proposed 2026-10-27, after networking)

| Choice | Decision | Notes |
|---|---|---|
| Sender | **Our own C client** of the Cast v2 protocol, maybe | No permissive C library exists: Google's Open Screen is C++ (BSD-3, BoringSSL, GN build), VLC's module is LGPL, the Rust crates are old or unchecked. Roughly 1,500-2,000 lines |
| Parts | **libcurl + mbedTLS** for the TLS connection to port 8009 (`CURLOPT_CONNECT_ONLY`), **cJSON** (MIT) for messages, the CastMessage protobuf by hand or **nanopb** (zlib), **mdns** (mjansson, public domain) for `_googlecast._tcp` discovery | Device authentication is optional for a sender and skipped |
| Receiver | Google's **Default Media Receiver** (no registration) | Load, play, pause, seek, volume, status |
| Media | The player serves the file over a small built-in HTTP server with range requests | The device plays only what it decodes natively (H.264, VP8/VP9, AAC/Opus; HEVC / AV1 / HDR on newer models). No re-encoding without FFmpeg; MKV gets remuxed to fragmented MP4. Subtitles as WebVTT |

## Step 11: quality (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| Unit tests | **greatest + CTest** | greatest: ISC (checked), single header, ~1.5k stars, last commit 2023-06 (stable, small). µnit (MIT) was the alternative |
| Conformance | **Official suites** | JCT-VC HEVC, JVT H.264, AV1 test vectors, AAC/MP3 conformance, compared with reference MD5s. Media3 ports tested against Media3's own test files |
| Fuzzing | **libFuzzer** | Every demuxer, parser, subtitle and playlist reader. Short runs per PR, longer nightly; OSS-Fuzz later |
| Sanitizers | **ASan + UBSan; TSan for the player core** | Separate CI builds |
| Playback tests | **Headless runs over a sample-file corpus** | Every file opens; A/V sync and seeking checked |
| CI | **GitHub Actions** | Hosted Windows, macOS (ARM) and Linux runners; FreeBSD, NetBSD and OpenBSD in VMs on Linux runners |
| Static analysis | **clang-tidy, CodeQL, warnings as errors** | Warnings as errors in our own code only, never third-party |

## UI direction (2026-10-26)

Inspired by Playerjs templates (playerjs.com), built with Reaktor:
- **Video, minimal:** black stage, large centred play icon; full-width seek bar with a
  thumb; under it a pill-shaped play button, skip back/forward 10 s, `0:03 / 0:05`, and
  on the right volume, picture-in-picture, settings (gear) and fullscreen.
- **Video, accent:** controls in a rounded, outlined bar; seek bar with a red→orange
  gradient fill; vertical volume slider with the same gradient; gradient pill dropdowns
  top left (folder / file); gradient settings menu (Quality, Speed) above the gear.
- **Audio:** large round play button with `<` `>` either side; track title and
  `0:06 / 0:47`; thin progress bar; skip ±15 s, volume slider, speed dial; playlist below
  with the current track highlighted.
- **Added:** previous / next file buttons in both video and audio layouts.
- **Audio visualiser:** cava-style bars with themes (see below).

Open: whether Reaktor's CSS supports gradients, outlined rounded bars and pill
buttons (unverified), and which elements the final design takes from each template.

### Visualiser: cavacore
- **cava** (karlstav/cava): MIT, C, active (2026-10-23). Its processing engine
  `cavacore.c` (588 lines) is a separate library: log frequency bands, smoothing,
  fall-off, automatic sensitivity. We feed it our decoded PCM and draw the bars with Reaktor.
- **Problem:** cavacore uses **FFTW, which is GPL**. It calls only a 1-D real-to-complex
  FFT (`fftw_plan_dft_r2c_1d`, `fftw_execute`, alloc/free, destroy).
- **Fix without patching (wrapper-first policy):** a small `fftw3.h` shim header of our own,
  earlier on the include path, implementing those few calls on **KISS FFT** (BSD-3, checked;
  decided 2026-10-26). cavacore uses double precision and power-of-two sizes from 512 to
  65536 (bass buffer twice that); `kiss_fftr` built with `kiss_fft_scalar=double` returns
  n/2+1 complex values, the same layout as FFTW's r2c output, so the shim only maps calls.
  Considered: pffft (faster, but packed output layout to convert), meow_fft (little
  activity), minfft (small user base).

### Visualiser themes (inspired by plasma-audio-visualizer)
luisbocanegra/plasma-audio-visualizer is **GPL-3.0** (checked), so we take **ideas only,
no code**. Its options, read from its settings UI (`configVisualizer.qml`, `FormColors.qml`):
- **Styles:** Bars, Wave (curve / square / triangle, optionally filled), Blocks
  (segmented bars, optionally showing inactive blocks).
- **Placement:** grow from top, bottom, left or right.
- **Colour sources:** fixed colour, system/theme accent, custom list (wraps around),
  gradient (horizontal or vertical), hue shift per bar, random, image (colours sampled
  from a picture, so album art would work), with alpha, saturation, lightness, reverse,
  contrast correction, and animated colours (interval, smoothing).

Our version: these styles drawn by our own code over cavacore output, with themes as
named presets (style + placement + colour source) in `settings.toml`. **Cover-art
colours** fit a music player well. Drawn with Reaktor primitives, or our own HLSL
shader if Reaktor can't draw it fast enough (unverified).

## Step 12: shipping (decided 2026-10-26)

| Choice | Decision | Notes |
|---|---|---|
| Windows | **NSIS** (zlib license) | `.exe` installer: file associations, uninstaller. Windows code signing not decided yet; unsigned installers get SmartScreen warnings |
| Linux | **Flatpak (Flathub), AppImage, .deb / .rpm, AUR** | mbedTLS is bundled everywhere; Flatpak takes VA-API from its runtime; .deb / .rpm / AUR use system libraries otherwise |
| macOS | **Unsigned for now** | Universal (ARM + Intel) app. Users right-click → Open past Gatekeeper. Signing + notarisation later (Apple Developer account) |
| BSDs | **FreeBSD ports / pkg, OpenBSD ports, NetBSD pkgsrc, release tarball** | Updates through each system's package manager |
| Auto-update | **WinSparkle (Windows) + Sparkle (macOS)**, both MIT | One appcast feed with EdDSA-signed updates. Linux updates through Flathub and package managers. How Sparkle behaves with an unsigned app is unverified |

## Open items

- **From the phase 0 spikes** (`spikes/RESULTS.md`): Reaktor needs an option for the renderer's output colorspace (HDR10 output) and, for custom shaders, one for SDL's GPU renderer (it forces `SDL_GPU` off); Reaktor's CSS has no gradients (draw them in our widgets or add them to Reaktor); SDL's renderer does YUV→RGB, HDR10 and tone mapping itself, so BT.2390 becomes optional; libavc's own CMake builds everything with AVX2, so our targets set SIMD flags per file.

- UI details: what Reaktor's CSS can draw, and which elements the design takes from each template.
- Windows code signing; macOS signing + notarisation.
- mbedTLS: loading the OS certificate stores on each platform; its major version (curl's support for mbedTLS 4 unverified).
- Checks marked (unverified) above: miniaudio non-PCM output, SDL GPU render state, cros-codecs C ABI and provenance, Media3 HLS/DASH/RTSP contents, tomlc17 writing.
- From CODECS.md: TS demuxer source, Windows builds of the Ittiam libraries, provenance checks, ASF legal check, patents.
- FFmpeg-free costs (deferred by the user).
