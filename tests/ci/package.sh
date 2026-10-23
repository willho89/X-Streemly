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
case $platform in
   # Stripping breaks the linker's ad-hoc signature, and arm64 macOS won't
   # load an unsigned library.
   macos-*) ${CODESIGN:-codesign} --force --sign - "$out/$(basename "$core")" ;;
esac
cp xstreemly_libretro.info README.md LICENSE "$out/"
cp deps/ffmpeg/COPYING.GPLv3 "$out/licenses/GPL-3.0.txt"
cp deps/ffmpeg/LICENSE.md "$out/licenses/ffmpeg-LICENSE.md"
case $platform in
   windows-*) ;;   # schannel, no mbedTLS
   *) cp deps/mbedtls/LICENSE "$out/licenses/mbedtls-LICENSE.txt" ;;
esac
lib=$(basename "$core")
case $platform in
   android-*) step1="1. In RetroArch: Load Core -> Install or Restore a Core, and pick
   $lib (unzipped somewhere on the device). For the core's name
   and details in the menus, put xstreemly_libretro.info in RetroArch's \"info\"
   folder (Settings -> Directory -> Core Info) if your RetroArch lets you." ;;
   macos-*) step1="1. Copy $lib into RetroArch's \"cores\" folder
   (~/Library/Application Support/RetroArch/cores) and xstreemly_libretro.info
   into its \"info\" folder. The core isn't notarized: if RetroArch can't load
   it, clear the download quarantine first:
     xattr -d com.apple.quarantine $lib" ;;
   *) step1="1. Copy $lib into RetroArch's \"cores\" folder
   and xstreemly_libretro.info into its \"info\" folder." ;;
esac
case $version in
   dev-*) src="https://github.com/willho89/X-Streemly/tree/${version#dev-}" ;;
   *) src="https://github.com/willho89/X-Streemly/releases/tag/v$version" ;;
esac
cat > "$out/INSTALL.txt" <<TXT
X-Streemly $version ($platform) - network video core for RetroArch

$step1
2. Load Core -> X-Streemly, then Start Core: the channels of an HDHomeRun
   tuner on your network, or iptv-org's free channels for the country set in
   the core options. Or Load Content -> an .m3u channel list or a .strm file
   holding one stream URL.

X-Streemly hosts no streams. See README.md for the controls.

Licence: X-Streemly is GPL-2.0-or-later; this build includes FFmpeg
(configured --enable-gpl --enable-version3) and so is distributed under
GPL-3.0-or-later (licenses/). Source code, including the bundled libraries:
$src
TXT
(cd build/package && rm -f "$name.zip" && zip -qr "$name.zip" "$name")
echo "build/package/$name.zip"
