#!/usr/bin/env bash
# Downloads the public-domain ENABLE word list into data/lexica/enable1.txt.
#
# The raw list is stored as published (lowercase, one word per line); length
# and alphabet filtering happens in the engine's loader so every lexicon goes
# through the same validation. The checksum pins the exact revision the tests
# were written against.

set -euo pipefail

readonly URL="https://raw.githubusercontent.com/dolph/dictionary/master/enable1.txt"
readonly SHA256="3f16130220645692ed49c7134e24a18504c2ca55b3c012f7290e3e77c63b1a89"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest_dir="${repo_root}/data/lexica"
dest="${dest_dir}/enable1.txt"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  else
    shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

if [[ -f "${dest}" && "$(sha256_of "${dest}")" == "${SHA256}" ]]; then
  echo "ENABLE already present: ${dest}"
  exit 0
fi

mkdir -p "${dest_dir}"
tmp="$(mktemp "${dest_dir}/enable1.XXXXXX")"
trap 'rm -f "${tmp}"' EXIT

echo "Downloading ENABLE from ${URL}"
curl --fail --silent --show-error --location --output "${tmp}" "${URL}"

actual="$(sha256_of "${tmp}")"
if [[ "${actual}" != "${SHA256}" ]]; then
  echo "Checksum mismatch for enable1.txt: expected ${SHA256}, got ${actual}" >&2
  exit 1
fi

chmod 644 "${tmp}"
mv "${tmp}" "${dest}"
trap - EXIT
echo "Wrote ${dest} ($(wc -l < "${dest}" | tr -d ' ') lines)"
