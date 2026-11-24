#!/bin/sh
# dist/package.sh — builds a release package of Nihil for one platform:
#
#   dist/package.sh linux-x86_64 OUTDIR NIHILC1 IDE_SERVER
#   dist/package.sh macos-arm64  OUTDIR NIHILC1_NATIVE RT_O IDE_SERVER [IDE_APP]
#
# IDE_SERVER is ide/server.nihil compiled for the platform (`nihil ide`); with the IDE go its
# page, SPEC.md and the examples, in share/nihil.
# writes OUTDIR/nihil-PLATFORM.tar.gz (the name stays the same in every release, so
# releases/latest/download/nihil-PLATFORM.tar.gz is always the newest) and its .sha256.
# Inside: nihil-VERSION/{bin/nihil, libexec/nihil/…, VERSION, LICENSE, README.md, CHANGELOG.md}.
#   NIHIL_PACKAGE_VERSION   the version the package says it is (default: VERSION), e.g. 4.2.4+nightly.10b3a6f
set -eu
cd "$(dirname "$0")/.."
[ $# -ge 3 ] || { echo "usage: dist/package.sh linux-x86_64|macos-arm64 OUTDIR COMPILER [RT_O]" >&2; exit 2; }
platform=$1
outdir=$2
version=${NIHIL_PACKAGE_VERSION:-$(cat VERSION)}
mkdir -p "$outdir"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
root=$stage/nihil-$version
mkdir -p "$root/bin" "$root/libexec/nihil" "$root/share/nihil/ide"
cp dist/nihil "$root/bin/nihil"
chmod 755 "$root/bin/nihil"
cp LICENSE README.md CHANGELOG.md "$root/"
printf '%s\n' "$version" > "$root/VERSION"
cp SPEC.md "$root/share/nihil/"
cp -R ide/static "$root/share/nihil/ide/static"
mkdir -p "$root/share/nihil/examples"
cp examples/*.nihil "$root/share/nihil/examples/"
cp -R examples/learn "$root/share/nihil/examples/learn"
rm -f "$root/share/nihil/examples/learn/"*.dat* "$root/share/nihil/examples/learn/notes.txt"
case $platform in
    linux-x86_64)
        [ $# -ge 4 ] || { echo "linux-x86_64 needs NIHILC1 and IDE_SERVER" >&2; exit 2; }
        cp "$3" "$root/libexec/nihil/nihilc1"
        cp "$4" "$root/libexec/nihil/ide-server"
        chmod 755 "$root/libexec/nihil/nihilc1" "$root/libexec/nihil/ide-server"
        ;;
    macos-arm64)
        [ $# -ge 5 ] || { echo "macos-arm64 needs NIHILC1_NATIVE, RT_O and IDE_SERVER" >&2; exit 2; }
        cp "$3" "$root/libexec/nihil/nihilc1-native"
        cp "$5" "$root/libexec/nihil/ide-server"
        chmod 755 "$root/libexec/nihil/nihilc1-native" "$root/libexec/nihil/ide-server"
        cp "$4" "$root/libexec/nihil/rt.o"
        cp macos/translate.py "$root/libexec/nihil/translate.py"
        cp macos/translate_rt.py "$root/libexec/nihil/translate_rt.py"     # the runtime's translation
        # IDE_APP: "Nihil IDE.app" from macos/ide-app/build.sh, the window of `nihil ide`
        if [ $# -ge 6 ]; then cp -R "$6" "$root/libexec/nihil/Nihil IDE.app"; fi
        ;;
    *)
        echo "unknown platform: $platform" >&2
        exit 2
        ;;
esac
tarball=$outdir/nihil-$platform.tar.gz
tar -czf "$tarball" -C "$stage" "nihil-$version"
if command -v sha256sum >/dev/null 2>&1; then
    (cd "$outdir" && sha256sum "nihil-$platform.tar.gz" > "nihil-$platform.tar.gz.sha256")
else
    (cd "$outdir" && shasum -a 256 "nihil-$platform.tar.gz" > "nihil-$platform.tar.gz.sha256")
fi
echo "$tarball"
