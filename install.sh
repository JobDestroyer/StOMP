#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-only
# Copyright (C) 2026 JobDestroyer
#
# Installs the StOMP binary and asks the running Steam client to add it.
# This script never reads or writes shortcuts.vdf or any other Steam config.
set -euo pipefail

REPO="${STOMP_REPO:-JobDestroyer/StOMP}"
RAW_BASE="https://raw.githubusercontent.com/${REPO}/main"
REL_BASE="https://github.com/${REPO}/releases/latest/download"
INSTALL_DIR="${STOMP_INSTALL_DIR:-${HOME}/.local/share/stomp}"
BIN_LINK="${HOME}/.local/bin/stomp"
DEST="${INSTALL_DIR}/stomp"
DESKTOP="${HOME}/.local/share/applications/stomp.desktop"

log() { printf 'stomp-install: %s\n' "$*"; }
die() { printf 'stomp-install: %s\n' "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "need $1"; }

need curl
need install

ROOT=""
if [[ -n "${BASH_SOURCE[0]:-}" && -f "${BASH_SOURCE[0]}" && -r "${BASH_SOURCE[0]}" ]]; then
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi

find_local_bin() {
  local c
  for c in \
    "${STOMP_BIN:-}" \
    "${ROOT:+${ROOT}/build/stomp}" \
    "${ROOT:+${ROOT}/stomp}" \
    "./build/stomp" \
    "./stomp"
  do
    [[ -n "$c" && -f "$c" && -r "$c" ]] && { printf '%s\n' "$c"; return 0; }
  done
  return 1
}

fetch() {
  local url="$1" out="$2"
  curl -fsSL --retry 3 --retry-delay 1 -o "$out" "$url"
}

SRC_BIN=""
if SRC_BIN="$(find_local_bin)"; then
  log "using local binary ${SRC_BIN}"
else
  log "downloading stomp from GitHub Releases"
  tmp="$(mktemp)"
  if fetch "${REL_BASE}/stomp" "$tmp"; then
    SRC_BIN="$tmp"
  else
    rm -f "$tmp"
    die "no stomp binary found. Build one (cmake --build build --target stomp-pack) or set STOMP_BIN=."
  fi
fi

mkdir -p "${INSTALL_DIR}" "${HOME}/.local/bin" "${HOME}/.local/share/applications"
install -m 0755 "${SRC_BIN}" "${DEST}"
ln -sfn "${DEST}" "${BIN_LINK}"
log "installed ${DEST}"

ASSET_DIR="${INSTALL_DIR}/steam-assets"
mkdir -p "${ASSET_DIR}"
ASSETS=(
  header_capsule_920x430.png
  library_capsule_600x900.png
  library_hero_3840x1240.png
  library_logo_1280x720.png
  community_icon_184x184.png
  client_icon_256x256.png
)
copy_asset() {
  local name="$1"
  if [[ -n "$ROOT" && -f "${ROOT}/steam-assets/${name}" ]]; then
    install -m 0644 "${ROOT}/steam-assets/${name}" "${ASSET_DIR}/${name}"
    return 0
  fi
  fetch "${RAW_BASE}/steam-assets/${name}" "${ASSET_DIR}/${name}"
}
ART_OK=0
for a in "${ASSETS[@]}"; do
  if copy_asset "$a"; then
    ART_OK=1
  else
    log "warning: missing art ${a}"
  fi
done

ICON="${ASSET_DIR}/community_icon_184x184.png"
[[ -f "$ICON" ]] || ICON="${ASSET_DIR}/client_icon_256x256.png"

cat > "${DESKTOP}" <<EOF
[Desktop Entry]
Type=Application
Name=StOMP
Comment=Steam OS Music Player
Exec=${DEST}
Icon=${ICON}
Terminal=false
Categories=AudioVideo;Audio;Player;
EOF

# Library art only. Never delete Steam files; never write shortcuts.vdf.
if [[ "$ART_OK" -eq 1 ]] && command -v python3 >/dev/null 2>&1; then
  APPID="$(e="${DEST}" python3 -c 'import os,zlib; e=os.environ["e"]; print((zlib.crc32(("\""+e+"\""+"StOMP").encode())&0xffffffff)|0x80000000)')"
  BP_ID=$(( (APPID << 32) | 0x02000000 ))
  copy_grid() {
    local grid="$1"
    mkdir -p "$grid"
    [[ -f "${ASSET_DIR}/header_capsule_920x430.png" ]] && install -m 0644 "${ASSET_DIR}/header_capsule_920x430.png" "${grid}/${APPID}.png"
    [[ -f "${ASSET_DIR}/library_capsule_600x900.png" ]] && install -m 0644 "${ASSET_DIR}/library_capsule_600x900.png" "${grid}/${APPID}p.png"
    [[ -f "${ASSET_DIR}/library_hero_3840x1240.png" ]] && install -m 0644 "${ASSET_DIR}/library_hero_3840x1240.png" "${grid}/${APPID}_hero.png"
    [[ -f "${ASSET_DIR}/library_logo_1280x720.png" ]] && install -m 0644 "${ASSET_DIR}/library_logo_1280x720.png" "${grid}/${APPID}_logo.png"
    [[ -f "${ASSET_DIR}/community_icon_184x184.png" ]] && install -m 0644 "${ASSET_DIR}/community_icon_184x184.png" "${grid}/${APPID}_icon.png"
    [[ -f "${ASSET_DIR}/header_capsule_920x430.png" ]] && install -m 0644 "${ASSET_DIR}/header_capsule_920x430.png" "${grid}/${BP_ID}.png"
  }
  seen=""
  for base in \
    "${HOME}/.steam/steam" \
    "${HOME}/.local/share/Steam" \
    "${HOME}/.steam/root" \
    "${HOME}/.var/app/com.valvesoftware.Steam/.local/share/Steam"
  do
    [[ -d "${base}/userdata" ]] || continue
    real="$(realpath "$base")"
    case " $seen " in
      *" $real "*) continue ;;
    esac
    seen+=" $real"
    for cfg in "${base}/userdata"/*/config; do
      [[ -d "$cfg" ]] || continue
      copy_grid "${cfg}/grid"
    done
  done
fi

# Ask Steam to add the shortcut. Steam is running on a Steam Machine.
# Valve's own path: steamos-add-to-steam / steam://addnonsteamgame
ask_steam() {
  local target="$1"
  touch /tmp/addnonsteamgamefile 2>/dev/null || true
  if command -v steamos-add-to-steam >/dev/null 2>&1; then
    steamos-add-to-steam "$target" && return 0
  fi
  if command -v steam >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    local url
    url="steam://addnonsteamgame/$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1], safe=""))' "$target")"
    steam "$url" && return 0
  fi
  return 1
}

ADDED=0
if ask_steam "${DESKTOP}" || ask_steam "${DEST}"; then
  ADDED=1
fi

if [[ "$ADDED" -eq 1 ]]; then
  log "asked Steam to add StOMP. Confirm the Add Non-Steam Game dialog if it appears."
else
  log "could not talk to Steam. In Desktop Mode: Games → Add a Non-Steam Game → ${DEST}"
fi
log "done. ${DEST}"
