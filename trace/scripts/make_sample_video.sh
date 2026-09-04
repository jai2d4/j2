#!/usr/bin/env bash
# Regenerates the developer-owned test clip used by the automated suites.
#
# The clip is produced entirely from FFmpeg's synthetic sources — a test
# pattern with a burned-in timecode and a sine tone — so it contains no
# third-party footage and is safe to distribute with the source.
#
#   8 seconds · 320x240 · 25 fps CFR · H.264 + AAC · ~160 KB
#
# The burned-in timecode makes frame-accurate seeking and frame stepping
# verifiable by eye in a screenshot as well as by assertion.
set -euo pipefail

output="$(dirname "$0")/../tests/fixtures/sample.mp4"

ffmpeg -hide_banner -v error -y \
    -f lavfi -i "testsrc2=size=320x240:rate=25:duration=8" \
    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=8" \
    -c:v libx264 -preset veryslow -crf 30 -pix_fmt yuv420p -g 50 \
    -c:a aac -b:a 32k -shortest -movflags +faststart \
    "$output"

echo "Wrote $output"
ffprobe -hide_banner -v error -show_entries format=duration,size -of default "$output"
sha256sum "$output"
