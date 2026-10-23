#!/bin/sh
# X-Streemly release packaging (CLAUDE.md M5): strip one core and zip it
# with the core info file, README, licence and the bundled libraries'
# licences.
#
#   package.sh <core> <strip command> <platform name> <version>
set -eu
core=$1 strip=$2 platform=$3 version=$4
name=xstreemly-$version-$platform
out=build/package/$name
rm -rf "$out"
mkdir -p "$out/licenses"
cp "$core" "$out/"
$strip "$out/$(basename "$core")"
cp xstreemly_libretro.info README.md LICENSE "$out/"
cp deps/ffmpeg/COPYING.GPLv3 "$out/licenses/GPL-3.0.txt"
cp deps/ffmpeg/LICENSE.md "$out/licenses/ffmpeg-LICENSE.md"
case $platform in
   windows-*) ;;   # schannel, no mbedTLS
   *) cp deps/mbedtls/LICENSE "$out/licenses/mbedtls-LICENSE.txt" ;;
esac
cat > "$out/INSTALL.txt" <<TXT
X-Streemly $version ($platform) - network video core for RetroArch

1. Copy $(basename "$core") into RetroArch's "cores" folder
   and xstreemly_libretro.info into its "info" folder.
   (Android: Load Core -> Install or Restore a Core, pick $(basename "$core").)
2. Load Core -> X-Streemly, then Start Core: the channels of an HDHomeRun
   tuner on your network, or iptv-org's free channels for the country set in
   the core options. Or Load Content -> an .m3u channel list or a .strm file
   holding one stream URL.

X-Streemly hosts no streams. See README.md for the controls.

Licence: X-Streemly is GPL-2.0-or-later; this build includes FFmpeg
(configured --enable-gpl --enable-version3) and so is distributed under
GPL-3.0-or-later (licenses/). Source code, including the bundled libraries:
https://github.com/willho89/X-Streemly/releases/tag/v$version
TXT
(cd build/package && rm -f "$name.zip" && zip -qr "$name.zip" "$name")
echo "build/package/$name.zip"
