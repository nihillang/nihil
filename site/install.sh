#!/bin/sh
# Installs Nihil (https://nihil-lang.dev):
#
#   curl -fsSL https://nihil-lang.dev/install.sh | sh
#
# Linux x86_64 and macOS on Apple silicon. It downloads the latest release from GitHub, checks
# its SHA-256, unpacks it into ~/.nihil and links ~/.local/bin/nihil to it. Nothing is installed
# outside your home directory, and no sudo is needed.
#
#   NIHIL_HOME=dir      where to unpack it (default ~/.nihil)
#   NIHIL_APPS=dir      macOS: where the copy of Nihil IDE.app goes (default ~/Applications); none: no app
#                       copy (macOS) and no menu entry (Linux)
#   NIHIL_BIN=dir       where to link the nihil command (default ~/.local/bin)
#   NIHIL_VERSION=x.y.z a given release instead of the latest; nightly: the newest build of main
#                       (built by GitHub on every push, not a release: it may break)
set -eu

say() { printf '%s\n' "$*"; }
fail() { printf 'nihil install: %s\n' "$*" >&2; exit 1; }

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64 | Linux-amd64) platform=linux-x86_64 ;;
    Darwin-arm64) platform=macos-arm64 ;;
    Darwin-x86_64) fail "Macs with Intel processors are not supported yet (Apple silicon only)" ;;
    Linux-*) fail "only Linux on x86_64 is supported for now (found $(uname -m)); on Windows, use WSL" ;;
    *) fail "unsupported system: $(uname -s) $(uname -m)" ;;
esac

if [ "$platform" = macos-arm64 ]; then
    # translating and linking programs needs clang, objdump and python3
    if ! xcode-select -p >/dev/null 2>&1 || ! command -v clang >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
        fail "Nihil on macOS needs the Xcode Command Line Tools: run 'xcode-select --install', then this installer again"
    fi
fi
command -v curl >/dev/null 2>&1 || fail "curl is needed"
command -v tar >/dev/null 2>&1 || fail "tar is needed"

repo=https://github.com/nihillang/nihil/releases
if [ "${NIHIL_VERSION:-}" = nightly ]; then
    base=$repo/download/nightly
elif [ -n "${NIHIL_VERSION:-}" ]; then
    base=$repo/download/v${NIHIL_VERSION#v}
else
    base=$repo/latest/download
fi
url=${NIHIL_URL:-$base/nihil-$platform.tar.gz}
dest=${NIHIL_HOME:-$HOME/.nihil}
bindir=${NIHIL_BIN:-$HOME/.local/bin}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
say "Downloading $url"
curl -fsSL "$url" -o "$work/nihil.tar.gz" || fail "could not download $url"
curl -fsSL "$url.sha256" -o "$work/nihil.tar.gz.sha256" || fail "could not download the checksum ($url.sha256)"

expected=$(cut -d ' ' -f 1 < "$work/nihil.tar.gz.sha256")
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$work/nihil.tar.gz" | cut -d ' ' -f 1)
else
    actual=$(shasum -a 256 "$work/nihil.tar.gz" | cut -d ' ' -f 1)
fi
[ -n "$expected" ] && [ "$expected" = "$actual" ] || fail "the checksum does not match: the download is corrupt or was changed"

mkdir -p "$work/unpacked"
tar -xzf "$work/nihil.tar.gz" -C "$work/unpacked" --strip-components 1
[ -x "$work/unpacked/bin/nihil" ] || fail "the package has no bin/nihil"

# replace the previous installation only once the new one is complete
rm -rf "$dest.new"
mkdir -p "$(dirname "$dest")"
mv "$work/unpacked" "$dest.new"
rm -rf "$dest"
mv "$dest.new" "$dest"
mkdir -p "$bindir"
ln -sf "$dest/bin/nihil" "$bindir/nihil"

# macOS: a copy of the IDE's app in ~/Applications, for Launchpad and Spotlight (NIHIL_APPS=none: not)
apps=${NIHIL_APPS:-$HOME/Applications}
app="$dest/libexec/nihil/Nihil IDE.app"
if [ "$(uname -s)" = Darwin ] && [ -d "$app" ] && [ "$apps" != none ]; then
    copy="$apps/Nihil IDE.app"
    if [ -d "$copy" ] && ! grep -q 'dev.nihil-lang.ide' "$copy/Contents/Info.plist" 2>/dev/null; then
        say "$copy exists and is not Nihil's: left as it is"
    else
        mkdir -p "$apps"
        rm -rf "$copy"
        cp -R "$app" "$copy"
        printf '%s\n' "$dest" > "$copy/Contents/Resources/nihil-home"
        codesign --force --sign - "$copy" >/dev/null 2>&1 || true
        say "The IDE's app: $copy (Launchpad, Spotlight: Nihil IDE)"
    fi
fi

# Linux: a menu entry for the IDE, with its icon (NIHIL_APPS=none: not)
if [ "$(uname -s)" = Linux ] && [ -x "$dest/libexec/nihil/ide-server" ] && [ "$apps" != none ]; then
    menu=${XDG_DATA_HOME:-$HOME/.local/share}/applications
    mkdir -p "$menu"
    cat > "$menu/nihil-ide.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Nihil IDE
Comment=The IDE of the Nihil language
Exec="$dest/bin/nihil" ide
Icon=$dest/share/nihil/ide/static/icon-512.png
Terminal=false
Categories=Development;IDE;
StartupWMClass=NihilIDE
DESKTOP
    say "The IDE's menu entry: $menu/nihil-ide.desktop (Nihil IDE)"
fi

say "Installed $("$dest/bin/nihil" version) in $dest"
case ":$PATH:" in
    *":$bindir:"*)
        say "Try it:  printf '%s\\n' '>> \"Hello, Nihil!\\n\";' > hello.nihil && nihil run hello.nihil"
        ;;
    *)
        say ""
        say "$bindir is not on your PATH. Add this line to your shell's startup file (~/.zshrc or ~/.bashrc):"
        say "  export PATH=\"$bindir:\$PATH\""
        ;;
esac
