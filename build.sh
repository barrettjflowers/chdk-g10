#!/bin/bash
# Build CHDK for the G10 (firmware 1.02A) with ARM GCC 11.3.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
export PATH="$ROOT/toolchain/arm-gnu-toolchain-11.3.rel1-aarch64-arm-none-eabi/bin:$PATH"

arm-none-eabi-gcc --version | head -1
cd "$ROOT/chdk"
make PLATFORM=g10 PLATFORMSUB=102a firzipsubcomplete 2>&1 | tail -20
ls -la bin/
