#!/bin/sh
# Configures and builds with clang and Ninja on macOS, Linux and the BSDs.
#   ./build.sh                         build everything into build/
#   ./build.sh -o build-static -- -DPAPAYA_SHARED_LIBS=OFF
#   ./build.sh -t                      build, then run ctest
set -eu

out=build
test=0
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out=$2; shift 2 ;;
        -t) test=1; shift ;;
        --) shift; break ;;
        *) echo "usage: $0 [-o dir] [-t] [-- cmake options]" >&2; exit 2 ;;
    esac
done

root=$(cd "$(dirname "$0")" && pwd)
if [ ! -f "$root/third_party/reaktor/external/SDL/CMakeLists.txt" ]; then
    echo "submodules missing - run: git submodule update --init --recursive" >&2
    exit 1
fi

: "${CC:=clang}"
: "${CXX:=clang++}"
export CC CXX

cmake -S "$root" -B "$root/$out" -G Ninja "$@"
cmake --build "$root/$out" --parallel
if [ "$test" = 1 ]; then
    ctest --test-dir "$root/$out" --output-on-failure
fi
