#!/usr/bin/env bash

# Just used others config
# c-iouring-tigokraft

set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build

CC="${CC:-gcc}"

"$CC" -std=gnu17 -O3 -march=native -pthread -Wall -Wextra -Wno-unused-parameter \
    -o build/server src/*.c -luring -lcrypt -lm

echo "build/server is ready"

