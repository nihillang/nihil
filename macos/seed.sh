#!/bin/bash
# seed.sh OUT — a first translator for this Mac, when there is none yet: macos/translate.nihil compiled
# by a Nihil that is already here (NIHIL_SEED=command, or `nihil` on the PATH) or, without one, by the
# latest release's package for macOS (downloaded and checked against its .sha256). That translator runs on
# the release's runtime; macos/translator.sh then makes the translator again with this checkout's.
#   NIHIL_SEED_URL   another package (default: the latest release's)
set -eu
out=$1
cd "$(dirname "$0")/.."
fail() { echo "macos/seed.sh: $*" >&2; exit 1; }
nihil=${NIHIL_SEED:-}
if [ -z "$nihil" ] && command -v nihil >/dev/null 2>&1 && [ -z "${NIHIL_SEED_URL:-}" ]; then nihil=nihil; fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ -z "$nihil" ]; then
    url=${NIHIL_SEED_URL:-https://github.com/nihillang/nihil/releases/latest/download/nihil-macos-arm64.tar.gz}
    echo "the translator from a seed: $url"
    curl -fsSL "$url" -o "$work/nihil.tar.gz" || fail "could not download $url"
    curl -fsSL "$url.sha256" -o "$work/nihil.tar.gz.sha256" || fail "could not download $url.sha256"
    [ "$(cut -d ' ' -f 1 < "$work/nihil.tar.gz.sha256")" = "$(shasum -a 256 "$work/nihil.tar.gz" | cut -d ' ' -f 1)" ] ||
        fail "the package's checksum does not match"
    mkdir -p "$work/unpacked"
    tar -xzf "$work/nihil.tar.gz" -C "$work/unpacked" --strip-components 1
    nihil=$work/unpacked/bin/nihil
    [ -x "$nihil" ] || fail "no bin/nihil in the package"
fi
mkdir -p "$(dirname "$out")"
"$nihil" macos/translate.nihil "$out" || fail "$nihil could not compile macos/translate.nihil"
echo "seed: $out (compiled by $nihil)"
