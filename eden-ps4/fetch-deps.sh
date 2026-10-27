#!/usr/bin/env bash
# Clones the Eden dependencies that its CPM would download as GitHub archives (/archive/ URLs
# are blocked in some build environments), at the versions Eden pins in cpmfile.json, into
# $EDEN_DEPS (default ~/eden-deps), and applies Eden's patches for them as Eden does
# (patch -p1, failures reported but not fatal). build.sh points Eden at these folders with
# <package>_CUSTOM_DIR.
#
# Boost and the time zone database are release downloads; CPM fetches those itself.
set -euo pipefail
EDEN_DIR="${EDEN_DIR:-$HOME/eden}"
EDEN_DEPS="${EDEN_DEPS:-$HOME/eden-deps}"

# What a build without Qt, SDL, cubeb, web services and tests asks for (FFmpeg is for
# build-ffmpeg.sh).
DEPS=(boost_headers enet ffmpeg fmt frozen gamemode httplib lz4 nlohmann opus simpleini sirit
    spirv-headers vulkan-headers vulkan-memory-allocator vulkan-utility-libraries xbyak zbic
    zlib zstd)

mkdir -p "$EDEN_DEPS"
for name in "${DEPS[@]}"; do
    read -r repo version patches < <(python3 -I - "$EDEN_DIR/cpmfile.json" "$name" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))[sys.argv[2]]
print(p["repo"], p["version"], ",".join(p.get("patches", [])) or "-")
PY
)
    dir="$EDEN_DEPS/$name"
    if [ -f "$dir/.eden-version" ] && [ "$(cat "$dir/.eden-version")" = "$version" ]; then
        continue
    fi
    echo "== $name: $repo $version"
    rm -rf "$dir"
    git clone -q --filter=blob:none "https://github.com/$repo.git" "$dir"
    git -C "$dir" -c advice.detachedHead=false checkout -q "$version"
    if [ "$patches" != "-" ]; then
        IFS=, read -ra list <<<"$patches"
        for p in "${list[@]}"; do
            if patch -d "$dir" -p1 -s <"$EDEN_DIR/.patch/$name/$p"; then
                echo "   applied $p"
            else
                echo "   $p did not apply (Eden ignores this too)"
            fi
        done
    fi
    echo "$version" >"$dir/.eden-version"
done
du -sh "$EDEN_DEPS"
