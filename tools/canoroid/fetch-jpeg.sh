#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

directory=${1:?usage: fetch-jpeg.sh target-directory}
mkdir -p "$directory"
cd "$directory"
if [ -f jpeg-9f/jconfig.h ]; then
    exit 0
fi
stage=$(mktemp -d .jpeg.XXXXXXXX)
trap 'rm -rf "$stage"' EXIT
curl --fail --location --silent --show-error --max-time 120 \
    https://www.ijg.org/files/jpegsrc.v9f.tar.gz -o "$stage/source.tar.gz"
printf '%s  %s\n' 04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b \
    "$stage/source.tar.gz" | sha256sum -c -
tar xzf "$stage/source.tar.gz" -C "$stage"
cp "$stage/jpeg-9f/jconfig.txt" "$stage/jpeg-9f/jconfig.h"

# Fit the 600D's boot reservation. Decode and encode single-scan Huffman
# JPEGs with the integer DCT. Canoroid does its own scanline resampling.
sed -E 's/^#define (DCT_IFAST|DCT_FLOAT|C_ARITH_CODING|C_MULTISCAN_FILES|C_PROGRESSIVE|DCT_SCALING|ENTROPY_OPT|INPUT_SMOOTHING|D_ARITH_CODING|D_MULTISCAN_FILES|D_PROGRESSIVE|IDCT_SCALING|SAVE_MARKERS|BLOCK_SMOOTHING|QUANT_1PASS|QUANT_2PASS)_SUPPORTED[[:space:]].*/#undef \1_SUPPORTED/' \
    "$stage/jpeg-9f/jmorecfg.h" > "$stage/jmorecfg.h"
mv "$stage/jmorecfg.h" "$stage/jpeg-9f/jmorecfg.h"
mv "$stage/jpeg-9f" .
