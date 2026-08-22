#!/usr/bin/env bash
#
# Downloads the ONNX Runtime C/C++ package TRACE builds against.
#
# The runtime is not vendored: it is a large third-party binary with its own
# licence, so it is fetched on demand into third_party/onnxruntime/ (which is
# git-ignored) and the build is pointed at it with -DTRACE_ONNXRUNTIME_ROOT.
#
#   ./scripts/fetch_onnxruntime.sh            # CPU build, default version
#   ./scripts/fetch_onnxruntime.sh --gpu      # CUDA build (needs a CUDA runtime)
#   ONNXRUNTIME_VERSION=1.18.0 ./scripts/fetch_onnxruntime.sh
#
# Nothing here is run automatically by the build. TRACE never downloads a
# runtime or a model behind the operator's back.
set -euo pipefail

VERSION="${ONNXRUNTIME_VERSION:-1.17.3}"
FLAVOUR="cpu"
if [[ "${1:-}" == "--gpu" ]]; then FLAVOUR="gpu"; fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${script_dir}/.." && pwd)"
destination="${root}/third_party/onnxruntime"

case "$(uname -s)" in
    Linux)  platform="linux" ;;
    Darwin) platform="osx" ;;
    *) echo "Unsupported platform $(uname -s). On Windows use the .zip package from" \
            "https://github.com/microsoft/onnxruntime/releases" >&2; exit 1 ;;
esac

case "$(uname -m)" in
    x86_64|amd64) arch="x64" ;;
    aarch64|arm64) arch="aarch64" ;;
    *) echo "Unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac

if [[ "${FLAVOUR}" == "gpu" ]]; then
    package="onnxruntime-${platform}-${arch}-gpu-${VERSION}"
else
    package="onnxruntime-${platform}-${arch}-${VERSION}"
fi
url="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}/${package}.tgz"

if [[ -f "${destination}/VERSION_NUMBER" ]] && \
   [[ "$(cat "${destination}/VERSION_NUMBER")" == "${VERSION}" ]]; then
    echo "ONNX Runtime ${VERSION} is already installed in ${destination}"
    exit 0
fi

echo "Fetching ${package}"
echo "  from ${url}"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

curl --fail --location --show-error --silent --output "${work}/onnxruntime.tgz" "${url}"
tar -xzf "${work}/onnxruntime.tgz" -C "${work}"

rm -rf "${destination}"
mkdir -p "$(dirname "${destination}")"
mv "${work}/${package}" "${destination}"

echo
echo "Installed ONNX Runtime $(cat "${destination}/VERSION_NUMBER") in ${destination}"
echo "Licence: ${destination}/LICENSE (MIT), third-party notices alongside it."
echo
echo "Configure TRACE with:"
echo "  cmake -S . -B build -DTRACE_WITH_ONNXRUNTIME=ON \\"
echo "        -DTRACE_ONNXRUNTIME_ROOT=${destination}"
