#!/bin/sh
# Removes the MaiCord that install.sh put under a prefix (the one this script is installed in: DIR/opt/MaiCord).
#
#   ~/.local/opt/MaiCord/uninstall.sh            remove the program, the link, the desktop entry, icons and metadata
#   ~/.local/opt/MaiCord/uninstall.sh --purge    also remove the settings, data, state and cache of the current user
#
# Without --purge the program's data stays: $XDG_DATA_HOME/maicord, $XDG_STATE_HOME/maicord and
# $XDG_CACHE_HOME/maicord (by default under ~/.local/share, ~/.local/state and ~/.cache).
# shellcheck disable=SC2015 # "command -v tool && tool ... || true": the tool is optional and its failure is not one
set -eu

purge=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --purge) purge=1; shift ;;
        -h|--help) sed -n '2,/^# shellcheck/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
done

here="$(cd "$(dirname "$0")" && pwd -P)"
# Refuse to delete a directory that is not an install of this program.
[ -f "$here/.maicord-portable" ] && [ -f "$here/MaiCord" ] && [ "$(basename "$here")" = MaiCord ] || {
    echo "$here is not a MaiCord install directory (no .maicord-portable marker, or not named MaiCord)." >&2
    exit 1
}
app="$here"
prefix="$(dirname "$(dirname "$app")")"
application_id=io.github.maj113.MaiCord

# The link only when it still points at this install.
if [ -L "$prefix/bin/maicord" ] && [ "$(readlink "$prefix/bin/maicord")" = "$app/MaiCord" ]; then
    rm -f "$prefix/bin/maicord"
fi
rm -f "$prefix/share/applications/$application_id.desktop" "$prefix/share/metainfo/$application_id.metainfo.xml"
find "$prefix/share/icons/hicolor" -type f -name "$application_id.png" -delete 2>/dev/null || true

# The default link handlers this install registered, in the user's own list.
mimeapps="${XDG_CONFIG_HOME:-$HOME/.config}/mimeapps.list"
if [ -f "$mimeapps" ] && grep -q "$application_id.desktop" "$mimeapps"; then
    sed -i "/^x-scheme-handler\/\(discord\|maicord\)=$application_id.desktop;\{0,1\}$/d" "$mimeapps"
fi

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$prefix/share/applications" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f "$prefix/share/icons/hicolor" 2>/dev/null || true

# Last: this script lives in the directory it removes.
cd /
rm -rf "$app"

if [ "$purge" = 1 ]; then
    rm -rf "${XDG_DATA_HOME:-$HOME/.local/share}/maicord" "${XDG_STATE_HOME:-$HOME/.local/state}/maicord" \
        "${XDG_CACHE_HOME:-$HOME/.cache}/maicord"
    echo "MaiCord and its data are removed."
else
    echo "MaiCord is removed; its settings and data are kept (--purge removes them)."
fi
