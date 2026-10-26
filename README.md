# PapayaPlayer

A cross-platform audio and video player in C, with [Reaktor](https://github.com/Alpaq92/Reaktor)
as its UI. Everything shipped is permissively licensed; no FFmpeg.

Platforms: Windows, macOS, Linux, FreeBSD, NetBSD, OpenBSD.

## Build

```sh
git clone --recursive <repo>
./build.ps1 -Test        # Windows: clang-cl + Ninja inside the MSVC environment
./build.sh -t            # macOS, Linux, BSDs: clang + Ninja
```

Needs clang (clang-cl on Windows), CMake 3.22+, Ninja and Rust (cargo).
`-DPAPAYA_SHARED_LIBS=OFF` builds one static executable instead of shared libraries.

## Documents

- [IMPLEMENTATION-PLAN.md](IMPLEMENTATION-PLAN.md): phases and releases
- [TECH-STACK.md](TECH-STACK.md): decisions
- [CODECS.md](CODECS.md): codecs, containers, subtitles, gaps
- [BACKEND-RESEARCH.md](BACKEND-RESEARCH.md): research evidence
- [spikes/RESULTS.md](spikes/RESULTS.md): phase 0 spike results
