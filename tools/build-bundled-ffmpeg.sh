#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-only
# Copyright (C) 2026 JobDestroyer
#
# Build a private LGPL FFmpeg 7.1 (shared). No --enable-gpl.
# HTTPS uses the host GnuTLS (LGPL, libgnutls.so.30) so FFmpeg stays 2.1.
# Output prefix/pack/<soname> for stomp-pack.
set -euo pipefail

FFMPEG_VER=7.1.5
STAMP_ID="ffmpeg-${FFMPEG_VER}+gnutls+stomp3"

FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VER}.tar.xz"
FFMPEG_SHA256="de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f"

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <install-prefix> [workdir]" >&2
    exit 1
fi

PREFIX=$(readlink -f "$1")
WORK=${2:-"${PREFIX}-work"}
mkdir -p "$PREFIX" "$WORK/dl"

STAMP="$PREFIX/.stomp-ffmpeg-stamp"
PACK="$PREFIX/pack"
if [[ -f "$STAMP" && $(cat "$STAMP") == "$STAMP_ID" &&
      -f "$PACK/libavformat.so.61" && -f "$PACK/libavcodec.so.61" &&
      -f "$PACK/libavutil.so.59" && -f "$PACK/libswresample.so.5" &&
      -f "$PREFIX/include/libavformat/avformat.h" ]]; then
    echo "stomp-ffmpeg: $STAMP_ID already built"
    exit 0
fi

dl_ok() {
    local f=$1 want=$2
    [[ -f "$f" ]] || return 1
    echo "$want  $f" | sha256sum -c --status
}

fetch() {
    local url=$1 dest=$2 want=$3
    if dl_ok "$dest" "$want"; then
        return 0
    fi
    echo "stomp-ffmpeg: download $(basename "$dest")"
    curl -fsSL -L -o "$dest.partial" "$url"
    mv "$dest.partial" "$dest"
    if ! dl_ok "$dest" "$want"; then
        echo "stomp-ffmpeg: sha256 mismatch for $dest" >&2
        exit 1
    fi
}

# Reuse a previous download next to the work dir (cmake build/ffmpeg-dl).
for cand in \
    "$(dirname "$WORK")/ffmpeg-dl" \
    "$WORK/dl" \
    "${CMAKE_SOURCE_DIR:-}/build/ffmpeg-dl"
do
    if [[ -d "$cand" ]]; then
        [[ -f $cand/ffmpeg-${FFMPEG_VER}.tar.xz ]] && cp -n "$cand/ffmpeg-${FFMPEG_VER}.tar.xz" "$WORK/dl/" || true
    fi
done

fetch "$FFMPEG_URL" "$WORK/dl/ffmpeg-${FFMPEG_VER}.tar.xz" "$FFMPEG_SHA256"

JOBS=$(nproc)
GNUTLS_INC=""
if [[ -f /usr/include/gnutls/gnutls.h ]]; then
    GNUTLS_INC=/usr/include
else
    GNUTLS_ROOT="$WORK/gnutls-dev"
    if [[ ! -f "$GNUTLS_ROOT/usr/include/gnutls/gnutls.h" ]]; then
        echo "stomp-ffmpeg: extract libgnutls28-dev headers"
        mkdir -p "$WORK/dl"
        (cd "$WORK/dl" && apt-get download libgnutls28-dev)
        rm -rf "$GNUTLS_ROOT"
        dpkg-deb -x "$WORK/dl"/libgnutls28-dev_*.deb "$GNUTLS_ROOT"
    fi
    GNUTLS_INC="$GNUTLS_ROOT/usr/include"
fi
if [[ ! -f "$GNUTLS_INC/gnutls/gnutls.h" ]]; then
    echo "stomp-ffmpeg: GnuTLS headers not found (need libgnutls28-dev)" >&2
    exit 1
fi
GNUTLS_LIB="$WORK/gnutls-lib"
mkdir -p "$GNUTLS_LIB"
if [[ ! -e "$GNUTLS_LIB/libgnutls.so" ]]; then
    if [[ -e /usr/lib/x86_64-linux-gnu/libgnutls.so ]]; then
        ln -sf /usr/lib/x86_64-linux-gnu/libgnutls.so "$GNUTLS_LIB/libgnutls.so"
    elif [[ -e /usr/lib/x86_64-linux-gnu/libgnutls.so.30 ]]; then
        ln -sf /usr/lib/x86_64-linux-gnu/libgnutls.so.30 "$GNUTLS_LIB/libgnutls.so"
    else
        echo "stomp-ffmpeg: libgnutls.so.30 not found" >&2
        exit 1
    fi
fi

rm -rf "$WORK/src" "$WORK/ffmpeg-build"
mkdir -p "$WORK/src" "$WORK/ffmpeg-build"

echo "stomp-ffmpeg: extract"
tar -xJf "$WORK/dl/ffmpeg-${FFMPEG_VER}.tar.xz" -C "$WORK/src"

DECODERS=(
    aac aac_fixed aac_latm ac3 ac3_fixed alac als ape aptx aptx_hd
    atrac1 atrac3 atrac3al atrac3p atrac3pal atrac9
    binkaudio_dct binkaudio_rdft cook dca dsd_lsbf dsd_lsbf_planar
    dsd_msbf dsd_msbf_planar dst eac3 evrc fastaudio flac g723_1 g729
    gsm gsm_ms iac ilbc imc mace3 mace6 metasound mlp mp1 mp1float
    mp2 mp2float mp3 mp3adu mp3adufloat mp3float mp3on4 mp3on4float
    mpc7 mpc8 nellymoser opus paf_audio qcelp qdm2 qdmc ra_144 ra_288
    ralf s302m shorten sipr smackaud tak truehd truespeech tta twinvq
    vmdaudio vorbis wavpack wmalossless wmapro wmav1 wmav2 wmavoice
    ws_snd1 xma1 xma2
    pcm_alaw pcm_bluray pcm_dvd pcm_f16le pcm_f24le pcm_f32be pcm_f32le
    pcm_f64be pcm_f64le pcm_lxf pcm_mulaw pcm_s8 pcm_s8_planar
    pcm_s16be pcm_s16be_planar pcm_s16le pcm_s16le_planar
    pcm_s24be pcm_s24daud pcm_s24le pcm_s24le_planar
    pcm_s32be pcm_s32le pcm_s32le_planar pcm_s64be pcm_s64le
    pcm_sga pcm_u8 pcm_u16be pcm_u16le pcm_u24be pcm_u24le
    pcm_u32be pcm_u32le pcm_vidc
    adpcm_4xm adpcm_adx adpcm_afc adpcm_agm adpcm_aica adpcm_argo
    adpcm_ct adpcm_dtk adpcm_ea adpcm_ea_maxis_xa adpcm_ea_r1
    adpcm_ea_r2 adpcm_ea_r3 adpcm_ea_xas adpcm_g722 adpcm_g726
    adpcm_g726le adpcm_ima_acorn adpcm_ima_alp adpcm_ima_amv
    adpcm_ima_apc adpcm_ima_apm adpcm_ima_cunning adpcm_ima_dat4
    adpcm_ima_dk3 adpcm_ima_dk4 adpcm_ima_ea_eacs adpcm_ima_ea_sead
    adpcm_ima_iss adpcm_ima_moflex adpcm_ima_mtf adpcm_ima_oki
    adpcm_ima_qt adpcm_ima_rad adpcm_ima_smjpeg adpcm_ima_ssi
    adpcm_ima_wav adpcm_ima_ws adpcm_ms adpcm_mtaf adpcm_psx
    adpcm_sbpro_2 adpcm_sbpro_3 adpcm_sbpro_4 adpcm_swf adpcm_thp
    adpcm_thp_le adpcm_vima adpcm_xa adpcm_xmd adpcm_yamaha
    adpcm_zork
)

DEMUXERS=(
    aac ac3 ace act adf adp ads adx aea afc aiff aix alp amr amrnb amrwb
    ape aptx aptx_hd asf asf_o au binka caf concat data dsf dts dtshd
    eac3 epaf flac flv g722 g723_1 g726 g726le g729 gsm hca hcom hls
    iff ilbc ircam iss kvag live_flv loas matroska mlp mov mp3 mpc mpc8
    mpegts mpegtsraw msf musx nistsphere nsp ogg oma paf pcm_alaw
    pcm_f32be pcm_f32le pcm_f64be pcm_f64le pcm_mulaw pcm_s16be pcm_s16le
    pcm_s24be pcm_s24le pcm_s32be pcm_s32le pcm_s8 pcm_u16be pcm_u16le
    pcm_u24be pcm_u24le pcm_u32be pcm_u32le pcm_u8 pvf qcp rsd rso rtp
    rtsp s337m sap sbc sbg sds sdx shorten sln sox spdif tak truehd tta
    voc w64 wav wsaud wsd wtv wv wve xa xmv xvag xwma
)

PARSERS=(aac aac_latm ac3 cook dca flac mlp mpegaudio opus sipr vorbis xma)
BSFS=(aac_adtstoasc chomp dump_extradata extract_extradata pcm_rechunk)
PROTOCOLS=(file http https tcp tls crypto rtp udp pipe data cache concat)

enable_flags=()
for n in "${DECODERS[@]}"; do enable_flags+=(--enable-decoder="$n"); done
for n in "${DEMUXERS[@]}"; do enable_flags+=(--enable-demuxer="$n"); done
for n in "${PARSERS[@]}"; do enable_flags+=(--enable-parser="$n"); done
for n in "${BSFS[@]}"; do enable_flags+=(--enable-bsf="$n"); done
for n in "${PROTOCOLS[@]}"; do enable_flags+=(--enable-protocol="$n"); done

echo "stomp-ffmpeg: ffmpeg ${FFMPEG_VER}"
mkdir -p "$GNUTLS_LIB/pkgconfig"
cat > "$GNUTLS_LIB/pkgconfig/gnutls.pc" <<EOF
Name: GnuTLS
Description: GnuTLS (headers + unversioned .so for the FFmpeg build)
Version: 3.8.9
Libs: -L${GNUTLS_LIB} -lgnutls
Cflags: -I${GNUTLS_INC}
EOF
export PKG_CONFIG_PATH="${GNUTLS_LIB}/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

cd "$WORK/ffmpeg-build"
# GnuTLS stays a system .so (libgnutls.so.30). zlib too (libz.so.1).
"$WORK/src/ffmpeg-${FFMPEG_VER}/configure" \
    --prefix="$PREFIX" \
    --enable-shared \
    --disable-static \
    --enable-pic \
    --disable-gpl \
    --disable-version3 \
    --disable-nonfree \
    --disable-autodetect \
    --disable-debug \
    --disable-doc \
    --disable-htmlpages \
    --disable-manpages \
    --disable-podpages \
    --disable-txtpages \
    --disable-programs \
    --disable-ffmpeg \
    --disable-ffplay \
    --disable-ffprobe \
    --disable-avdevice \
    --disable-avfilter \
    --disable-swscale \
    --disable-postproc \
    --disable-everything \
    --enable-avutil \
    --enable-avcodec \
    --enable-avformat \
    --enable-swresample \
    --enable-network \
    --enable-zlib \
    --enable-gnutls \
    --enable-x86asm \
    --extra-cflags="-I${GNUTLS_INC} -fPIC -O2" \
    --extra-ldflags="-L${GNUTLS_LIB}" \
    "${enable_flags[@]}"

# Four dollars: make stores $$ORIGIN, the recipe then passes $ORIGIN to ld.
# Two dollars becomes $ORIGIN in the makefile value, then $O (FFmpeg's obj
# prefix) expands to empty and RUNPATH is left as "RIGIN".
printf '%s\n' 'LDFLAGS+=-Wl,-z,origin -Wl,-rpath,$$$$ORIGIN' >> ffbuild/config.mak

make -j"$JOBS"
# Wipe a previous prefix so leftover system-style libs cannot sneak into the pack.
rm -rf "$PREFIX/lib" "$PREFIX/include" "$PREFIX/pack" "$PREFIX/share"
make install

mkdir -p "$PACK"
copy_soname() {
    local lib=$1
    local src
    src=$(find "$PREFIX/lib" -maxdepth 1 -name "${lib}.so.*" -type f | sort | tail -1)
    if [[ -z "$src" ]]; then
        echo "stomp-ffmpeg: missing $lib" >&2
        exit 1
    fi
    local soname
    soname=$(readelf -d "$src" | awk '/SONAME/{gsub(/[][]/,"",$NF); print $NF; exit}')
    if [[ -z "$soname" ]]; then
        echo "stomp-ffmpeg: no SONAME on $src" >&2
        exit 1
    fi
    cp -a "$src" "$PACK/$soname"
    echo "stomp-ffmpeg: pack $soname ($(du -h "$PACK/$soname" | awk '{print $1}'))"
}

copy_soname libavformat
copy_soname libavcodec
copy_soname libavutil
copy_soname libswresample

ensure_origin_rpath() {
    local f=$1
    local rp
    rp=$(readelf -d "$f" | awk '/RUNPATH|RPATH/{gsub(/[][]/,"",$NF); print $NF; exit}')
    if [[ "$rp" == '$ORIGIN' ]]; then
        return 0
    fi
    echo "stomp-ffmpeg: RUNPATH on $(basename "$f") is '${rp:-none}', setting \$ORIGIN"
    local pe
    pe=$(command -v patchelf || true)
    if [[ -z "$pe" ]]; then
        local pe_root="$WORK/patchelf-bin"
        if [[ ! -x "$pe_root/usr/bin/patchelf" ]]; then
            echo "stomp-ffmpeg: extract patchelf"
            mkdir -p "$WORK/dl"
            (cd "$WORK/dl" && apt-get download patchelf)
            rm -rf "$pe_root"
            dpkg-deb -x "$WORK/dl"/patchelf_*.deb "$pe_root"
        fi
        pe="$pe_root/usr/bin/patchelf"
    fi
    if [[ ! -x "$pe" ]]; then
        echo "stomp-ffmpeg: patchelf not available to fix RUNPATH on $f" >&2
        exit 1
    fi
    "$pe" --set-rpath '$ORIGIN' "$f"
    rp=$(readelf -d "$f" | awk '/RUNPATH|RPATH/{gsub(/[][]/,"",$NF); print $NF; exit}')
    if [[ "$rp" != '$ORIGIN' ]]; then
        echo "stomp-ffmpeg: failed to set RUNPATH on $f (got '${rp:-none}')" >&2
        exit 1
    fi
}

for _so in libavformat.so.61 libavcodec.so.61 libavutil.so.59 libswresample.so.5; do
    ensure_origin_rpath "$PACK/$_so"
done

echo "$STAMP_ID" > "$STAMP"
echo "stomp-ffmpeg: installed $STAMP_ID -> $PREFIX"
