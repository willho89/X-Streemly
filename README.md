# X-Streemly

A [libretro](https://www.libretro.com/) core for **network video**: tune in to live TV streams the way you'd flip
channels on a TV. Built for [VRT](https://github.com/willho89/VRT) (an emulated CRT TV for Meta Quest), and usable in
RetroArch too.

> **Status: early development (M0 done).** The core builds, loads in RetroArch and connects to HLS/MPEG-TS streams
> over HTTP(S), but doesn't show pictures or play sound yet. Nothing to install yet.

## Planned
- **Free IPTV out of the box:** channel lists from [iptv-org](https://github.com/iptv-org/iptv), a community
  collection of publicly available TV streams, browsable by country, category and language. X-Streemly hosts no
  streams.
- **Your own sources:** M3U/M3U8 playlists and direct URLs (HLS, MPEG-TS, HTTP).
- **TV-style control:** channel up/down, last channel, an info banner with the channel name.
- **CRT-friendly output:** standard-definition pictures, and interlaced streams sent field by field to frontends that
  draw interlace like a real tube (VRT's field-output extension).
- **Later:** a companion app for Linux and Windows that casts a low-resolution virtual monitor (an app, a video
  player…) to X-Streemly over the network.

## License
GPL-2.0-or-later. See [LICENSE](LICENSE).
