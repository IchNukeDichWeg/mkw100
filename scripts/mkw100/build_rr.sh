#!/bin/sh
# mkw100 Retro Rewind build on macOS arm64: build.sh for the base, then the Retro Rewind mod
# translation, the build shards with it, and the RetroRewind runtime.
#
#   scripts/mkw100/build_rr.sh [build-dir]       default build-macos
#
# Needs the Retro Rewind release folder at PulsarPacks/completed/RetroRewind/RetroRewind6 (the
# base translation picks its Code.pul up from there; a symlink is fine) and, to run, the same
# folder next to the binary as <build-dir>/RetroRewind6. Offline: no Retro-WFC payload.
# Measured on an M5 Pro: about 2 min with nothing to retranslate.
set -eu

cd "$(dirname "$0")/../.."
BUILD_DIR=${1:-build-macos}
PROJECT=projects/mkwii/recomp.yml
RR=PulsarPacks/completed/RetroRewind/RetroRewind6
OUT=build/mods/retro_rewind_full_cpp

stage() { printf '[build-rr %s/4] %s (%s)\n' "$1" "$2" "$(date +%H:%M:%S)"; }
translator() { DOTNET_ROOT=/opt/homebrew/opt/dotnet@8/libexec build-translator/Translator.Cli "$@"; }

if [ ! -f "$RR/Binaries/Code.pul" ]; then
    echo "no $RR/Binaries/Code.pul; see docs/building-macos.md" >&2
    exit 1
fi

stage 1 "base (build.sh)"
sh scripts/mkw100/build.sh "$BUILD_DIR"

stage 2 "translate Retro Rewind"
mkdir -p "$OUT"
translator translate-mod --project "$PROJECT" --profile retro-rewind \
    --base-manifest build/base/mkwii_base_manifest.json \
    --base-translation-output-metadata generated/base_translation_output.json \
    --code-pul "$RR/Binaries/Code.pul" --mod-root "$RR" --mod-name "Retro Rewind" --region P \
    --out "$OUT" --prefer-cached-inputs --emit-cpp --threads "$(sysctl -n hw.ncpu)" --skip-retro-wfc

stage 3 "build shards with Retro Rewind"
translator emit-build-shards --project "$PROJECT" --base-metadata generated/base_translation_output.json \
    --base-functions-dir generated/functions --native-source-dir runtime/src --out generated/build_shards \
    --resolved-profile "$OUT/resolved_dispatch_profile.json" --retro-cpp-dir "$OUT/cpp"

stage 4 "runtime ($BUILD_DIR)"
cmake "$BUILD_DIR" > /dev/null
cmake --build "$BUILD_DIR" --target WiiCompiled RetroRewind --parallel "$(sysctl -n hw.ncpu)"
