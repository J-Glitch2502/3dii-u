#!/bin/bash
# Installs the Wii U toolchain natively on Windows, the same versions the WSL builds used, under C:\devkitPro. Run once with MSYS2's bash:
#   C:\msys64\usr\bin\bash.exe --noprofile --norc tools/win/install_toolchain.sh
# It uses MSYS2's pacman but keeps its own config, database and keys under C:\devkitPro, so MSYS2's settings stay as they are.
# Nothing else here needs MSYS2 afterwards: these are ordinary Windows programs. The downloaded packages stay in C:\devkitPro\pacman\cache,
# because devkitPro's server only keeps the newest versions, and pinned ones are what's been tested on the console.
set -e
export PATH=/usr/bin
HERE="$(cd "$(dirname "$0")" && pwd)"
R=/c/devkitPro
P=$R/pacman
mkdir -p "$P/db" "$P/cache" "$P/gnupg"
cat > "$P/pacman.conf" <<EOF
[options]
RootDir     = $R
DBPath      = $P/db/
CacheDir    = $P/cache/
LogFile     = $P/pacman.log
GPGDir      = $P/gnupg/
HookDir     = $P/hooks/
Architecture = x86_64
SigLevel    = Required DatabaseOptional
LocalFileSigLevel = Optional
ParallelDownloads = 3

[dkp-libs]
Server = https://pkg.devkitpro.org/packages

[dkp-windows]
Server = https://pkg.devkitpro.org/packages/windows/\$arch
EOF
PK="pacman-key --config $P/pacman.conf --gpgdir $P/gnupg"
if [ ! -f "$P/gnupg/trustdb.gpg" ]; then
    $PK --init >/dev/null 2>&1
    $PK --add "$HERE/../../toolchain/keyring/devkitpro.gpg" 2>&1 | tail -1
    cut -d: -f1 "$HERE/../../toolchain/keyring/devkitpro-trusted" | grep -v '^#' | while read -r f; do [ -n "$f" ] && $PK --lsign-key "$f" 2>&1 | tail -1; done
fi
PAC="pacman --config $P/pacman.conf --noconfirm"
$PAC -Sy 2>&1 | tail -3
if [ "$1" = list ]; then
    $PAC -Sl dkp-windows | grep -E 'devkitppc|wut|general-tools|devkitPPC' | cut -c1-80
    $PAC -Sl dkp-libs | grep -E 'devkitppc|wut |wut-|devkitPPC' | cut -c1-80
    gpgconf --homedir "$P/gnupg" --kill all 2>/dev/null || true
    echo finished
    exit 0
fi
# -dd: their dependencies only name the MSYS2 side of a full devkitPro install. --noscriptlet: those only set up that install's shell.
$PAC -S -dd --noscriptlet --needed "$@" 2>&1 | grep -vE 'checking|loading|resolving' | tail -12 | cut -c1-120
gpgconf --homedir "$P/gnupg" --kill all 2>/dev/null || true
$PAC -Q | cut -c1-60
echo finished
