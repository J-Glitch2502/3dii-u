#!/usr/bin/env bash
# Launch the packaged Windows Builder with Steam Proton, in its own prefix.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exe=${THREEDII_BUILDER_EXE:-"$here/3Dii U Builder.exe"}
proton=${THREEDII_PROTON:-}
steam=${STEAM_COMPAT_CLIENT_INSTALL_PATH:-"$HOME/.local/share/Steam"}
if [[ -z "$proton" ]]; then
    for base in "$steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        candidate="$base/steamapps/common/Proton 11.0/proton"
        if [[ -x "$candidate" ]]; then
            proton=$candidate
            steam=$base
            break
        fi
    done
fi
if [[ ! -x "$proton" || ! -f "$exe" ]]; then
    echo 'Install Steam Proton 11.0 and keep this launcher beside 3Dii U Builder.exe.' >&2
    echo 'For another Steam library, set THREEDII_PROTON to its proton script.' >&2
    exit 1
fi
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam"
export STEAM_COMPAT_DATA_PATH=${THREEDII_PROTON_DATA:-"${XDG_DATA_HOME:-$HOME/.local/share}/3dii-u-builder/proton"}
export SteamAppId=0 SteamGameId=0
mkdir -p -- "$STEAM_COMPAT_DATA_PATH"
# Qt imports Windows' icuuc.dll name; Proton supplies the same exports in
# icu.dll. Keep this alias in the Builder's own prefix, never in the package.
if [[ ! -f "$STEAM_COMPAT_DATA_PATH/pfx/system.reg" ]]; then
    "$proton" run cmd /c exit 0
fi
system32="$STEAM_COMPAT_DATA_PATH/pfx/drive_c/windows/system32"
icu="$(dirname -- "$proton")/files/lib/wine/x86_64-windows/icu.dll"
if [[ ! -e "$system32/icuuc.dll" && -f "$icu" ]]; then
    ln -sfn -- "$icu" "$system32/icuuc.dll"
fi
# Proton's default prefix can alias the missing Segoe UI font to Times New
# Roman. Use its supplied sans-serif replacement in this dedicated prefix.
if [[ ! -f "$STEAM_COMPAT_DATA_PATH/3dii-fonts-v1" ]]; then
    for font in 'Segoe UI' 'Segoe UI Semibold'; do
        "$proton" run reg add 'HKCU\Software\Wine\Fonts\Replacements' /v "$font" /t REG_SZ /d Arial /f
    done
    touch "$STEAM_COMPAT_DATA_PATH/3dii-fonts-v1"
fi
exec "$proton" run "$exe" "$@"
