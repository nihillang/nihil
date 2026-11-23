#!/bin/sh
# dist/seed.sh DIR — the compiler of the latest release (Linux x86_64), checked against its
# .sha256, in DIR/nihilc1. `make` uses it to build stage 1 when nasm or ld is missing, in place
# of stage 0: it compiles src1/ and the compiler that comes out compiles src1/ again.
#   NIHIL_SEED_URL   another package (default: the latest release's)
set -eu
dir=$1
url=${NIHIL_SEED_URL:-https://github.com/nihillang/nihil/releases/latest/download/nihil-linux-x86_64.tar.gz}
fail() { echo "dist/seed.sh: $*" >&2; exit 1; }
[ "$(uname -s) $(uname -m)" = "Linux x86_64" ] || fail "the seed is a Linux x86_64 compiler; on this machine, build stage 0 (nasm, ld) or see docs/macos-arm64.md"
command -v curl >/dev/null 2>&1 || fail "curl is needed (or nasm and ld, for stage 0)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
echo "stage 1 from a seed (no nasm or ld): $url"
curl -fsSL "$url" -o "$work/nihil.tar.gz" || fail "could not download $url"
curl -fsSL "$url.sha256" -o "$work/nihil.tar.gz.sha256" || fail "could not download $url.sha256"
expected=$(cut -d ' ' -f 1 < "$work/nihil.tar.gz.sha256")
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$work/nihil.tar.gz" | cut -d ' ' -f 1)
else
    actual=$(shasum -a 256 "$work/nihil.tar.gz" | cut -d ' ' -f 1)
fi
[ "$expected" = "$actual" ] || fail "the package's checksum does not match"
mkdir -p "$work/unpacked" "$dir"
tar -xzf "$work/nihil.tar.gz" -C "$work/unpacked" --strip-components 1
[ -x "$work/unpacked/libexec/nihil/nihilc1" ] || fail "no libexec/nihil/nihilc1 in the package"
cp "$work/unpacked/libexec/nihil/nihilc1" "$dir/nihilc1"
echo "seed: $dir/nihilc1 (Nihil $(cat "$work/unpacked/VERSION" 2>/dev/null || echo '?'))"
