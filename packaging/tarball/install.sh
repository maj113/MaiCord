#!/bin/sh
# Installs MaiCord for the current user, from the unpacked release tarball.
#
#   ./install.sh                     install under ~/.local
#   ./install.sh --prefix DIR        install under DIR (DIR/opt/MaiCord, DIR/bin, DIR/share)
#   ./install.sh --no-url-handler    do not make MaiCord the handler of discord:// and maicord:// links
#   ./uninstall.sh                   (the installed copy, DIR/opt/MaiCord/uninstall.sh) removes it again
#
# What it puts where (nothing outside the prefix is written except the default link handler, which is the user's own
# mimeapps list):
#   DIR/opt/MaiCord/            the program, fonts/, licenses/, uninstall.sh and .maicord-portable. The user owns
#                               it, because this install updates itself: the marker file tells the program that
#                               it is the self-updating channel (packaging/README.md).
#   DIR/bin/maicord             a link to the program
#   DIR/share/applications/     the desktop entry, with the program's full path in Exec
#   DIR/share/icons/hicolor/    the icons
#   DIR/share/metainfo/         the AppStream metadata
# The program's settings and data are not here: they live under $XDG_DATA_HOME/maicord (and the state and cache
# directories of the same name), which install and uninstall leave alone.
# shellcheck disable=SC2015 # "command -v tool && tool ... || true": the tool is optional and its failure is not one
set -eu

prefix="$HOME/.local"
url_handler=1
while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) [ "$#" -ge 2 ] || { echo "--prefix needs a directory" >&2; exit 2; }; prefix="$2"; shift 2 ;;
        --no-url-handler) url_handler=0; shift ;;
        -h|--help) sed -n '2,/^# shellcheck/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
done

here="$(cd "$(dirname "$0")" && pwd -P)"
[ -f "$here/MaiCord" ] && [ -f "$here/.maicord-portable" ] || { echo "Run this script from the unpacked MaiCord tarball." >&2; exit 1; }
mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd -P)"
# The path goes into a desktop entry's Exec line, where spaces and quotes need escapes: keep to plain characters.
case "$prefix" in
    *[!A-Za-z0-9._/+@:-]*) echo "The prefix $prefix has characters a desktop entry cannot hold unquoted; use another." >&2; exit 1 ;;
esac

app="$prefix/opt/MaiCord"
application_id=io.github.maj113.MaiCord

# Replace an earlier install atomically enough: the new tree is made beside the old one, then swapped in.
mkdir -p "$prefix/opt" "$prefix/bin" "$prefix/share"
stage="$app.installing.$$"
rm -rf "$stage"
mkdir -p "$stage"
cp -R "$here/MaiCord" "$here/fonts" "$here/licenses" "$here/.maicord-portable" "$here/uninstall.sh" "$stage/"
for extra in README.md LICENSE; do
    [ ! -f "$here/$extra" ] || cp "$here/$extra" "$stage/"
done
chmod 0755 "$stage/MaiCord" "$stage/uninstall.sh"
rm -rf "$app.previous"
[ ! -e "$app" ] || mv "$app" "$app.previous"
mv "$stage" "$app"
rm -rf "$app.previous"

ln -sf "$app/MaiCord" "$prefix/bin/maicord"

# share/: everything of the tarball's share directory, with the program's own path in the desktop entry.
(cd "$here/share" && find . -type f) | while IFS= read -r file; do
    mkdir -p "$prefix/share/$(dirname "$file")"
    cp "$here/share/$file" "$prefix/share/$file"
done
sed -i "s|^Exec=.*|Exec=$app/MaiCord %U|" "$prefix/share/applications/$application_id.desktop"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$prefix/share/applications" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f "$prefix/share/icons/hicolor" || true
if [ "$url_handler" = 1 ] && command -v xdg-mime >/dev/null 2>&1; then
    xdg-mime default "$application_id.desktop" x-scheme-handler/discord x-scheme-handler/maicord || true
fi

echo "MaiCord is installed in $app"
case ":$PATH:" in
    *":$prefix/bin:"*) ;;
    *) echo "Add $prefix/bin to PATH to start it as 'maicord'; the application menu entry works already." ;;
esac
