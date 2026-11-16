#!/usr/bin/env bash
# The web test: the C# library built with NativeAOT-LLVM (its emsdk), linked into host.cpp with the
# engine's emsdk, run under Node. Usage: build.sh [dotnet-emsdk-dir] [engine-emsdk-dir]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dotnet_emsdk="${1:-$HOME/emsdk-3.1.56}"
engine_emsdk="${2:-$HOME/emsdk}"
out="$here/out"
mkdir -p "$out"

# 1. C# to LLVM bitcode, with the emsdk NativeAOT-LLVM was built against.
( source "$dotnet_emsdk/emsdk_env.sh" >/dev/null 2>&1
  dotnet publish "$here/Spike.csproj" -c Release -r browser-wasm \
      -p:EMSDK="$dotnet_emsdk" -p:LinkInputsFile="$out/link-inputs.txt" -o "$out/publish" )

# 2. The host, linked with that bitcode, by the engine's emsdk (single-threaded, as the engine's web
#    build is).
source "$engine_emsdk/emsdk_env.sh" >/dev/null 2>&1
mapfile -t inputs < "$out/link-inputs.txt"
em++ -O2 "$here/host.cpp" "${inputs[@]}" -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node \
    -sERROR_ON_UNDEFINED_SYMBOLS=1 -o "$out/host.js"

# 3. Run it.
node "$out/host.js"
