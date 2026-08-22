#!/usr/bin/env bash
#
# Downloads a detection model artefact into models/ and verifies its SHA-256
# against the digest recorded in TRACE's model catalogue
# (ai/detection/models/model_manager.cpp).
#
#   ./scripts/fetch_models.sh                 # the default model, YOLOX-Tiny
#   ./scripts/fetch_models.sh yolox-s         # the larger sibling
#   TRACE_MODEL_DIR=/srv/models ./scripts/fetch_models.sh
#
# TRACE itself never downloads a model. An absent model is reported to the
# operator, with the path it was expected at and this script's name — the
# application does not reach out to the network on its own.
#
# A digest mismatch is a hard failure: the file is deleted and nothing is
# installed. Two files with the same name are not necessarily the same artefact,
# and a model that produced evidence has to be identifiable.
set -euo pipefail

MODEL="${1:-yolox-tiny}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${script_dir}/.." && pwd)"
destination="${TRACE_MODEL_DIR:-${root}/models}"

case "${MODEL}" in
    yolox-tiny)
        file_name="yolox_tiny.onnx"
        url="https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_tiny.onnx"
        sha256="427cc366d34e27ff7a03e2899b5e3671425c262ea2291f88bb942bc1cc70b0f7"
        licence="Apache-2.0 (Megvii YOLOX)"
        ;;
    yolox-s)
        file_name="yolox_s.onnx"
        url="https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_s.onnx"
        # No digest is recorded in the catalogue for this optional model, so
        # TRACE reports the digest it computes instead of asserting a match.
        sha256=""
        licence="Apache-2.0 (Megvii YOLOX)"
        ;;
    *)
        echo "Unknown model '${MODEL}'. Known: yolox-tiny, yolox-s" >&2
        exit 1
        ;;
esac

target="${destination}/${file_name}"
mkdir -p "${destination}"

if [[ -f "${target}" ]]; then
    existing="$(sha256sum "${target}" | cut -d' ' -f1)"
    if [[ -z "${sha256}" || "${existing}" == "${sha256}" ]]; then
        echo "${MODEL} is already installed at ${target}"
        echo "  SHA-256 ${existing}"
        exit 0
    fi
    echo "A different file is already at ${target} (SHA-256 ${existing})." >&2
    echo "Remove it first if you meant to replace it." >&2
    exit 1
fi

echo "Fetching ${MODEL} (${licence})"
echo "  from ${url}"
echo "  to   ${target}"
curl --fail --location --show-error --silent --output "${target}.partial" "${url}"

computed="$(sha256sum "${target}.partial" | cut -d' ' -f1)"
if [[ -n "${sha256}" && "${computed}" != "${sha256}" ]]; then
    rm -f "${target}.partial"
    echo >&2
    echo "SHA-256 mismatch — nothing was installed." >&2
    echo "  expected ${sha256}" >&2
    echo "  received ${computed}" >&2
    exit 1
fi

mv "${target}.partial" "${target}"
echo
echo "Installed ${file_name}"
echo "  SHA-256 ${computed}"
if [[ -z "${sha256}" ]]; then
    echo "  (no digest is recorded for this optional model; TRACE records the digest"
    echo "   of the file that actually runs on every analysis run)"
fi
