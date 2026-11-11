#!/bin/sh
# mkw100 build on macOS arm64: the fork's translator (it knows translation.player_slot_sites, the
# bundled v0.9.3 one does not), translation, manifests, build shards, then the runtime.
#
#   scripts/mkw100/build.sh [build-dir]        default build-macos; configure it once first
#
# Needs Homebrew `dotnet` (compiles the net8.0 translator; dotnet@8's 8.0.1xx compiler is too old
# for the Tomlyn source generator) and `dotnet@8` (runs it), plus the WiiCompiled Setup app's ninja.
# Measured on an M5 Pro: 73 s when translation changes many functions, 18 s when nothing changed.
set -eu

cd "$(dirname "$0")/../.."
BUILD_DIR=${1:-build-macos}
SDK=/opt/homebrew/opt/dotnet/bin/dotnet
RUNTIME_ROOT=/opt/homebrew/opt/dotnet@8/libexec
PROJECT=projects/mkwii/recomp.yml
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

stage() { printf '[build %s/6] %s (%s)\n' "$1" "$2" "$(date +%H:%M:%S)"; }
translator() { DOTNET_ROOT=$RUNTIME_ROOT build-translator/Translator.Cli "$@"; }

stage 1 "translator"
"$SDK" build translator/src/Translator.Cli/Translator.Cli.csproj -c Release -o build-translator -v quiet -nologo

stage 2 "translate"
translator translate-recursive 0x800060A4 --project "$PROJECT" --outdir generated/functions \
    --output-metadata generated/base_translation_output.json \
    --production-source-bundle generated/base_translation_sources.bin \
    --no-function-files --prune-stale --threads "$(sysctl -n hw.ncpu)"

stage 3 "base manifest"
translator emit-base-manifest --project "$PROJECT" --out build/base --functions-dir generated/functions \
    --translation-output-metadata generated/base_translation_output.json --region P

stage 4 "data init"
translator generate-data-init --project "$PROJECT"

stage 5 "build shards"
translator emit-build-shards --project "$PROJECT" --base-metadata generated/base_translation_output.json \
    --base-functions-dir generated/functions --native-source-dir runtime/src --out generated/build_shards

stage 6 "runtime ($BUILD_DIR)"
cmake --build "$BUILD_DIR" --target WiiCompiled --parallel "$(sysctl -n hw.ncpu)"
