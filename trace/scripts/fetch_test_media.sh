#!/usr/bin/env bash
#
# Downloads the test clip used by the tests that exercise real inference.
#
# vtest.avi is a short street scene shipped with OpenCV (BSD-3-Clause). It is
# not committed: it is several megabytes of third-party media, and the tests
# that need it skip themselves — saying so — when it is absent.
#
#   ./scripts/fetch_test_media.sh
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${script_dir}/.." && pwd)"
destination="${root}/tests/fixtures/external"

file_name="vtest.avi"
url="https://raw.githubusercontent.com/opencv/opencv/4.9.0/samples/data/vtest.avi"
sha256="45cddc9490be69345cbdab64ca583be65987e864ca408038e648db99e10516cf"

target="${destination}/${file_name}"
mkdir -p "${destination}"

if [[ -f "${target}" ]]; then
    existing="$(sha256sum "${target}" | cut -d' ' -f1)"
    if [[ "${existing}" == "${sha256}" ]]; then
        echo "${file_name} is already present at ${target}"
        exit 0
    fi
    echo "A different ${file_name} is already at ${target} (SHA-256 ${existing})." >&2
    exit 1
fi

echo "Fetching ${file_name} (OpenCV sample data, BSD-3-Clause)"
echo "  from ${url}"
curl --fail --location --show-error --silent --output "${target}.partial" "${url}"

computed="$(sha256sum "${target}.partial" | cut -d' ' -f1)"
if [[ "${computed}" != "${sha256}" ]]; then
    rm -f "${target}.partial"
    echo "SHA-256 mismatch — nothing was installed." >&2
    echo "  expected ${sha256}" >&2
    echo "  received ${computed}" >&2
    exit 1
fi

mv "${target}.partial" "${target}"
echo "Installed ${target}"
echo "  SHA-256 ${computed}"
