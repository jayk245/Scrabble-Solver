#!/usr/bin/env bash
# Downloads a word list into data/lexica/.
#
# Usage: scripts/fetch_lexicon.sh [enable|wow24]   (default: enable)
#
#   enable  The public-domain ENABLE list -> data/lexica/enable1.txt. The
#           unit and integration tests are written against this revision.
#   wow24   WGPO Official Words 2024 (WOW24), the Word Game Players
#           Organization's tournament lexicon -> data/lexica/wow24.txt.
#           Free for non-commercial use only; it must not be redistributed
#           in a commercial product, so it is never committed (see
#           data/lexica/README.md).
#
# Lists are stored as published; case, CRLF and length/alphabet filtering are
# handled by the engine's loader so every lexicon goes through the same
# validation. Each checksum pins the exact revision that was verified.

set -euo pipefail

list="${1:-enable}"
case "${list}" in
  enable)
    readonly NAME="ENABLE"
    readonly URL="https://raw.githubusercontent.com/dolph/dictionary/master/enable1.txt"
    readonly SHA256="3f16130220645692ed49c7134e24a18504c2ca55b3c012f7290e3e77c63b1a89"
    readonly FILE="enable1.txt"
    ;;
  wow24)
    readonly NAME="WOW24"
    readonly URL="https://wordgameplayers.org/wp-content/uploads/2024/03/FINAL-WOW24-Full-Alphabetical.txt"
    readonly SHA256="84c2b77b8f3e98693cb280d6906b5fc3787073bd57e0fe9c3ed89ffd8fbfc601"
    readonly FILE="wow24.txt"
    ;;
  *)
    echo "Unknown word list '${list}'; expected 'enable' or 'wow24'" >&2
    exit 2
    ;;
esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest_dir="${repo_root}/data/lexica"
dest="${dest_dir}/${FILE}"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  else
    shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

if [[ -f "${dest}" && "$(sha256_of "${dest}")" == "${SHA256}" ]]; then
  echo "${NAME} already present: ${dest}"
  exit 0
fi

mkdir -p "${dest_dir}"
tmp="$(mktemp "${dest_dir}/${FILE%.txt}.XXXXXX")"
trap 'rm -f "${tmp}"' EXIT

echo "Downloading ${NAME} from ${URL}"
# Some hosts reject curl's default user agent.
curl --fail --silent --show-error --location --user-agent "Mozilla/5.0" \
  --output "${tmp}" "${URL}"

actual="$(sha256_of "${tmp}")"
if [[ "${actual}" != "${SHA256}" ]]; then
  echo "Checksum mismatch for ${FILE}: expected ${SHA256}, got ${actual}" >&2
  exit 1
fi

chmod 644 "${tmp}"
mv "${tmp}" "${dest}"
trap - EXIT
# The compiled cache belongs to the previous revision, if any.
rm -f "${dest%.txt}.gaddag"
echo "Wrote ${dest} ($(wc -l < "${dest}" | tr -d ' ') lines)"
