# X-Streemly

A [libretro](https://www.libretro.com/) core for **network video**: tune in to live TV streams the way you'd flip
channels on a TV. Developed and tested in RetroArch; support for [VRT](https://github.com/willho89/VRT) (an
emulated CRT TV for Meta Quest) comes later.

> **Status: first pre-release, [v0.1.0](https://github.com/willho89/X-Streemly/releases/tag/v0.1.0).** Start the
> core in RetroArch with no content and it shows the channels of an HDHomeRun tuner on your network, or iptv-org's
> free channels for your country; or load your own M3U list. Builds for Linux (x86_64, ARM), Windows, macOS and
> Android; developed and tested on Linux, the others tested in CI only so far.

## What works
- **Live streams over HTTP(S):** HLS (MPEG-TS segments) and plain MPEG-TS, plus files over HTTP.
- **Full resolution:** the best quality an HLS stream offers, at its native size (tested up to 1080p); a
  *Max Resolution* option caps it.
- **Codecs:** H.264, HEVC, MPEG-2 video; AAC, AC-3, E-AC-3, MP2 audio, folded down to stereo.
- **Interlaced broadcasts** deinterlaced at field rate (bwdif or yadif), or shown woven.
- **Smooth on any display:** frames are shown by their timestamps at the display's rate; a small, inaudible rate
  correction keeps the buffer steady whatever the broadcaster's clock does.
- **Rides out a bad connection:** holds the picture through a stall, re-buffers, reconnects without losing what's
  buffered, learns a bigger buffer for streams that need one, and drops to a lower HLS quality if the connection
  can't keep up. Changes mid-stream (resolution, 5.1 ↔ stereo, ad breaks) and timestamp wraps play straight
  through.
- **Audio tracks:** second languages and audio description; the *Audio Language* option picks one to start with,
  L2 switches instantly while watching. Optional *Volume Levelling* evens out loudness between channels.
- **TV out of the box:** with no content, the channels of an **HDHomeRun** network tuner if one is on the LAN (found
  automatically; ATSC channels numbered 3.1, 3.2 …), else [iptv-org](https://github.com/iptv-org/iptv)'s free
  public channels (a community collection; X-Streemly hosts no streams), filtered by country,
  category and language, cached for offline use. Channels that don't answer are skipped for a day.
- **Channel lists:** M3U/M3U8 playlists, local or at a URL, numbered in order or by `tvg-chno`; encrypted (DRM)
  channels are recognised and skipped.
- **Favourites:** X marks one, Y jumps to the next.
- **TV-style control:** channel up/down (the tune waits until you stop flicking), last channel, previous/next group,
  number entry on the keyboard, a channel list, an info band; the last channel is remembered.
- **On-screen display drawn into the picture:** channel number and name, tuning / no signal / end of stream cards.

## Trying it
Download the zip for your system from the [releases](https://github.com/willho89/X-Streemly/releases) and follow
its `INSTALL.txt` (copy the core into RetroArch's `cores` folder and the `.info` file into `info`).

In RetroArch, load the core and choose **Start Core**: you get your HDHomeRun's channels, or iptv-org's for the
country in the core options (*Channels → Country*, default United Kingdom). Or load an `.m3u` channel list. For a
single stream, or a channel list on the web, write its URL into a text file ending in `.strm`:

```
https://example.com/live/channel/playlist.m3u8
```

| RetroPad | |
|---|---|
| Up / Down | Channel up / down |
| B | Last channel |
| L / R | Previous / next group |
| A | Info |
| Select | Channel list (Up/Down, A to tune, B to close) |
| X / Y | Favourite on/off / next favourite |
| L2 | Next audio track |
| Keyboard 0–9 (and . for 3.1) | Channel number |

## Planned
- **VRT:** standard-definition output and interlaced streams sent field by field to VRT's virtual CRT.
- **Later:** a companion app for Linux and Windows that casts a low-resolution virtual monitor (an app, a video
  player…) to X-Streemly over the network.

## Building
`make` builds FFmpeg and mbedTLS from the submodules first, then the core; `make fixtures && make check` runs the
tests. Other targets: `make platform=win` (MinGW-w64 cross build; tests under Wine), `platform=osx` (on a Mac),
`platform=linux-arm64` / `linux-armhf` (cross), `platform=android-arm64` / `android-armv7` (Android NDK). CI builds
all of them on every push. See [CLAUDE.md](CLAUDE.md) §6.

## License
GPL-2.0-or-later. See [LICENSE](LICENSE). Builds include FFmpeg with mbedTLS (configured `--enable-version3`), so
binaries are distributed under GPL-3.0-or-later.
