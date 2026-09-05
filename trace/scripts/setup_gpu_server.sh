#!/usr/bin/env bash
#
# Set up TRACE on a Linux machine that has an NVIDIA GPU, and prove the GPU
# paths actually work there.
#
# Two things in TRACE have been written and never executed, for want of
# hardware: accelerated decode, and CUDA inference. Both ship with tests that
# skip themselves on a machine with no GPU and do real work on one. This script
# gets such a machine to the point where those tests run.
#
#   curl -fsSL https://raw.githubusercontent.com/jai2d4/j2/main/trace/scripts/setup_gpu_server.sh | bash
#
# or, from a clone:  ./scripts/setup_gpu_server.sh
#
# It is deliberately noisy about what it finds. A setup script that quietly
# continues after the GPU check fails would leave the same tests skipping, and
# the whole point is to stop them skipping.

set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/jai2d4/j2}"
BRANCH="${BRANCH:-windows-encrypted-ci}"
WORKDIR="${WORKDIR:-$HOME/j2}"

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
warn() { printf '\033[33m!! %s\033[0m\n' "$*"; }
die()  { printf '\033[31m!! %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 1. the GPU
say "Checking for an NVIDIA GPU"
if ! command -v nvidia-smi >/dev/null 2>&1; then
    die "nvidia-smi not found. Either there is no NVIDIA GPU here, or the driver
    is not installed. This script has nothing to offer a machine without one —
    TRACE builds and runs fine on CPU, but the two paths this exists to prove
    would still skip."
fi
nvidia-smi --query-gpu=name,driver_version,memory.total \
           --format=csv,noheader || die "nvidia-smi failed to query the device."

# Compute capability decides whether the pinned ONNX Runtime has kernels for
# this card. 8.9 is Ada (L4, RTX 40-series) and is covered; 12.0 is Blackwell
# (RTX 50-series) and is not, in prebuilt wheels at the time of writing.
#
# compute_cap is not a field every driver version knows, and an older driver
# answering "unrecognised field" is not a reason to stop: it says nothing about
# whether the card works. So the query is allowed to fail and the unknown case
# is handled below.
CC="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d ' ' || true)"
[[ -n "${CC}" ]] && echo "Compute capability: ${CC}"
case "${CC}" in
    8.9|8.6|8.0|7.5|7.0|9.0)
        echo "Compute capability ${CC}: covered by prebuilt ONNX Runtime CUDA wheels." ;;
    12.*)
        warn "Compute capability ${CC} is Blackwell. Prebuilt onnxruntime-gpu wheels
    ship kernels only up to sm_90, and the documented failure is a silent fall
    back to CPU. Decode will still work. TRACE will report the CPU honestly
    rather than claiming the GPU — that is what the execution-provider probe is
    for — but GPU inference will need a newer ONNX Runtime than the pinned one." ;;
    "")
        warn "This driver did not report a compute capability. That is a limitation
    of the query, not of the card. Decode should work; if GPU inference falls
    back to the CPU, TRACE will say so rather than claim the GPU." ;;
    *)
        warn "Compute capability ${CC} not recognised by this script. Decode should
    work; check ONNX Runtime's CUDA compatibility table for inference." ;;
esac

# ------------------------------------------------------- 2. build dependencies
say "Installing build dependencies"
if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update
    # The same set the Linux CI job installs, which is the configuration TRACE
    # is known to build in, plus libavdevice for attached cameras.
    sudo apt-get install -y --no-install-recommends \
        git curl cmake ninja-build g++ pkg-config \
        qt6-base-dev qt6-multimedia-dev \
        libavformat-dev libavcodec-dev libavutil-dev \
        libswscale-dev libswresample-dev libavdevice-dev \
        libsqlcipher-dev libssl-dev libgtest-dev
else
    warn "Not an apt system. Install the equivalents of: cmake ninja g++ pkg-config,
    Qt6 base + multimedia, FFmpeg dev libraries including libavdevice,
    SQLCipher, OpenSSL, GoogleTest."
fi

# ------------------------------------------------------------------ 3. source
say "Fetching TRACE"
if [[ -d "${WORKDIR}/.git" ]]; then
    git -C "${WORKDIR}" fetch origin "${BRANCH}"
    git -C "${WORKDIR}" checkout "${BRANCH}"
    git -C "${WORKDIR}" pull origin "${BRANCH}"
else
    git clone --branch "${BRANCH}" "${REPO_URL}" "${WORKDIR}"
fi
cd "${WORKDIR}/trace"

# ------------------------------------------- 4. ONNX Runtime and the model
say "Fetching the CUDA build of ONNX Runtime"
# --gpu is the whole point: the default package is CPU-only, which is why
# cudaAvailable() is false on an ordinary checkout.
./scripts/fetch_onnxruntime.sh --gpu

say "Fetching the detection model"
./scripts/fetch_models.sh || warn "Model fetch failed; detection tests will skip."

# ------------------------------------------------------------------ 5. build
say "Configuring and building"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

# --------------------------------------------------- 6. what the GPU changes
say "What TRACE sees on this machine"
ctest --test-dir build -R 'HardwareDecode' --output-on-failure || true

say "Running the two tests that have never executed on any machine"
# These skip on a GPU-less machine and do real work here. HardwareDecodeOnRealHardware
# decodes the same frames both ways and compares them byte for byte;
# ExecutionProvider reads which provider actually ran the model rather than
# which one was requested.
ctest --test-dir build -R 'HardwareDecodeOnRealHardware|ExecutionProvider' \
      --output-on-failure || warn "One of the GPU tests failed — that is a real
    result and worth reporting, not something to work around."

say "Full suite"
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure --timeout 400

cat <<'DONE'

== Done ==

To let Claude Code work on this machine — with this filesystem and this GPU —
run, from this directory:

    claude remote-control

It prints a URL and a QR code. Open either, and the session runs here rather
than in a cloud container with no GPU.

If it refuses, run `claude` once in this directory first to accept the
workspace-trust prompt, and `/login` if you are not signed in.
DONE
