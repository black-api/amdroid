#!/bin/bash
# Builds the vendored decoders that Reel (the media process) links, into out/codecs/:
#   musl 1.2.5      the C library (string functions, libm, snprintf), static     MIT
#   FFmpeg 7.1.2    libavcodec (H.264, VP9, AAC, Opus), libavutil, libswresample  LGPL-2.1+
#   dav1d 1.5.1     the AV1 decoder                                               BSD-2-Clause
# Each tarball is checked against the SHA-256 below before it is used. Everything is built
# for a single thread (Reel is one process on one CPU) and with the x86 SIMD code
# (nasm), which the codecs pick at run time from CPUID. Needs: gcc, nasm, ninja, python3
# (meson comes from a venv here). Run from the repository root; the Makefile runs it when
# out/codecs/.done is missing. Takes a few minutes.
set -euo pipefail
ROOT=$(pwd)
OUT=$ROOT/out/codecs
SRC=$OUT/src
mkdir -p "$SRC"

fetch(){  # url sha256 file
    local url=$1 sum=$2 f=$SRC/$3
    if [ ! -f "$f" ] || ! echo "$sum  $f" | sha256sum -c --quiet - 2>/dev/null; then
        echo "codecs: downloading $3"
        curl -sSfL -o "$f.part" "$url"
        mv "$f.part" "$f"
    fi
    echo "$sum  $f" | sha256sum -c --quiet - || { echo "codecs: $3 does not match its checksum"; exit 1; }
}
fetch https://musl.libc.org/releases/musl-1.2.5.tar.gz a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4 musl-1.2.5.tar.gz
fetch https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz 089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304 ffmpeg-7.1.2.tar.xz
fetch https://code.videolan.org/videolan/dav1d/-/archive/1.5.1/dav1d-1.5.1.tar.gz fa635e2bdb25147b1384007c83e15de44c589582bb3b9a53fc1579cb9d74b695 dav1d-1.5.1.tar.gz

cd "$SRC"
# ---- musl ----
if [ ! -f "$OUT/musl/lib/libc.a" ]; then
    rm -rf musl-1.2.5 && tar xf musl-1.2.5.tar.gz
    (cd musl-1.2.5 && ./configure --prefix="$OUT/musl" --disable-shared \
        CFLAGS="-O2 -fno-stack-protector -fno-pie" >/dev/null && make -j"$(nproc)" >/dev/null && make install >/dev/null)
fi
export PATH="$OUT/musl/bin:$PATH"

# ---- FFmpeg: only the four decoders, no programs, no formats, no threads ----
if [ ! -f "$OUT/ffbuild/libavcodec/libavcodec.a" ]; then
    rm -rf ffmpeg-7.1.2 "$OUT/ffbuild" && tar xf ffmpeg-7.1.2.tar.xz
    mkdir -p "$OUT/ffbuild"
    (cd "$OUT/ffbuild" && "$SRC/ffmpeg-7.1.2/configure" --cc=musl-gcc --enable-cross-compile --target-os=linux --arch=x86_64 \
        --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-avdevice --disable-avformat \
        --disable-swscale --enable-swresample --disable-postproc --disable-avfilter --disable-network --disable-pthreads \
        --enable-decoder=h264,vp9,aac,opus --disable-debug --disable-iconv --disable-zlib --disable-bzlib --disable-lzma \
        --enable-static --disable-shared --disable-stripping --disable-pic \
        --extra-cflags="-fno-stack-protector -fno-pie" --extra-ldflags="-no-pie -static" >/dev/null \
     && make -j"$(nproc)" >/dev/null 2>&1)
fi

# ---- dav1d: 8-bit only ----
if [ ! -f "$SRC/dav1d-1.5.1/build/src/libdav1d.a" ]; then
    rm -rf dav1d-1.5.1 && tar xf dav1d-1.5.1.tar.gz
    if ! command -v meson >/dev/null; then
        [ -x "$OUT/venv/bin/meson" ] || { python3 -m venv "$OUT/venv" && "$OUT/venv/bin/pip" install -q meson; }
        export PATH="$OUT/venv/bin:$PATH"
    fi
    (cd dav1d-1.5.1 && CC=musl-gcc CFLAGS="-fno-stack-protector -fno-pie" LDFLAGS="-no-pie -static" \
        meson setup build --buildtype=release --default-library=static -Dbitdepths=8 -Denable_tools=false \
        -Denable_tests=false -Denable_examples=false -Dlogging=false >/dev/null && ninja -C build >/dev/null)
fi

# ---- where the Makefile looks ----
mkdir -p "$OUT/lib"
cp "$OUT/ffbuild/libavcodec/libavcodec.a" "$OUT/ffbuild/libavutil/libavutil.a" "$OUT/ffbuild/libswresample/libswresample.a" \
   "$SRC/dav1d-1.5.1/build/src/libdav1d.a" "$OUT/lib/"
cp "$OUT/musl/lib/libc.a" "$OUT/lib/libc-musl.a"
touch "$OUT/.done"
echo "codecs: built into out/codecs/lib"
