# X-Streemly: network-video libretro core — starting plan

**Status:** project created 2026-10-21. This file seeds the first session. Its first job is to turn this into a full
plan, with milestones, a status board and a decision log, the way DeeVee's `CLAUDE.md` is organised
(`~/.claude/projects/DeeVee/CLAUDE.md`), and then get the user's approval.

## What it is
X-Streemly is a libretro core that plays **network video streams** like TV channels. It's built first for **VRT**
(`~/.claude/projects/VRT`, an emulated CRT TV in VR/AR on Meta Quest 2/3) and should also run in RetroArch.

## Decisions so far (user, 2026-10-21)
- **Name:** X-Streemly. **Repo:** https://github.com/willho89/X-Streemly, public.
- **License:** GPL-2.0-or-later, matching DeeVee. That's compatible with VRT (GPLv3), RetroArch, FFmpeg and the
  libretro buildbot.
- **First source:** free IPTV from **iptv-org** (https://github.com/iptv-org/iptv), a community collection of
  publicly available streams; X-Streemly hosts nothing. Its playlists are at `https://iptv-org.github.io/iptv/index.m3u`
  (about 2.5 MB, thousands of channels), with per-country, per-category and per-language lists. Its JSON API is at
  `https://iptv-org.github.io/api/` (`channels.json`, `streams.json`, guides/EPG). Expect geo-blocked and dead
  streams.
- **Far later, its own project:** a Linux/Windows companion app that creates a low-resolution, low-latency virtual
  monitor and streams it to X-Streemly, i.e. cast a screen, an app or a video player to the CRT. Don't design for it
  in detail yet, but keep the input side open to a custom low-latency source.

## Lessons from VRT and DeeVee to carry over
- **Check existing work first** (user rule). Prior art to study before building:
  - RetroArch's FFmpeg core (`libretro-ffmpeg`): file playback only, threaded FIFO.
  - `libretro-mpv`: plays URLs/HLS, but it's GL-only. That's a poor fit for VRT, where a GLES core gave a black
    screen and dragged frame rates on Quest. **Prefer software video output.**
  - DeeVee's A/V clock, pacing and field output, which are the closest model.
- **VRT integration:**
  - VRT downloads third-party cores from the libretro buildbot and bundles only GPL-compatible ones it builds itself
    (like DeeVee).
  - Its private libretro extension for field-accurate interlace is `VRT/include/vrt_libretro_ext.h`; DeeVee
    implements it (`src/vrt_libretro_ext.h`).
  - Its audio pacing holds a ~50 ms queue and works best when a core emits audio steadily (a frame's worth per
    `retro_run`).
  - It has a TV-style on-screen guide and OSD of its own. Agree with VRT on how channel lists and names surface
    (core options, `SET_MESSAGE`, or a small extension).
- **Network on Android:** HTTPS/HLS needs TLS in the FFmpeg build (mbedTLS or OpenSSL statically linked; Android
  has no system OpenSSL for NDK apps). Cores run on the emulation thread, so connection and buffering must never
  block `retro_run`. Read on a separate thread with a bounded buffer and keep presenting the last picture or a
  "tuning" card while it fills.
- **Build:** same setup as DeeVee: C99, Makefile with `platform=` (Linux x86_64 first, then `android-arm64`). Tools:
  the `deevee-build` distrobox, NDK r27c at `~/android-ndk/android-ndk-r27c`, host tools at `~/.local/deevee/bin`.
  Pin FFmpeg to a release (DeeVee uses n8.1.3) and build only the needed demuxers, decoders and protocols.
- **Testing:** a headless harness like DeeVee's `tests/harness.c` from M0. Use a local HLS fixture made with ffmpeg
  (`-f hls`) served by `python3 -m http.server`, so tests don't depend on live streams.

## Rough milestones (refine in session 1)
1. **M0:** skeleton, FFmpeg deps with TLS, Linux + Android builds, the harness, one local HLS fixture.
2. **M1:** play one URL. Network reader thread, demux/decode, a frame queue, an A/V clock, audio resampled to 48 kHz,
   with standard-definition output.
3. **M2:** playlists. Load M3U/M3U8, then channel up/down and last channel, with the channel name and number on
   switching.
4. **M3:** iptv-org. Fetch and cache its lists and API data, filter by country/category/language, skip dead or
   geo-blocked streams, and choose a default set.
5. **M4:** VRT integration. Field output for interlaced streams, the channel list in VRT's guide, and RF/composite
   defaults.
6. **M5:** robustness. Reconnects, stalls, format changes mid-stream, long runs with A/V sync.
7. **M6:** distribution. Release builds, and possibly the libretro buildbot.

## Open questions for session 1
- Content model: does the core take an `.m3u` file as content, a small `.xstreemly` config, or run with no content
  and fetch iptv-org itself?
- Which iptv-org lists by default (country? categories?), and whether to filter out NSFW categories (iptv-org tags
  them).
- How much of the channel UI lives in the core (simple OSD) versus in VRT's guide.
