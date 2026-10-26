# PapayaPlayer: backend research

Research notes from 2026-10-25. Goal: a cross-platform C audio/video player with
[Reaktor](https://github.com/Alpaq92/Reaktor) as the frontend and a backend
under permissive licenses, or at least nothing copyleft inside the installer.

Items marked **(unverified)** come from memory, not from reading the code, and
must be checked before relying on them.

---

## 1. Decision so far

> **2026-10-26: no FFmpeg for the time being, not even at run time.** Codec choices and gaps: [CODECS.md](CODECS.md).

**Leading candidate: the "Firefox model".** Bundle permissive decoders and use
the OS decoders where available; load FFmpeg at run time, never bundled, as the
last fallback.

```
Video:  OS hardware decoder ─► bundled permissive ─────────────────────────► runtime FFmpeg
        (MF, VideoToolbox,     (dav1d, libvpx, libavc, libhevc, libmpeg2)    (VC-1, everything else)
         VA-API, WebCodecs)

Audio:  OS decoder ─► bundled permissive ─────────────────────────► runtime FFmpeg
                      (libxaac, libopus, libvorbis, libFLAC, dr_mp3)  (DTS, TrueHD, AC-3)

Containers: nestegg, minimp4 (patched), own TS/AVI/FLV ─► runtime FFmpeg

Output: frames ─► SDL3 texture on Reaktor's renderer · audio ─► SDL3 audio / miniaudio
```

Without FFmpeg present, this plays nearly everything **except DTS, TrueHD,
AC-3/E-AC-3, VC-1 and ASF**. With FFmpeg on the system or a codec pack,
everything plays.

**Fallback choice if this proves too much work:** FFmpeg's libraries bundled as
LGPL DLLs, using SDL3's GPU renderer, or libmpv if speed of development matters
more than HDR.

---

## 2. Frontend: Reaktor (checked in source)

- C99, MIT. Nuklear + SDL3, CSS styling. Its default renderer is software.
- `reaktor_sdl_renderer(app)` returns the SDL renderer (`include/reaktor/launch.h:156`),
  so an SDL streaming texture in NV12/IYUV/P010 can show video frames;
  `reaktor_image` is `nk_image`, which can wrap it.
- `reaktor_wake()` is atomic and safe to call from other threads
  (`runtime/app.c:1846`). The decoder thread calls it when a frame is ready
  (Reaktor draws nothing while idle).
- **SDL is built without audio** (`docs/NOTICE.md`). Re-enable it in Reaktor's
  CMake, or use miniaudio.
- GPU renderers: `--renderer direct3d12|direct3d11|opengl`. **SDL's Vulkan
  renderer is forced off** (`CMakeLists.txt:36`); check whether Metal is built.
- HDR requires a GPU renderer + SDL3 HDR output colorspace + P010 BT.2020/PQ
  textures. SDL3's software renderer can't do HDR.
- Reference for zero-copy video + HDR in SDL3: SDL's `test/testffmpeg.c`
  (zlib), which covers VA-API/EGL, D3D11 and VideoToolbox/Metal. **(unverified: check property names against SDL headers)**

---

## 3. Rejected options

| Option | Why rejected |
|---|---|
| **BMF** (ByteDance) | Its decoder is FFmpeg (`bmf/c_modules/src/ffmpeg_decoder.cpp`), so it's still LGPL. Not a player: no clock, no audio output, no display. Seeking only through `start_time` when the decoder starts (`:714`, `:900–920`) or `DynamicReset`. Heavy C++/Python/CUDA build. Its GPU strength is CUDA transcoding, not display. Only worthwhile later as an add-on stage (AI super-resolution, denoise) |
| **ffmpreg** (Rust) | 1,934 lines in total; only WAV/Y4M, PCM/ADPCM/raw. Experimental, one developer. Apache-2.0, but nothing to play |
| **libmkv** (quadrifoglio) | 5 commits, all on 2016-04-03. Only splits files into raw EBML elements: no block parsing, no Cues, whole file in memory, stops on unknown elements. WTFPL |
| GStreamer, libvlc | LGPL; heavy to package on Windows and macOS; libvlc ships GPL plugins you'd need to remove |
| **minih264** (lieff) | Encoder only (H.264 baseline/SVC); README: "Decoder can be popped up in future". Last commit 2020-12. CC0 |
| pl_mpeg, theoraplay | Permissive, but play only MPEG-1 / Theora content you convert yourself |

**FFmpeg is effectively the only open-source library that covers every format.**
mpv, VLC, GStreamer (`gst-libav`), Chromium, Kodi, OBS and BMF all decode with it.

---

## 4. Demuxers (checked in source)

### nestegg — ISC, C, ~4.3k lines, Mozilla (last commit 2026-09-16)
- Accepts WebM **and** Matroska doctypes (`nestegg.c:2722`); read/seek/tell callbacks.
- Codecs it identifies: VP8, VP9, AV1, H.264, HEVC, AAC, Vorbis, Opus, FLAC, MP3, PCM (`nestegg.h:70–81`); everything else is `NESTEGG_CODEC_UNKNOWN`.
- Video and audio track types only; **subtitle tracks come back as unknown** (`nestegg.c:2946–2952`).
- **HDR metadata:** matrix, range, transfer, primaries, mastering display, MaxCLL/MaxFALL, projection, stereo, alpha, crop (`nestegg_video_params`); BlockAdditions (HDR10+) through `nestegg_packet_additional_data`.
- Lacing ✅, encrypted WebM ✅.
- **Seeking only with Cues** (`TODO`: "Support seeking without Cues"); unknown-size clusters untested; content compression not decompressed; no chapters or tags.

### libwebm (mkvparser) — BSD-3 + patent grant, C++, ~8.1k lines, Google (last commit 2026-10-03)
- Codec-agnostic (raw `CodecID` + `CodecPrivate`); subtitle tracks; Colour, MasteringMetadata, Projection; chapters and tags.
- Unknown-size clusters ✅ (`DoLoadClusterUnknownSize`); seeks with Cues or by scanning loaded clusters.
- `webm_parser/`: newer C++11 callback parser covering every WebM element (`-DENABLE_WEBM_PARSER=ON`).
- Also writes files (`mkvmuxer`).
- Use instead of nestegg if arbitrary MKV (subtitles, chapters, no Cues, live) matters more than pure C.

### minimp4 — CC0, one C header of ~3.8k lines, lieff (last commit 2026-09-25)
- ✅ `moov`/`mdat` MP4, `co64` (files over 4 GB), H.264 (`avc1`, SPS/PPS), HEVC (`hvc1`/`hev1`), AAC/MP3 through `esds` (audio only), read callback.
- ❌ **`ctts` read and thrown away** (`minimp4.h:3040`): no presentation timestamps, so B-frame files are wrong. **Patch needed** (~30 lines).
- ❌ **`stss` not parsed**: no keyframe flags for seeking. **Patch needed** (~30 lines).
- ❌ `elst` ignored: small A/V offset at the start (AAC priming). Patch preferred.
- ❌ Fragmented MP4: `moof` commented out in the demuxer (`minimp4.h:2717`).
- ❌ AV1 (`av1C`), Opus (`dOps`), HDR boxes (`colr`/`mdcv`/`clli`). For HEVC the HDR info is also in the bitstream, so that's fine.
- 32-bit timestamps.

### To write yourself
- MPEG-TS / M2TS (~1–2k lines), AVI and FLV (~500 lines each), Ogg via libogg (BSD).
- ASF/WMV: Media Foundation on Windows, otherwise runtime FFmpeg.

---

## 5. Decoders

### Bundled, permissive

| Codec | Library | License | Notes |
|---|---|---|---|
| AV1 | dav1d | BSD-2 | Fast; fine on Apple Silicon in software |
| VP8 / VP9 | libvpx | BSD-3 | |
| H.264 | **libavc** (Ittiam / Android) | Apache-2.0 | Contains interlaced (MBAFF) code (`ih264d_api.c:1234`). Check High-profile coverage. **Windows not listed in its README** |
| H.264 (alternative) | OpenH264 (Cisco) | BSD-2 | No interlaced. Patent coverage applies **only to Cisco's binary downloaded at run time** |
| HEVC | **libhevc** (Ittiam / Android) | Apache-2.0 | Main, Main Still, RExt (4:0:0/4:2:0/4:2:2). **10-bit only with `u4_enable_hbd`** (`ihevcd_parse_headers.c:1712`), which HDR10 requires. Windows not listed in its README |
| HEVC (alternative) | **hevc.js** (lid-labs), native C++17 core | MIT | Checked 2026-10-26. Main + Main 10 only (no RExt); all 147 HEVC v1 conformance streams bit-exact. ~12k lines, C API in `src/wasm/hevc_api.h` (`feed`/`drain`/`flush`, 16-bit planes). WPP threaded, tiles sequential. Ignores SEI and VUI colour info (take HDR metadata from the container). GCC/clang only: `-Werror -Wpedantic`, `__attribute__((vector_size))` in `src/common/simd.h`, CI Linux-only, so MSVC needs work (clang-cl may do). 7 months old, one developer, AI-assisted |
| MPEG-2 | **libmpeg2** (Ittiam; *not* the GPL library with the same name) | Apache-2.0 | Windows not listed in its README |
| MPEG-4 Part 2 / H.263 | Android `m4v_h263` decoder | Apache-2.0 | `frameworks/av/media/module/codecs/m4v_h263/dec` (checked, see 5b) |
| AAC (LC, HE v1/v2, xHE/USAC, LD/ELD) | **libxaac** (Ittiam / Android) | Apache-2.0 | Has Windows (Visual Studio) build support. **Closes the AAC gap; replaces fdk-aac** |
| AAC (alternative) | fdk-aac | FDK license | Not OSI-approved; its license says you need your own AAC patent license |
| AAC-LC (alternative) | Symphonia (Rust) | MPL-2.0 | Also MP3/FLAC/Vorbis/ALAC; needs a C wrapper |
| Opus / Vorbis / FLAC | libopus / libvorbis / libFLAC | BSD | |
| MP3 / WAV / FLAC | dr_libs, minimp3 | PD / CC0 | |

The Ittiam libraries were all checked at their 2026-10 commits. Their Apache-2.0
patent grant covers only the code's contributors, **not the H.264/HEVC patent pools**.

### Missing a permissive decoder

| Codec | Open-source options | License |
|---|---|---|
| AC-3 | liba52 (GPL-2), FFmpeg (LGPL) | **AC-3 patents expired; spec ATSC A/52 is public, so writing your own (~3–5k lines) is feasible** |
| E-AC-3 | FFmpeg only | LGPL |
| DTS | **dcadec** (standalone, LGPL-2.1, includes DTS-HD MA; no longer developed), libdca (GPL-2), FFmpeg | DTS core spec ETSI TS 102 114 is public; patents probably expired **(unverified)** |
| TrueHD / MLP | FFmpeg only | LGPL |
| VC-1 | FFmpeg; GPU decoding (VA-API/D3D/NVDEC) with your own parser; Media Foundation on Windows | LGPL / system |

### OS and hardware decoders (no license cost; OS vendor pays the patent royalties)

| Platform | API | Notes |
|---|---|---|
| Windows | Media Foundation (`IMFTransform`) | H.264, AAC ✅; **HEVC needs "HEVC Video Extensions"**; AC-3 varies by version **(unverified)** |
| Windows / Linux / BSD | **VA-API** (libva, MIT) | Works on Windows too, through Microsoft's VAOn12 (Mesa D3D12; NuGet `Microsoft.Direct3D.VideoAccelerationCompatibilityPack`). Slice-level, so **needs your own parser + DPB** |
| macOS | VideoToolbox + AudioToolbox | H.264, HEVC, ProRes; VP9 on macOS 11+; AV1 in hardware on M3+; AAC; AC-3 **(unverified)**. No slice parser needed. MoltenVK has no Vulkan Video **(unverified)** |
| Web (wasm) | WebCodecs | H.264, AAC; HEVC depends on the browser |
| Android | MediaCodec | |
| Cross-vendor | Vulkan Video, NVDEC (has its own parser), Intel VPL, V4L2 | |

Permissive parser sources for slice-level APIs: Chromium `media/parsers` +
`media/gpu/vaapi` (BSD-3), cros-codecs (Rust, BSD-3), vk_video_samples
(Apache/MIT), libgav1 (Apache). Don't copy from GStreamer or FFmpeg (LGPL);
some old libyami parser files also came from GStreamer.

---

## 5b. Android (AOSP) codec survey (checked 2026-10-26)

Source: Codec2 software components in `frameworks/av/media/codec2/components/`,
each component's `Android.bp` tells which library it wraps. AOSP mirror:
`https://android.googlesource.com/platform/<path>`.

### Decoders

| Codec | Codec2 component → library | Where | License |
|---|---|---|---|
| H.264 | avc → libavcdec | external/libavc | Apache-2.0 |
| HEVC | hevc → libhevcdec | external/libhevc | Apache-2.0 |
| MPEG-2 video | mpeg2 → libmpeg2dec | external/libmpeg2 | Apache-2.0 |
| MPEG-4 Part 2 / H.263 | mpeg4_h263 → libstagefright_m4vh263dec | frameworks/av/media/module/codecs/m4v_h263/dec | Apache-2.0 (PacketVideo) |
| AV1 | dav1d → libdav1d; gav1 → libgav1; aom → libaom | external/libdav1d, external/libgav1, external/libaom | BSD-2 / Apache-2.0 / BSD-2 + AOM patent |
| VP8 / VP9 | vpx → libvpx | external/libvpx | BSD-3 |
| APV (Samsung "Advanced Professional Video") | apv → libopenapv | external/libopenapv | BSD-3 |
| AAC (Codec2 default) | aac → **libFraunhoferAAC** | external/aac | **FDK license (not libxaac)** |
| AAC incl. xHE-AAC | xaac → libxaacdec | external/libxaac | Apache-2.0 |
| MP3 | mp3 → libstagefright_mp3dec | frameworks/av/media/module/codecs/mp3dec | Apache-2.0 (PacketVideo) |
| AMR-NB / AMR-WB | amr_nb_wb → libstagefright_amrnbdec / amrwbdec | frameworks/av/media/module/codecs/amrnb, amrwb | Apache-2.0 |
| Vorbis | vorbis → libvorbisidec (Tremolo 0.07, fixed-point, security-patched) | external/tremolo | BSD-like |
| Opus | opus → libopus | external/libopus | BSD-3 |
| FLAC | flac → libstagefright_flacdec | external/flac | BSD-3 |
| IAMF (immersive audio) | iamf → iamf_tools | external/iamf_tools | BSD-3-Clear + AOM patent |
| G.711 µ/A-law | g711 → codecs_g711dec | frameworks/av/media/module/codecs/g711 | Apache-2.0 |
| GSM 06.10 | gsm → libgsm | external/libgsm | TU Berlin, permissive |
| PCM | raw | | Apache-2.0 |
| MIDI / XMF / RTTTL | (sonivox via MIDI extractor) | external/sonivox | Apache-2.0 |
| Bluetooth SBC, G.722, LC3 | (Bluetooth stack, not Codec2) | packages/modules/Bluetooth/system/embdrv/{sbc,g722}, external/liblc3 | Apache-2.0 |

Encoder-only (not useful for playback): libldac (Sony), aptX/aptX-HD encoders, AAC/AMR/AVC/HEVC/MPEG-4/VP8/VP9/APV encoders.

**Not in AOSP at all: AC-3, E-AC-3, AC-4, DTS, TrueHD, VC-1, WMA, ProRes.**
Phone makers license those and ship them as vendor components, so Android
doesn't close the gaps in section 5.

### Demuxers (`frameworks/av/media/module/extractors/`, Apache-2.0)

aac (ADTS), amr, flac, midi, mkv (wraps libwebm `mkvparser`), mp3, mp4
(`MPEG4Extractor.cpp`, also AC-4 parser and HEIF), mpeg2 (`MPEG2TSExtractor`
**and** `MPEG2PSExtractor`), ogg, wav. TS parsing lives in
`media/module/mpeg2ts/` (`ATSParser.cpp`, `ESQueue.cpp`); `ESQueue` frames
H.264, MPEG-1/2/4 video, AAC, MP3, AC-3/E-AC-3, AC-4, DTS/DTS-HD/DTS-UHD and
PCM elementary streams. Could replace the "write yourself" TS/PS demuxer and
the minimp4 patches. **(unverified)** They depend on libstagefright_foundation
(`AMessage`, `sp<>`, `MediaBuffer`), so the effort to lift them out is unknown.

### Subtitles
`frameworks/av/media/libstagefright/timedtext/` (Apache-2.0): 3GPP timed text
and SRT descriptions.

## 6. Runtime FFmpeg (the Firefox approach)

- `papaya_ffmpeg` module loads `libavformat`/`libavcodec` at run time and stays inactive if they're missing.
- Compiled against the headers of each supported major version (libavcodec 58–62), because the versions aren't binary-compatible.
- Where it looks: Linux/BSD system libraries, Homebrew on macOS, an optional **codec pack** download (Windows/macOS) in the app's data folder, Flatpak `codecs-extra`.
- Not inside the installer, so PapayaPlayer stays permissive. A self-hosted codec pack is a separate LGPL download.
- Fedora's default `ffmpeg-free` omits HEVC.

---

## 7. Subtitles

- SRT / WebVTT: your own parser, drawn with Reaktor.
- ASS/SSA: libass (ISC), but it requires FriBidi (LGPL).
- PGS / VobSub: small RLE bitmap decoders you'd write, or FFmpeg (`pgssub`, `dvdsub`).

---

## 8. Comparison of complete stacks (for reference)

| | FFmpeg libraries | libmpv | libvlc | GStreamer |
|---|---|---|---|---|
| License | LGPL without `--enable-gpl` | LGPL with `-Dgpl=false` | LGPL + GPL plugins to remove | LGPL; avoid "ugly" |
| Player features included | ❌ (you write sync and seeking) | ✅ | ✅ | via pipeline |
| Frames stay on the GPU in Reaktor | ✅ SDL3 interop | ⚠️ OpenGL render API only | ⚠️ libvlc 4 (unreleased) | ⚠️ complex |
| HDR | ✅ SDL3 HDR10 | ✅ only as a child window | ⚠️ native window | ⚠️ some sinks |
| Size | ~5–15 MB trimmed | ~30–60 MB | ~50–100+ MB | large |

libmpv's software render API is 8-bit SDR; HDR needs either the child-window
approach or the FFmpeg + SDL3 route.

---

## 9. Open items / next steps

1. **Build libhevc, libavc and libmpeg2 on Windows** (clang-cl). This is the largest unknown.
2. Test libhevc's 10-bit mode with HDR10 samples, and libavc with High-profile/interlaced samples.
3. Write the minimp4 patches: `ctts`, `stss`, `elst`.
4. Check whether Reaktor's SDL build includes the Metal renderer; re-enable SDL audio and the Vulkan renderer.
5. Confirm the SDL3 HDR / texture-interop property names against SDL headers and `testffmpeg.c`.
6. Look at Android's `m4v_h263` decoder (MPEG-4 Part 2).
7. Decide whether to write an AC-3 decoder (patents expired) or rely on runtime FFmpeg.
8. Check the **(unverified)** items: Media Foundation AC-3 per Windows version, AudioToolbox AC-3, DTS core patent status.
9. Patent review for H.264/HEVC/AAC if PapayaPlayer will be sold commercially.

### Suggested build order
1. **v0:** nestegg + dav1d/libvpx + libopus + SDL audio + Reaktor, all software. Proves the engine, A/V sync, seeking and the UI on every platform.
2. **v1:** minimp4 (patched) + libavc + libxaac → MP4/H.264/AAC.
3. **v2:** libhevc + OS decoders (Media Foundation, VideoToolbox) + GPU texture interop + HDR.
4. **v3:** runtime-FFmpeg fallback, TS/AVI demuxers, subtitles.
5. **v4:** VA-API backend with ported parsers.

---

## 10. Re-research of the remaining gaps (2026-10-26)

Three research passes; licenses read in each repo, no downloaded code run.
"FFmpeg-derived" checks are by headers/READMEs/grep, not line-by-line.
Clones: session scratchpad `research-audio/`, `research-containers/`, `research-video/`.

**Provenance warning.** Many 2025–2026 permissive codecs are AI-assisted rewrites.
Several openly port FFmpeg (= LGPL in disguise): harletty `dca`/`eac3`, LibDTSX,
remade_ffmpeg_rs, wmvkit, wmv-decoder crate, jcodec ProRes. **OxideAV** crates
(MIT, "clean-room") are medium risk: a changelog shows "matching ffmpeg" comments
reworded rather than code reworked. Use OxideAV as reference, spot-check before copying.

### Audio
| Codec | Candidate | License | State |
|---|---|---|---|
| AC-3 | pschatzmann/codec-ac3 (C++) or PasyDev/ac3psy (C17) | MIT | Full AC-3, spec-derived |
| E-AC-3 | chyinan/OpenJOC (Rust, C ABI, also Atmos JOC) or oxideav-ac3 | Apache-2.0 / MIT | Extensions covered, no conformance |
| DTS core | oxideav-dts (Rust) | MIT | Core only; no EXSS/XCH/X96/XLL |
| DTS-HD MA | — | — | **Nothing clean.** Play core + passthrough |
| TrueHD / MLP | truehdd `truehd` crate (Rust) | Apache-2.0 | "Experimental, not for production" |
| WMA (all) | oxideav-wma v1/v2 only, tables taken from a Microsoft DLL: legal risk | — | **Gap** → MF / FFmpeg |

Patents: AC-3 expired 2017; E-AC-3 last reported expiry 2026-01-30; DTS core, MLP, WMA v1/v2 probably expired **(unverified)**.
Chromium has only AC-3/DTS header parsers (passthrough), no decoders.

### Video
| Codec | Candidate | License | State |
|---|---|---|---|
| VC-1 / WMV9 software | — (go-vc1, BSD-2, I-frames only, comments "mirror ffmpeg") | — | **Gap** |
| VC-1 hardware path | Intel vpl-gpu-rt UMC `vc1_dec`/`vc1_common` (C++, active) or libyami `vc1Parser.cpp` (archived) | MIT / Apache-2.0 | Header + bitplane parsers to feed VA-API / D3D11VA / NVDEC |
| WMV7/8, MSS1/2 | — | — | **Gap** |
| ProRes (all 6 profiles, 10/12-bit, alpha, interlaced) | oxideav-prores (Rust, ~11k lines) | MIT | Spot-check provenance first. turbores is MPL-2.0 and cites proresdec.c |
| ProRes RAW | — | — | Gap |

VC-1 pool (Via LA) still active; ProRes patents held by Apple **(unverified terms)**.

### Containers and subtitles: port AndroidX Media3 (Java, Apache-2.0, no FFmpeg attributions)
Paths under `libraries/extractor/src/main/java/androidx/media3/extractor/`.

| Format | Source | Note |
|---|---|---|
| AVI | `avi/` (~1.3k lines) | No OpenDML (>1 GB): add from spec |
| FLV | `flv/` (~950 lines) | H.264 + AAC/MP3/G.711 only |
| ASF | — (jchv/asf-rs, ISC, WIP, 2021, as reference) | Write own; **legal check on the Microsoft ASF spec license** |
| SRT | `text/subrip/SubripParser` (311 lines) | |
| WebVTT | `text/webvtt/` (~2.2k lines); mozilla/vtt.js (Apache-2.0) as second reference | |
| PGS | `text/pgs/PgsParser` (276 lines) | Ignores WDS/composition: add, using libpgs / pgs-rs (MIT) |
| VobSub | `text/vobsub/VobsubParser` (479 lines, MKV only) | Standalone .idx/.sub: model on emk `vobsub` (Apache-2.0) |
| DVB subs, CEA-608/708, SSA, TTML | `text/dvb`, `text/cea`, `text/ssa`, `text/ttml` | Bonus |

Subtitle **rendering** is done by Reaktor (shaping, BiDi, fonts).

### Gaps that remain (OS decoder / runtime FFmpeg / passthrough only)
VC-1 + WMV7/8/9 software decode (MF on Windows; elsewhere hardware via Intel parser),
WMA all versions, DTS-HD MA lossless (core + passthrough), ProRes RAW, MSS1/2,
ASF until we write it.

### Rust question
AC-3 aside, the best E-AC-3, DTS, TrueHD and ProRes candidates are Rust. Decide:
add a Rust toolchain and link them as staticlibs through a C ABI, or port to C.
