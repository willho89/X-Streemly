# X-Streemly

A [libretro](https://www.libretro.com/) core for **network video**: tune in to live TV streams the way you'd flip
channels on a TV. Developed and tested in RetroArch; support for [VRT](https://github.com/willho89/VRT) (an
emulated CRT TV for Meta Quest) comes later.

> **Status: early development (M1 done).** Plays one stream: point a `.strm` file at an HLS or MPEG-TS URL and load
> it in RetroArch. Channel lists, channel changing and iptv-org come next. No release yet.

## What works
- **Live streams over HTTP(S):** HLS (MPEG-TS segments) and plain MPEG-TS, plus files over HTTP.
- **Full resolution:** the best quality an HLS stream offers, at its native size (tested up to 1080p); a
  *Max Resolution* option caps it.
- **Codecs:** H.264, HEVC, MPEG-2 video; AAC, AC-3, E-AC-3, MP2 audio, folded down to stereo.
- **Interlaced broadcasts** deinterlaced at field rate (bwdif or yadif), or shown woven.
- **Smooth on any display:** frames are shown by their timestamps at the display's rate; a small, inaudible rate
  correction keeps the buffer steady whatever the broadcaster's clock does.
- **Rides out a bad connection:** holds the picture through a stall, re-buffers, reconnects without losing what's
  buffered, and learns a bigger buffer for streams that need one.
- **On-screen cards** (tuning, no signal, end of stream), drawn into the picture.

## Trying it
Write the stream's URL into a text file ending in `.strm`, then load that file with the core in RetroArch:

```
https://example.com/live/channel/playlist.m3u8
```

## Planned
- **Free IPTV out of the box:** channel lists from [iptv-org](https://github.com/iptv-org/iptv), a community
  collection of publicly available TV streams, browsable by country, category and language. X-Streemly hosts no
  streams.
- **Your own playlists:** M3U/M3U8 channel lists.
- **TV-style control:** channel up/down, last channel, number entry, an info banner with the channel name.
- **VRT:** standard-definition output and interlaced streams sent field by field to VRT's virtual CRT.
- **Later:** a companion app for Linux and Windows that casts a low-resolution virtual monitor (an app, a video
  player…) to X-Streemly over the network.

## Building
Linux x86_64 for now: `make` (builds FFmpeg and mbedTLS from the submodules first), `make fixtures && make check`
for the tests. See [CLAUDE.md](CLAUDE.md) §6.

## License
GPL-2.0-or-later. See [LICENSE](LICENSE). Builds include FFmpeg with mbedTLS (configured `--enable-version3`), so
binaries are distributed under GPL-3.0-or-later.
