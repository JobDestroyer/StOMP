#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-only
# Copyright (C) 2026 JobDestroyer
# Install StOMP on a Steam Machine / SteamOS box: binary, Steam library
# shortcut, and Big Picture / library art.
#
# From a built tree:
#   ./install.sh
#
# One-liner (after this file is on GitHub):
#   curl -fsSL https://raw.githubusercontent.com/JobDestroyer/StOMP/main/install.sh | bash
set -euo pipefail

REPO="${STOMP_REPO:-JobDestroyer/StOMP}"
RAW_BASE="https://raw.githubusercontent.com/${REPO}/main"
REL_BASE="https://github.com/${REPO}/releases/latest/download"
APP_NAME="StOMP"
INSTALL_DIR="${STOMP_INSTALL_DIR:-${HOME}/.local/share/stomp}"
BIN_LINK="${HOME}/.local/bin/stomp"
DEST="${INSTALL_DIR}/stomp"

log() { printf 'stomp-install: %s\n' "$*"; }
die() { printf 'stomp-install: %s\n' "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "need $1"; }

need curl
need python3
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
    "${ROOT:+${ROOT}/Releases/StOMP-0.1}" \
    "${ROOT:+${ROOT}/stomp}" \
    "./build/stomp" \
    "./Releases/StOMP-0.1"
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
  if fetch "${REL_BASE}/stomp" "$tmp" || fetch "${RAW_BASE}/Releases/StOMP-0.1" "$tmp"; then
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
  local localp=""
  if [[ -n "$ROOT" && -f "${ROOT}/steam-assets/${name}" ]]; then
    localp="${ROOT}/steam-assets/${name}"
  fi
  if [[ -n "$localp" ]]; then
    install -m 0644 "$localp" "${ASSET_DIR}/${name}"
    return 0
  fi
  if fetch "${RAW_BASE}/steam-assets/${name}" "${ASSET_DIR}/${name}"; then
    return 0
  fi
  return 1
}
ART_OK=0
for a in "${ASSETS[@]}"; do
  if copy_asset "$a"; then
    ART_OK=1
  else
    log "warning: missing art ${a}"
  fi
done

cat > "${HOME}/.local/share/applications/stomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=StOMP
Comment=Steam OS Music Player
Exec=${DEST}
Icon=${ASSET_DIR}/community_icon_184x184.png
Terminal=false
Categories=AudioVideo;Audio;Player;
EOF

# --- Steam library shortcut + Big Picture grid art ---
export STOMP_EXE="${DEST}"
export STOMP_APP_NAME="${APP_NAME}"
export STOMP_ASSET_DIR="${ASSET_DIR}"
export STOMP_ART_OK="${ART_OK}"

python3 - <<'PY'
import os, sys, struct, zlib, shutil, glob

exe = os.environ["STOMP_EXE"]
name = os.environ["STOMP_APP_NAME"]
asset_dir = os.environ["STOMP_ASSET_DIR"]
art_ok = os.environ.get("STOMP_ART_OK") == "1"
home = os.path.expanduser("~")

def steam_roots():
    cands = [
        os.path.join(home, ".steam", "steam"),
        os.path.join(home, ".local", "share", "Steam"),
        os.path.join(home, ".steam", "root"),
        os.path.join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"),
    ]
    out = []
    for c in cands:
        if os.path.isdir(os.path.join(c, "userdata")):
            out.append(os.path.realpath(c))
    # unique
    seen = set()
    uniq = []
    for c in out:
        if c not in seen:
            seen.add(c)
            uniq.append(c)
    return uniq

def userdata_ids(root):
    ud = os.path.join(root, "userdata")
    ids = []
    if not os.path.isdir(ud):
        return ids
    for name in os.listdir(ud):
        if not name.isdigit():
            continue
        cfg = os.path.join(ud, name, "config")
        if os.path.isdir(cfg):
            ids.append(name)
    return ids

exe_field = '"' + exe + '"'
start_dir = '"' + os.path.dirname(exe) + os.sep + '"'
icon = os.path.join(asset_dir, "community_icon_184x184.png")
if not os.path.isfile(icon):
    icon = os.path.join(asset_dir, "client_icon_256x256.png")
appid = (zlib.crc32((exe_field + name).encode("utf-8")) & 0xFFFFFFFF) | 0x80000000
bp_id = (appid << 32) | 0x02000000

def read_cstr(buf, i):
    j = buf.index(b"\x00", i)
    return buf[i:j].decode("utf-8", "replace"), j + 1

def parse_obj(buf, i):
    obj = {}
    while i < len(buf):
        t = buf[i]
        i += 1
        if t == 0x08:
            return obj, i
        key, i = read_cstr(buf, i)
        if t == 0x00:
            child, i = parse_obj(buf, i)
            obj[key] = child
        elif t == 0x01:
            val, i = read_cstr(buf, i)
            obj[key] = val
        elif t == 0x02:
            obj[key] = struct.unpack_from("<I", buf, i)[0]
            i += 4
        elif t == 0x03:
            obj[key] = struct.unpack_from("<f", buf, i)[0]
            i += 4
        elif t == 0x07:
            obj[key] = struct.unpack_from("<Q", buf, i)[0]
            i += 8
        else:
            raise ValueError("unsupported vdf type %s" % t)
    return obj, i

def write_obj(obj):
    out = bytearray()
    for k, v in obj.items():
        key = k.encode("utf-8") + b"\x00"
        if isinstance(v, dict):
            out.append(0x00)
            out += key
            out += write_obj(v)
            out.append(0x08)
        elif isinstance(v, str):
            out.append(0x01)
            out += key
            out += v.encode("utf-8") + b"\x00"
        elif isinstance(v, int):
            out.append(0x02)
            out += key
            out += struct.pack("<I", v & 0xFFFFFFFF)
        elif isinstance(v, float):
            out.append(0x03)
            out += key
            out += struct.pack("<f", v)
        else:
            raise TypeError(type(v))
    return bytes(out)

def load_shortcuts(path):
    if not os.path.isfile(path) or os.path.getsize(path) == 0:
        return {"shortcuts": {}}
    data = open(path, "rb").read()
    if len(data) < 12 or data[0] != 0x00:
        raise ValueError("not a shortcuts.vdf")
    i = 1
    key, i = read_cstr(data, i)
    if key != "shortcuts":
        raise ValueError("not a shortcuts.vdf")
    inner, i = parse_obj(data, i)
    if not isinstance(inner, dict):
        return {"shortcuts": {}}
    return {"shortcuts": inner}

def make_entry():
    return {
        "appid": appid,
        "AppName": name,
        "Exe": exe_field,
        "StartDir": start_dir,
        "icon": icon if os.path.isfile(icon) else "",
        "ShortcutPath": "",
        "LaunchOptions": "",
        "IsHidden": 0,
        "AllowDesktopConfig": 1,
        "AllowOverlay": 1,
        "OpenVR": 0,
        "Devkit": 0,
        "DevkitGameID": "",
        "LastPlayTime": 0,
        "tags": {"0": "StOMP"},
    }

def upsert(root):
    sc = root.setdefault("shortcuts", {})
    found = None
    for k, v in sc.items():
        if isinstance(v, dict) and v.get("AppName") == name:
            found = k
            break
    if found is None:
        idxs = []
        for k in sc.keys():
            try:
                idxs.append(int(k))
            except ValueError:
                pass
        nxt = (max(idxs) + 1) if idxs else 0
        found = str(nxt)
    sc[found] = make_entry()
    return root

def copy_art(grid):
    os.makedirs(grid, exist_ok=True)
    mapping = [
        ("header_capsule_920x430.png", "%s.png" % appid),
        ("library_capsule_600x900.png", "%sp.png" % appid),
        ("library_hero_3840x1240.png", "%s_hero.png" % appid),
        ("library_logo_1280x720.png", "%s_logo.png" % appid),
        ("community_icon_184x184.png", "%s_icon.png" % appid),
        ("header_capsule_920x430.png", "%s.png" % bp_id),
    ]
    n = 0
    for src_name, dst_name in mapping:
        src = os.path.join(asset_dir, src_name)
        if not os.path.isfile(src):
            continue
        dst = os.path.join(grid, dst_name)
        shutil.copy2(src, dst)
        n += 1
    return n

roots = steam_roots()
if not roots:
    print("stomp-install: Steam userdata not found; binary is installed, add it in Desktop Mode if you use Steam later.")
    sys.exit(0)

wrote = 0
for root in roots:
    for uid in userdata_ids(root):
        cfg = os.path.join(root, "userdata", uid, "config")
        vdf_path = os.path.join(cfg, "shortcuts.vdf")
        bak = vdf_path + ".stomp.bak"
        try:
            data = load_shortcuts(vdf_path)
        except Exception as e:
            print("stomp-install: skip %s (%s)" % (vdf_path, e))
            continue
        data = upsert(data)
        raw = b"\x00" + b"shortcuts\x00" + write_obj(data["shortcuts"]) + b"\x08"
        os.makedirs(cfg, exist_ok=True)
        if os.path.isfile(vdf_path):
            shutil.copy2(vdf_path, bak)
        tmp = vdf_path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(raw)
        os.replace(tmp, vdf_path)
        nart = 0
        if art_ok:
            nart = copy_art(os.path.join(cfg, "grid"))
        print("stomp-install: Steam user %s shortcut appid=%s art=%d" % (uid, appid, nart))
        wrote += 1

if wrote == 0:
    print("stomp-install: no Steam user config written")
    sys.exit(0)
print("stomp-install: restart Steam (or return to Gaming Mode) so Big Picture picks up StOMP.")
PY

if pgrep -x steam >/dev/null 2>&1; then
  log "Steam is running — restart it or switch back to Gaming Mode to see StOMP in the library."
fi
log "done. Launch: ${DEST}"
log "or from a terminal: ${BIN_LINK}"
