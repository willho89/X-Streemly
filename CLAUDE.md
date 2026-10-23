# X-Streemly — libretro network-video core: Master Development Plan

**Status:** v1.0 (2026-10-21), approved by the user: the authoritative guide for all development sessions.
**Goal:** a libretro core that plays **network video streams like TV channels**: channel up/down, last channel, a
channel banner, picture and sound at the stream's **full resolution**, starting without stalling the frontend.
**X-Streemly is a libretro core first** and is developed and tested in RetroArch. VRT (emulated CRT TV in VR/AR on
Meta Quest, `~/.claude/projects/VRT`) is a later, separate phase (§7 "After v1"), outside the current scope.

---

## 0. How to use this document (session protocol)

Every development session should:

1. Read this file first, then the **Status Board** (§12) and **Decision Log** (§13).
2. Pick the next open milestone task; don't skip ahead of a milestone's acceptance criteria.
3. Before writing a subsystem, open the **reference source** listed for it (§4) and port/adapt rather than invent.
4. End the session by updating the Status Board, adding any decisions to the Decision Log, and noting open questions.

Guiding rule: **check existing work first, don't reinvent the wheel.** FFmpeg already does network I/O, HLS, demuxing,
decoding, scaling and resampling; ffplay already shows how to thread a live player. X-Streemly's own code is the
*glue*: libretro plumbing, a tuner thread that never blocks `retro_run`, an A/V clock that copes with a live source
running at its own rate, playlists, and the TV-style UI.

---

## 1. Scope

### In scope (v1)
- **Sources:** HTTP(S) live streams: HLS (`.m3u8`, MPEG-TS and fMP4 segments, AES-128), plain MPEG-TS over HTTP,
  progressive HTTP files (MP4/TS) as "VOD channels". Local files for tests.
- **Playlists:** M3U/M3U8 (`#EXTINF` with `tvg-id`, `tvg-logo`, `group-title`, `tvg-chno`; `#EXTVLCOPT:http-referrer`
  / `http-user-agent`), single-URL `.strm` files (the Kodi convention: a text file holding one URL).
- **iptv-org:** fetch, cache and filter its public lists (country / category / language), drop NSFW, mark or skip
  geo-blocked and dead streams.
- **Video:** H.264 (the bulk of IPTV), HEVC, MPEG-2; output at the stream's **native resolution** (up to 1080p as
  standard, 4K allowed), best HLS variant by default (§5.3, §5.4); interlaced sources deinterlaced.
- **Audio:** AAC (LC/HE), MP2/MP3, AC-3/E-AC-3 → 48 kHz stereo.
- **TV control:** channel up/down, last channel, direct number entry (keyboard), info banner, in-picture OSD
  (channel number + name, "tuning" card with optional static and hiss).
- **Platforms:** Linux x86_64 first, developed and tested in the user's Steam RetroArch. Windows, macOS, Linux ARM,
  Android at M5 (DeeVee's CI recipe).

### Out of scope (v1, revisit later)
- **VRT integration** (field output, SD/CRT tuning, Quest performance, VRT's guide, bundling): a separate phase after
  v1 (§7 "After v1"). The core keeps the doors open (options that cap resolution, a field-output path is easy to add)
  but nothing is built or tested for VRT now.
- EPG / programme guide data (iptv-org's `guides.json` only lists grabber sources; no hosted EPG).
- Time-shift, pause and rewind of live TV; seeking in VOD beyond the basics.
- DRM'd streams (Widevine etc.): impossible in a GPL core, never a goal.
- DASH, RTMP, RTSP, SRT, UDP multicast: possible later via FFmpeg protocols; the companion app (below) may want
  one of them.
- Hardware decoding and a GL output path (§9 lists when they'd be needed).
- **Companion app** (Linux/Windows virtual low-res monitor streamed to X-Streemly): its own project, far later.
  The only requirement now: the source layer (§3) must accept a custom low-latency source without reworking the
  clock or output.
- Hosting or curating streams. X-Streemly hosts nothing.

---

## 2. Licensing

| Component | License | Notes |
|---|---|---|
| FFmpeg (libavformat/avcodec/swscale/swresample/avfilter) | LGPL-2.1+, GPL with `--enable-gpl` | Pin a release (§11); build only what we need. |
| mbedTLS 3.6 LTS | Apache-2.0 OR GPL-2.0-or-later | TLS for Linux/Android/(macOS). In FFmpeg's `EXTERNAL_LIBRARY_VERSION3_LIST` (checked in n8.1.3 `configure`), so it needs `--enable-version3`. |
| Windows TLS: schannel | system | No extra dependency, no version3 needed. |
| libretro-common | MIT | rthreads, file_stream, rjson. |
| ffplay.c / libretro-ffmpeg / DeeVee code we port | LGPL/GPL-2.0+ | Keep upstream copyright headers plus "Adapted for X-Streemly". |

**X-Streemly source: GPL-2.0-or-later.** **Binaries that include mbedTLS are effectively GPL-3.0-or-later**, because
FFmpeg is built with `--enable-version3`. That's fine for RetroArch (GPLv3), the buildbot and VRT (GPLv3). The
README's licence section should say so once builds exist (D-004).

---

## 3. Architecture overview

```
 Frontend (RetroArch)
   │ retro_run / video_refresh / audio_batch / input_state / environment
   ▼
┌───────────────────────────── X-Streemly core ─────────────────────────────┐
│ libretro.c     entry points, av_info, core options, environment, OSD msgs │
│ xs_input.c     RetroPad/keyboard → channel commands                       │
│ xs_chan.c      channel list, numbering, current/last channel, persistence │
│ xs_playlist.c  M3U/M3U8/.strm parser (attrs, VLC opts) ◄─ VLC m3u.c        │
│ xs_iptv.c      iptv-org fetch + disk cache + filters (thread)             │
│ xs_osd.c       5x7 bitmap font, tuning card (banner/static: M2)           │
│ xs_present.c   per tick: clock, buffer floor controller + catch-up, pick  │
│                frame, emit audio (never blocks, no copy)                  │
│- - - - - - - - - - - - - - - thread boundary - - - - - - - - - - - - - - -│
│ xs_tuner.c     TUNER THREAD: open (interrupt cb, timeouts), variant pick, │
│                timeline join, audio decode, reconnect ◄─ ffplay read_thread│
│ xs_pktq.c      bounded video packet queue with serials ◄─ ffplay PacketQ  │
│ xs_video.c     VIDEO THREAD: decode (frame threads) → bwdif → swscale to  │
│                XRGB8888 at native size → frame slots (zero copy)          │
│ xs_audio.c     decoder + swresample 48 kHz stereo (rate comp) → PCM ring  │
│ xs_source.c    content → source (URL, headers), open with FFmpeg options  │
│ xs_fetch.c     small HTTP(S) GETs via avio (playlists, iptv-org; M2/M3)   │
└───────────────────────────────────────────────────────────────────────────┘
   │
 FFmpeg (avformat: hls/mpegts/mov, protocols http/https/tls/crypto) + mbedTLS
```

### Key architectural decisions (rationale in §13)
- **Threaded, unlike DeeVee (D-005 there).** Network opens take seconds and reads stall unpredictably, so network
  and demux run on a **tuner thread**, video decode + conversion on a **video thread** (audio decode is cheap and can
  stay on the tuner thread). `retro_run` only takes ready frames and audio from bounded rings (D-001).
- **Full resolution, software output** (D-005, D-002): frames are converted to XRGB8888 at the stream's native size
  on the video thread (1080p ≈ 2–4 ms of swscale per frame on a desktop CPU, off the emulation thread). `retro_run`
  hands the ring slot's pointer straight to `video_cb` (no 8 MB copy per 1080p frame); the slot stays reserved until
  the next `retro_run`.
- **FFmpeg for everything media**, including HTTP fetches of playlists and iptv-org data (one TLS stack, no libcurl).
- **Live source rate ≠ display rate:** the core declares the frontend's display rate and presents frames by PTS
  (repeating/dropping as needed, like DeeVee's pulldown). The buffer depth is held near a target by tiny audio
  resampling (`swr_set_compensation`), ffplay's external-clock idea (§5.5, D-008).
- **Best HLS variant by default**, capped only by a *Max resolution* option (§5.3).
- **Source abstraction:** the tuner opens "a source" (URL + headers + options) through one function; a later
  low-latency source (companion app) plugs in there with a different buffer target, nothing else changes.

---

## 4. Reference source map (read before writing each subsystem)

| X-Streemly module | Reference | What to take |
|---|---|---|
| xs_tuner.c | FFmpeg `fftools/ffplay.c` `read_thread()`, `decode_interrupt_cb`, `stream_component_open` | Threaded open/read loop, interrupt callback for abort + timeouts, packet queues, EOF handling, `infinite_buffer`/realtime detection (`is_realtime()`). |
| xs_present.c | ffplay `sync_clock_to_slave`, `check_external_clock_speed`, `synchronize_audio`, `compute_target_delay` | Speed nudging from queue depth for realtime sources; audio sample-count compensation; frame drop/repeat. |
| xs_present.c / xs_audio.c | DeeVee `src/dv_clock.c`, `dv_audio.c` (PTS-chunk queue, per-tick render, segment fades), D-013 | libretro tick-driven clock, exact samples per tick, discontinuity handling, fades. |
| xs_video.c | DeeVee `dv_video.c` / `dv_render.c`, D-021 (bwdif at field rate); ffplay `video_thread`, `get_video_frame` (early frame drop) | swscale setup, BT.601/709 matrices by stream colourspace, deinterlace, dropping late frames before conversion. |
| libretro.c | DeeVee `src/libretro.c`; RetroArch `cores/libretro-ffmpeg/ffmpeg_core.c` | Options v2, av_info/geometry changes, logging. libretro-ffmpeg: what it does about fps vs display rate and big frames; what *not* to do (file-only FIFO model). |
| xs_playlist.c | VLC `modules/demux/playlist/m3u.c`; Kodi PVR IPTV Simple Client (`pvr.iptvsimple`) M3U parser | Attribute parsing, `#EXTVLCOPT`, `#KODIPROP`, encodings, relative URLs. |
| xs_tuner.c (HLS) | FFmpeg `libavformat/hls.c` options (`live_start_index`, `http_persistent`, `http_multiple`, `seg_max_retry`), `http.c` (`reconnect*`, `rw_timeout`, `referer`, `user_agent`, `headers`) | Correct options for live HLS; known weak spots (§9). |
| (prior art) | `libretro-mpv` | Confirms URL playback in a core is viable; GL-only, so not a model for output. |
| xs_osd.c | DeeVee OSD (SET_MESSAGE_EXT) | Messages; the in-picture OSD itself is new code. |
| xs_iptv.c | iptv-org `api` README (`streams.json`: `channel, feed, title, url, referrer, user_agent, quality, labels`; `channels.json`: `is_nsfw`, `categories`, `country`, `closed`; `blocklist.json`), `iptv` README (playlist URLs) | Data model and filters. |

Prior-art search (2026-10-21): no existing libretro IPTV/live-stream core found; libretro-mpv is the only core that
opens URLs.

---

## 5. Subsystem specifications

### 5.1 Content model and loading (xs_chan.c, libretro.c) — D-006
- `valid_extensions = "m3u|m3u8|strm"`, `need_fullpath = true`, plus `SET_SUPPORT_NO_GAME = true`.
- **Playlist as content** (`.m3u`/`.m3u8`): its entries are the channels, numbered in order (or by `tvg-chno`).
  A playlist whose first line is `#EXTM3U` and whose entries are media URLs is a channel list; an HLS *media*
  playlist (`#EXT-X-TARGETDURATION`) given as content is played as a single channel.
- **`.strm`**: one URL, one channel.
- **No content** (RetroArch "Start Core"): the core runs the **iptv-org lineup** chosen in core options (§5.8).
- **`.xstreemly`** reserved: a small INI-style file combining several playlists/iptv-org filters into one lineup.
  Only if needed.
- Persist per lineup: last channel and the previous channel, in `<save_dir>/X-Streemly/<lineup-hash>.state`
  (plain text). Load starts on the last-watched channel.

### 5.2 Tuner thread (xs_tuner.c)
- One tuner thread; commands from the emulation thread via a small lock-protected mailbox: `TUNE(source)`,
  `STOP`. Status back via atomics: `IDLE | CONNECTING | BUFFERING | PLAYING | STALLED | FAILED(reason)`.
- Open with `avformat_open_input` and an **interrupt callback** that returns 1 when a newer command is pending or a
  deadline passed. That makes channel changes instant even mid-connect (ffplay pattern).
- Channel change: abort the current source (interrupt), flush rings, open the new one. Never `join` a thread from
  `retro_run`.
- Options per open: `user_agent`, `referer`/`headers` from the playlist or iptv-org, `rw_timeout` (e.g. 10 s),
  `reconnect=1`, `reconnect_streamed=1`, `reconnect_on_network_error=1`, `reconnect_delay_max`, HLS
  `http_persistent=1`, `live_start_index=-3` (start 3 segments from the live edge).
- `avformat_find_stream_info` is slow on live HLS: cap `probesize`/`analyzeduration` (e.g. 1 MB / 2 s).
- Failure handling: timeouts, 403/404 (likely geo-blocked/dead), no decodable video. Report a reason string for
  the OSD and for the M3 dead-stream list.
- **Retries (M1, D-018):** a failed open is retried after 2, 4, 8 … 30 s. A connection lost *after* media flowed is
  retried at once and **keeps the serial**: nothing buffered is dropped, and the new connection's timestamps are
  joined right after the furthest point reached, so playback carries on through a reconnect.
- **Live or file (D-021):** live = no duration, unless the input (not hls) has a known size (`Content-Length`):
  FFmpeg gives MPEG-TS over HTTP no duration. Files end ("END OF STREAM"); live streams reconnect.
- **Timeline (M1, D-023):** a packet more than 1 s *behind*, or more than 10 s *ahead of*, where its stream should
  continue (HLS discontinuity, ad splice, wrap, encoder restart) is joined on; a smaller forward jump is a real gap
  (segments that expired unread) and is kept, so the clock steps over it. Joins go through **epochs**: the first
  stream to cross opens an epoch with the offset that makes it continue, and each other stream adopts that epoch's
  offset when it crosses (if it fits). Interleaving means one stream's new timestamps arrive while another's old
  ones still do: a single shared offset flipped back and forth (seen live: ±5.9 s on a flaky 1080i restream).

### 5.3 Stream and variant selection
- HLS master playlists show up as several AVPrograms. Default: the **highest-resolution variant** (ties: highest
  bandwidth) not above the *Max resolution* option (`Native` default / 1080p / 720p / 576p / 480p). Set
  `discard = AVDISCARD_ALL` on every other program's streams, so FFmpeg's hls demuxer downloads only the chosen one.
  Verify with a 2-variant fixture in M1.
- No adaptive switching in v1 (FFmpeg's hls demuxer has none). If the chosen variant can't keep the buffer filled
  (two underruns within 60 s on a live stream), the tuner reconnects with the next variant down, keeping the serial
  and what is buffered (D-033).
- Audio: every decodable audio stream of the chosen program (up to 4) is decoded, each into its own ring; the
  *Audio Language* option picks the one to start with (else the stream's default, avoiding audio description), and
  L2 switches the ring the presenter reads, at the same timestamp: instant (D-035). The rings not played are trimmed
  to the clock every tick.

### 5.4 Video (xs_video.c) — D-005
- Decoders: `h264`, `hevc`, `mpeg2video`, `mpeg1video`, frame + slice threading with **half the CPUs**; swscale and the
  deinterlacer get 2 threads each (D-017: auto everywhere was ~25 threads on 8 CPUs and stalled `retro_run` for
  5–10 ms when a lock holder was preempted).
- **Output at native resolution**, XRGB8888 via swscale with the stream's colour matrix (BT.709 for HD, BT.601 for
  SD) and range. `geometry.base_width/height` = the stream's coded display size; `aspect_ratio` from the DAR
  (SAR × size), never assumed square.
- **Geometry changes:** `max_width/height` declared at load as 1920×1080. A channel within that changes size with
  `SET_GEOMETRY` (cheap). A larger stream (4K) raises the maximum with `SET_SYSTEM_AV_INFO` once (RetroArch
  reinitialises its video driver for that). Verify both in the user's RetroArch in M1/M2.
- **Interlaced sources** (some broadcast restreams are 1080i/576i H.264 or MPEG-2; most IPTV is progressive): bwdif
  at field rate by default (DeeVee D-021), option off/bwdif/yadif. With *Off*, swscale converts the woven frame field
  by field (destination flagged interlaced; the new swscale API refuses interlaced → progressive otherwise).
- Scan type comes from the decoded frames (`AV_FRAME_FLAG_INTERLACED`), not `codecpar->field_order` (unknown in HLS).
- Late frames are dropped before conversion (ffplay's early drop) so a slow conversion never builds latency.
- Ring: a few ready frames (4 × 1080p XRGB ≈ 33 MB; 4K ≈ 133 MB, so the ring is sized in bytes, not frames). The
  video thread blocks (on an interruptible condvar) when full.
- A *Max resolution* cap (§5.3) also downscales non-HLS streams, which is the hook for VRT's SD use later.

### 5.5 Clock, output rate, buffering and A/V sync (xs_present.c, xs_audio.c) — D-007, D-008, D-019, D-020
- **Output rate:** the core declares `timing.fps` = the display rate (`RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE`,
  falling back to 60) and presents each tick the newest frame with PTS ≤ clock, so 25/30/50/60 fps streams all play
  at real speed on a 60 Hz display (with the unavoidable 50→60 cadence judder). Option *Output rate*: Auto / 60 /
  59.94 / 50 / Stream rate. Check in M1 against how libretro-ffmpeg declares its fps and how the user's RetroArch
  (vsync + audio sync on, 60 Hz) behaves.
- The frontend calls `retro_run` at the declared rate; the core advances a presentation clock by one tick and emits
  exactly the tick's audio (fractional accumulator, 48000/fps).
- **Start:** after a tune, present the tuning card until the audio ring holds the *start target* (default 1.0 s for
  network streams), then anchor `clock = first audio PTS` and start.
- **The clock is the audio (D-020):** decoded audio waits in a ring of PTS-stamped chunks; the clock is the timestamp
  of the next sample to play, and each tick shows the newest frame due by the middle of the tick. Without audio
  the clock advances by the tick from the first frame.
- **Steady state:** the live source produces media at the broadcaster's rate, the frontend consumes at its own. Live
  HLS arrives in segment bursts, so the level is a saw-tooth: the controller steers its **floor** (minimum over the
  last 20 s) to the floor target with a proportional rate change of at most ±0.5 % (0.2 % per second of error),
  applied by `swr_set_compensation` in the producer; the clock follows automatically because chunks carry their
  source duration. Files are not steered (they can't drift).
- **Adaptive jitter buffer (D-022):** floor target = *Buffer* option + 1 s per underrun on this tune (max +10 s) + half
  the burst size (the level's peak-to-trough over the window, ~one segment plus FFmpeg's reload lateness). The
  start/re-buffer threshold is the option + underrun extra. Burst size only grows within a tune, except it is
  relearned after a catch-up (the window held the backlog). CNBC UK (6 s segments): burst ~8 s, target ~5 s,
  floor 2.3–3.2 s above empty, 0 underruns in 5 min; with the fixed 1 s target it underran on every segment.
- **Catch-up (D-019):** live HLS starts `live_start_index=-3` segments back (CNBC UK: 18 s in 6 s segments, fetched
  over the first seconds), far more than ±0.5 % can work off. When a full 20 s window's floor is > 3 s over the floor
  target, skip the oldest audio down to target + 0.5 s (fade out/in; video frames before the new clock are dropped
  unconverted), then wait for a fresh window. Judged on the floor, never the instantaneous level, so a skip can't
  cut into the wait for the next segment. It also absorbs a frontend that runs slow (below the declared rate the
  buffer grows; seen when the Deck was decoding three 1080p streams at once).
- Stats line every minute (INFO): state, measured ticks/s, buffer / floor / target, rate correction, frames shown /
  dropped, underruns, convert and decode times.
- **Underrun (stall):** freeze the last picture, fade audio out (64-sample fade like DeeVee D-013), after ~1 s show
  the NO SIGNAL / BUFFERING card, re-buffer to the start target, fade back in.
- **Discontinuities:** joined on the tuner's timeline (§5.2), so the presenter sees one continuous clock.
- Fast-forward (RetroArch throttle state): not meaningful for live; drop video, keep the clock tied to the buffer.
- **Low-latency mode** (companion app, later): same controller with a ~100 ms target instead of ~1 s.

### 5.6 Audio (xs_audio.c)
- Decoders: `aac`, `aac_latm`, `mp2`, `mp3`, `ac3`, `eac3`. AC-3/E-AC-3 downmix in the decoder (DeeVee D-014);
  swresample for the rest with `rematrix_maxval=1`. Output s16 stereo 48 kHz.
- Ring holds PTS-stamped chunks (DeeVee's model). A timestamp jump inside a read (reconnect join, gap) is reported
  to the presenter, which fades out to it and in after it over 64 samples (D-034).
- **Volume Levelling** option (off by default, D-036): a slow gated AGC (`xs_agc_*`) to about −20 dBFS RMS, ±12 dB,
  falling in 0.5 s and rising in 4 s, soft limit above −1 dBFS; reset on every tune.
- No audio stream: emit silence at the tick rate, drive the clock from video PTS.

### 5.7 Playlists (xs_playlist.c)
- Parse `#EXTM3U` header attributes (`url-tvg` ignored in v1), `#EXTINF:<dur> key="value" ...,<title>`,
  `#EXTVLCOPT:http-referrer=`, `http-user-agent=`, `#EXTGRP`, `#KODIPROP` (DRM ones mark the channel unplayable),
  relative URLs (relative to the playlist), UTF-8 with or without BOM, CRLF.
- iptv-org titles carry suffixes like ` (720p)`, ` [Geo-blocked]`, ` [Not 24/7]`: strip them for display, keep them
  as flags.
- Remote playlists (URL in a `.strm` pointing to an `.m3u`) are fetched by xs_fetch off the emulation thread, never
  in `retro_load_game`: **M3** (it needs the fetch thread iptv-org uses; M2 reads local playlists only).
- Done in M2 (`src/xs_playlist.c`, adapted from VLC m3u.c): quote-aware `#EXTINF` attributes (the first unquoted
  comma starts the title), `tvg-name` when the title is empty, `tvg-chno`/`channel-number`, `group-title` or a
  carried-over `#EXTGRP`, Latin-1 lists widened to UTF-8, an HLS playlist given as content = one channel.
- Numbering (`src/xs_chan.c`): the playlist's `tvg-chno` when every channel has its own, else 1.. in order. Last and
  previous channel remembered per lineup in `<save_dir>/X-Streemly/<hash of the content path>.state`, written once
  the channel has settled for 5 s and at unload (not on every zap: file I/O on the emulation thread).

### 5.8 iptv-org (xs_iptv.c) — D-010, D-011
- Data used (sizes measured 2026-10-21): per-country playlists `https://iptv-org.github.io/iptv/countries/<cc>.m3u`
  (UK = `uk`, not `gb`; UK 70 KB, US 325 KB), per-category `.../categories/<id>.m3u`, per-language
  `.../languages/<code>.m3u`; `index.m3u` 2.6 MB. API: `streams.json` 3.8 MB, `channels.json` 8.0 MB,
  `feeds.json` 8.1 MB, `categories.json` 3 KB, `blocklist.json` 89 KB.
- Build the lineup from the **filtered M3U playlists** (small; already carry name, logo, group, the
  Geo-blocked/Not-24/7 flags and the referrer/user-agent options), merged and de-duplicated by `tvg-id`.
  `categories.json` supplies the category list; `blocklist.json` + the `xxx` category drop NSFW. Use the big JSON
  files only if a filter needs them.
- Cache under `<save_dir>/X-Streemly/iptv-org/` with the fetch date; refresh in the background when older than
  N days; work offline from cache.
- **Dead-stream handling:** a stream that fails to tune (timeout, 4xx, no video) is remembered with a timestamp and
  skipped by channel up/down for a day; it stays reachable by number.
- Default lineup (user, 2026-10-21): the configured country (default **UK**), all categories, geo-blocked hidden,
  NSFW excluded always, no option to enable it.
- Core options: Country (default UK), Category (All / News / Movies / Music / Kids / …), Language, Hide geo-blocked
  (default on), Hide not-24/7, Refresh lists now.
- **Done in M3** (`src/xs_iptv.c`, `src/xs_fetch.c`): a lineup thread (`xs-lineup`) downloads through avio, caches in
  `<save_dir>/X-Streemly/iptv-org/` (re-downloaded when older than *Refresh Channel Lists*: daily / weekly / every
  start; offline the cache is used however old), parses, filters and de-duplicates by URL off the emulation thread.
  Country → `countries/<cc>.m3u`; All countries + a language → `languages/<code>.m3u`; All + Any → `index.m3u`;
  country + language → the country's channels whose URL is also in the language list. Categories are matched in
  `group-title` (`Animation;Kids`). The option tables (126 countries by channel count, 40 languages, 29 categories;
  libretro allows 128 values per option) are generated by `tools/gen_iptv_tables.py` into `src/xs_iptv_tables.h`.
  UK on 2026-10-22: 295 entries, 54 geo-blocked hidden, 241 channels. Changing an iptv-org option rebuilds the
  lineup; the old one plays until the new one is ready. While loading, the idle card says LOADING CHANNELS (with the
  file being downloaded); a failure shows NO CHANNELS and the reason.
- **Dead channels** (`xs_chan`, `xs_tv`): a tune that fails before any picture (tuner FAILED with no packets) marks
  the channel dead for 24 h in `<save_dir>/X-Streemly/dead.txt`; a picture clears it. Up/Down skip dead channels
  (unless all are), the list shows "(NO SIGNAL)", number entry still tunes them. When the core chose the channel
  itself (start, new lineup), a dead one is skipped automatically, up to 20 times; any button press stops that.
- **Favourites:** X toggles (`favourites.txt`), Y jumps to the next; "*" in the banner and the list. Marks are
  keyed by tvg-id, else URL (iptv-org URLs change more often than ids), shared by all lineups.
- For tests, `XSTREEMLY_IPTV_BASE` replaces the iptv-org URL and `XSTREEMLY_CA_FILE` the CA bundle: `make check`
  serves a fake iptv-org (`tests/fixtures/iptv_*.m3u.in`).

### 5.9 Input (xs_input.c)
| Button | Action |
|---|---|
| D-pad Up / Down | Channel up / down, skipping DRM channels (auto-repeat after 400 ms, every 120 ms; the banner follows at once, the tune happens when no zap button is held and input has been idle 500 ms, so flicking through doesn't open every stream) |
| B | Last channel (recall) |
| A | Info band (number, name, group/flags, stream summary, state) for 6 s; A again hides it |
| L / R | Previous / next group (first channel of it) |
| X | Favourite toggle (M3, favourites group) |
| Select | Channel list (Up/Down move, L/R page, A tunes, B/Select close) |
| Keyboard 0–9 | Direct channel number entry: tunes at once when no longer number exists, else after 1.5 s or Enter; "NO CHANNEL n" otherwise |

Done in M2 (`src/xs_input.c`, `src/xs_tv.c`). Encrypted (DRM) channels picked by number or from the list show
"ENCRYPTED: CAN'T PLAY" and stay on the current channel.

### 5.10 OSD (xs_osd.c) — D-009
- The channel number and name are drawn **into the picture** by the core, like a real TV's on-screen display, and
  work in any frontend. Bitmap font scaled to the output height (so it reads the same at 480p and 1080p), drawn on
  the ring frame after conversion. Option to turn the OSD off.
- Elements: channel banner (big number top-right, a few seconds), info banner, "tuning…" card (black or static +
  optional hiss), "no signal"/error card with a short reason, number-entry digits, channel list overlay.
- `SET_MESSAGE_EXT` is sent as well when the in-picture OSD is off.
- **Done in M2 (D-024):** banner (number in 90s-TV green, 4 font pixels per unit, top right inside the overscan;
  name and group under it; only the number over a card, which already names the channel), notes, info band and
  channel list (opaque panels in a 90s menu blue), laid out in `xs_osd.c` from one `layout()` that also gives the
  covered rectangles. Drawn **in place** on the picture on screen: the areas under the banner text are saved and
  restored when it changes; the panels are rendered once per change into a buffer (allocated and touched at load)
  and pasted onto each new frame, and only saved on cards. Cost at 1080p: ~0.2 ms per tick with the banner,
  ~0.5 ms mean / 1.8 ms p99 with the list open (2–3 ms when it was copied whole or the panel darkened the picture).

### 5.11 Core options (SET_CORE_OPTIONS_V2)
Lineup source (playlist / iptv-org), iptv-org filters (§5.8), Max resolution, Output rate, Deinterlace,
Tuning effect (black / static / static + hiss), Show OSD, Buffer target, Verify TLS certificates, Loudness
levelling, Verbose logging.

As of M3: **Channels** (Country, Category, Language, Hide Geo-blocked Channels, Hide Part-time Channels, Refresh
Channel Lists), **Video** (Max Resolution, Deinterlace, Show OSD), **Advanced** (Buffer, Verbose Logging). Not yet:
Output rate (display rate is used, D-008), Tuning effect, Verify TLS for streams, Loudness levelling.

### 5.12 Save states
No save states (`retro_serialize_size` = 0); last channel persists via §5.1. Live TV has nothing to restore.

### 5.13 Network security — D-012
- `tls_verify`: FFmpeg's default is **no verification**. iptv-org metadata fetches verify against a bundled Mozilla
  CA list (embedded at build time); stream verification follows the *Verify TLS certificates* option, default off
  (public streams, many broken certificates).
- Playlist-supplied headers are limited to referrer and user-agent; no `file:` or `pipe:` URLs from remote
  playlists (`protocol_whitelist` = `http,https,tls,tcp,crypto` for network sources, `file,crypto` for local
  ones; done in M0, `src/xs_source.c`).

---

## 6. Build & dependencies

- Repo layout:
  ```
  X-Streemly/
    src/                 core sources (§3)
    deps/ffmpeg/         git submodule (pin, §11)
    deps/mbedtls/        git submodule (pin 3.6 LTS)
    libretro-common/     git submodule
    Makefile, Makefile.common, Makefile.deps, link.T
    xstreemly_libretro.info
    tests/               harness.c, fixtures (generated), server.py, check scripts
  ```
- Same setup as DeeVee: C99, `platform=` switch (`unix` now; `win`, `osx`, `linux-arm64`, `android-arm64` … at M5),
  distrobox `deevee-build`. Deps built statically (PIC) into the core; export only `retro_*` (link.T,
  `-fvisibility=hidden`); glibc floor 2.35 like DeeVee so the container-built `.so` runs in Steam RetroArch.
- FFmpeg configure (starting point, trim in M0/M1):
  `--enable-gpl --enable-version3 --enable-mbedtls --disable-everything --disable-programs --disable-doc
  --disable-autodetect --disable-avdevice --enable-avformat --enable-avcodec --enable-swscale --enable-swresample
  --enable-avfilter --enable-network --enable-pthreads --enable-protocol=http,https,tls,tcp,crypto,file,udp
  --enable-demuxer=hls,mpegts,mov,aac,ac3,eac3,mp3,flv --enable-decoder=h264,hevc,mpeg2video,mpeg1video,aac,aac_latm,mp2,mp3,ac3,eac3
  --enable-parser=h264,hevc,mpegvideo,aac,aac_latm,mpegaudio,ac3 --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc
  --enable-filter=bwdif,yadif,buffer,buffersink` (+ x86 asm via nasm, which matters for 1080p decode speed).
  Windows: `--enable-schannel` instead of mbedTLS.
- `xstreemly_libretro.info`: `display_name = "Network Video (X-Streemly)"`, `corename = "X-Streemly"`,
  `supported_extensions = "m3u|m3u8|strm"`, `supports_no_game = true`, `needs_fullpath = true`,
  `license = "GPLv2+"` (note GPLv3 binaries), `categories = "Media Player"`.

---

## 7. Milestones & acceptance criteria

All "in RetroArch" checks use the **user's Steam RetroArch** (§8), not VRT.

### M0 — Skeleton, dependencies, harness
- Repo layout, submodules (FFmpeg, mbedTLS, libretro-common), Makefile (`platform=unix`).
- Core loads, opens a URL on a thread with the interrupt callback, logs stream info (format, variants, codecs,
  resolution, frame rate, interlacing), shows a black/tuning frame and silence; clean unload mid-connect.
- `tests/harness.c` (DeeVee's, adapted): loads the core, real-time or fast pacing, `HARNESS_INPUT`, frame/audio
  dumps and hashes, max/p99 `retro_run` time. `tests/server.py`: local HTTP(S) server (self-signed cert for the TLS
  path) with fault injection (delay, stall, 404, drop connection).
- Fixtures generated by `make fixtures` with ffmpeg: HLS 2-variant (720p + 1080p) live-style playlist, 1080i and
  576i H.264 TS, 29.97 fps 16:9 H.264, 50 fps 720p, A/V-sync clip (flash + beep every second, as DeeVee's `avs_*`).
- **Done when:** `make check` passes on Linux (harness opens the local HTTPS HLS fixture and logs its streams), and
  the core loads in the user's RetroArch with a `.strm` pointing at the fixture and logs the same.

### M1 — Play one stream
- Tuner + video threads, variant selection, native-resolution ring with zero-copy present, audio resample, clock
  with buffer controller, output-rate handling, start/stall handling, `.strm` content, in-picture tuning card.
- **Done when:** the HLS fixture plays in the harness with A/V offset within ±1 frame and no drift over 30 min of
  simulated time; the best variant is the one downloaded (server log); a real public **1080p** HLS stream (and a
  1080i one, deinterlaced) plays 30 min in the user's RetroArch without audio underruns or dropped frames; CPU per
  stream and max `retro_run` time recorded.

### M2 — Playlists and channel changing
- M3U parser, channel list, up/down/last/number entry, banner OSD, last-channel persistence, channel list overlay,
  geometry changes between channels of different sizes.
- **Done when:** a fixture playlist of 10 local channels (mixed resolutions, some dead) zaps in < 1 s to picture,
  dead channels show an error card without hanging, rapid zapping (20 presses) never blocks `retro_run` (harness
  asserts max time), and the same works in the user's RetroArch.

### M3 — iptv-org
- Fetch/cache/filter, options, dead-stream memory, geo-blocked/NSFW handling, default UK lineup with no content.
- **Done when:** "Start Core" with no content builds the lineup online and offline-from-cache, and a scripted zap
  through the first 50 channels finishes with every channel either playing or marked dead.

### M3b — HDHomeRun (added 2026-10-22, user request)
- Find HDHomeRun network tuners on the LAN (SiliconDust's UDP discovery broadcast on port 65001, else the
  `hdhomerun.local` mDNS name), read `discover.json` and `lineup.json`, play their MPEG-TS streams (ATSC 1.0:
  MPEG-2/H.264 + AC-3, interlaced: already handled). Lineup option: Auto (a tuner if found, else iptv-org) /
  iptv-org / HDHomeRun / both (iptv-org numbered from 1001). Major.minor channel numbers ("3.1") in the banner,
  list, number entry (keyboard . or -) and the remembered state. DRM and AC-4 (ATSC 3.0) channels marked unplayable
  (user: not a concern); tuner failures are never "dead"; a 5xx reads NO FREE TUNER.
- **Done when:** the user's HDHomeRun FLEX 4K is found with no setup, its ATSC 1.0 channels play with picture and
  sound, number entry reaches a channel like 10.1, and `make check` covers a fake tuner.

### M4 — Robustness
- Reconnects, stalls, step-down to a lower variant, format changes mid-stream (resolution, codec, audio layout, ad
  breaks), PTS wraps, loudness levelling, audio track choice (HDHomeRun channels carry SAP / described-video AC-3
  tracks).
- **Done when:** fault-injection suite (server.py) passes, including dropped connections and a server that goes away
  and comes back; a mid-stream resolution change and a 5.1 ↔ stereo change recover within 1 s without clicks; a
  stream crosses the 33-bit PTS wrap in sync; repeated stalls on HLS step down to a lower variant; audio tracks can be
  picked by language and cycled.
- **Deferred (user, 2026-10-22):** the 8-hour soak run (D-032). The 30-minute runs from M1 stand in for now.

### M5 — Distribution
- Builds for Linux x86_64/arm64, Windows, macOS, Android via CI like DeeVee; packaging, GitHub release, possibly
  buildbot / core-info.
- **Done when:** a tagged release on GitHub with every target built and tested in CI.

### After v1 — VRT phase (separate, out of scope here)
Once X-Streemly is a viable RetroArch core, switch to VRT and make it work well there: SD rendering (~480p via the
Max resolution cap), field output via `vrt_libretro_ext.h`, Quest performance (possibly hardware decode), VRT's
guide listing the channels, content folder (`m3u` is also claimed by `pcsx_rearmed` in VRT's
`core_registry.gd`), bundling. Plan it then, likely in VRT's own sessions.

---

## 8. Test corpus & methodology
- **Deterministic first:** everything in `make check` uses locally generated fixtures over `server.py`, no live
  internet (D-013).
- **A/V sync:** flash + beep fixtures, measured offset in the harness (DeeVee `HARNESS_AVSYNC` method).
- **Latency budget:** harness reports max and p99 `retro_run` time; any blocking call shows up there.
- **Fault injection:** server.py stalls N seconds, drops the TCP connection mid-segment, serves 404/403, rewrites a
  playlist with a discontinuity, changes resolution, wraps PTS.
- **Live smoke tests (manual / opt-in `make check-live`):** a short list of stable public iptv-org streams (24/7 HLS
  news channels, 1080p and 1080i), never required to pass for CI.
- **Oracle:** `ffplay`/`ffprobe` from the same FFmpeg build for stream info and frame comparisons.
- **RetroArch (D-016):** the user's Steam RetroArch at `~/.local/share/Steam/steamapps/common/RetroArch` (gl driver,
  60 Hz, vsync + audio sync on, as of 2026-10-21). Launch with the Steam runtime wrapper:
  `cd ~/.local/share/Steam/steamapps/common/RetroArch && ~/.local/share/Steam/steamapps/common/SteamLinuxRuntime/steam-runtime/run.sh ./retroarch.sh -L <core.so> <content> --verbose`
  (DeeVee's notes). **Never `--appendconfig`** (config_save_on_exit writes it into the real `retroarch.cfg`); for
  automated runs copy `retroarch.cfg` to the scratchpad, set `audio_driver = "null"` only if sound isn't needed,
  `config_save_on_exit = "false"`, `core_options_path = <scratch>/x.opt`, and launch with `--config <copy>`.
  Installing the core into RetroArch's `cores/` is a user-visible change: ask first.

---

## 9. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| 1080p/1080i/4K software decode + bwdif + swscale cost | Dropped frames on slower PCs | Frame-threaded decode, conversion on its own thread, early frame drop; Max resolution option; measure on the Deck in M1; hardware decode (VAAPI/D3D11VA/MediaCodec) only if needed after M1. |
| Big frames through libretro's software path (8 MB/frame at 1080p) | Memory bandwidth, frontend upload cost | Zero-copy present; measure RetroArch's upload cost in M1. A GL path is out of scope unless this fails. |
| FFmpeg's HLS demuxer weaknesses (live refresh, fMP4, discontinuities, slow open, no ABR) | Stalls, failed tunes | Tune options (§5.2); fault-injection tests; variant step-down (M4); read hls.c before M1; patch locally if needed (DeeVee's patches/ model). |
| Source/display clock drift | Buffer under/overrun after minutes | Buffer-depth controller (§5.5); 8-hour test (M4). |
| fps vs display rate in RetroArch (vsync + audio sync) | Judder, wrong speed | Declare the display rate, present by PTS (D-008); verify in M1. |
| Blocking network calls reach the emulation thread | Frontend freeze | Hard rule: no avformat/avio call outside the tuner/fetch threads; harness asserts `retro_run` time. |
| CA certificates on non-Linux platforms (no system bundle for mbedTLS) | TLS failures | Embedded Mozilla CA bundle for metadata; stream verification off by default (§5.13). |
| mbedTLS needs `--enable-version3` | Binaries are GPLv3 | Acceptable (D-004); stated in README. |
| Dead/geo-blocked streams dominate iptv-org lists | Poor first impression | Hide geo-blocked by default, dead-stream memory. |
| Content-rights concerns with stream lists | Distribution issues | Core hosts and ships no stream lists; it fetches iptv-org's public lists at runtime; NSFW and blocklisted channels removed. |

---

## 10. Coding conventions
- C99, no C++. libretro-common helpers (`rthreads`, `file_stream`, `rjson`).
- One module per subsystem; no globals outside `libretro.c` (pass an `xs_ctx_t*`).
- **Threading rule:** every field shared across a thread boundary is either behind the ring/mailbox lock or an
  atomic; document which thread owns each struct.
- Ported functions get a comment: `/* Ported from FFmpeg fftools/ffplay.c read_thread() @ n8.1.3 */`.
- Logging via `retro_log_printf` with `[X-Streemly]` prefix; FFmpeg's log routed there (`av_log_set_callback`).

---

## 11. Upstream versions
- FFmpeg: reuse DeeVee's pin **n8.1.3** (D-007 there), re-check at M0. mbedTLS: latest **3.6.x LTS** at M0.
  libretro-common: master at M0.
- Re-check at the start of M0 and M5.

---

## 12. Status board

| Milestone | Status | Notes |
|---|---|---|
| Plan | ☑ Approved (2026-10-21) | All §13 proposals accepted by the user. |
| M0 Skeleton | ☑ Done (2026-10-21) | Submodules FFmpeg n8.1.3, mbedTLS v3.6.7, libretro-common master (a3194c1). `Makefile.deps` builds both static (mbedTLS from a copy in `build/mbedtls/src`, its own Makefile). Core: `libretro.c` (options v2, FFmpeg log bridge, display-rate timing via GET_TARGET_REFRESH_RATE, black 640×360 frame then dupes, exact silence per tick, SET_MESSAGE_EXT status), `xs_source.c` (.strm / URL → source, open with headers, timeouts, reconnect, live_start_index, protocol whitelist), `xs_tuner.c` (thread, mailbox, interrupt callback for abort + 15 s open / 20 s data deadlines, stream-info log, packet read loop). `tests/harness.c` (real-time pacing, retro_run mean/p99/max excluding its own callbacks, unload time, OSD lines), `tests/server.py` (HTTP(S), looping live.m3u8 from vod.m3u8, `_delay/_status/_stall/_drop`), `tests/fixtures/make_fixtures.sh` (hls2 720p+1080p, i1080, i576 MPEG-2, p2997 AC-3, p50, HEVC/E-AC-3, TLS cert), `tests/check.sh`. **make check: 10/10 pass** (HTTPS HLS opens in ~0.5 s with both variants; retro_run max ≤ 111 µs; unload while connecting 4–6 ms; 404 → No signal). Core NEEDED = libc, libm only; 25 exports; glibc floor 2.35. **Acceptance in the user's Steam RetroArch** (copied config, `-L`): HTTPS HLS fixture opened in 465 ms, same stream log, OSD summary shown, tuner stopped in 8 ms on quit. **Next: M1.** |
| M1 One stream | ☑ Done (2026-10-22) | Tuner thread (variant pick, timeline epochs D-023, inline audio decode, retries/reconnect D-018), `xs_pktq` + video thread (frame threads, bwdif/yadif at field rate, swscale threaded into zero-copy slots, early drop), audio ring as the clock (D-020) with floor controller, adaptive jitter buffer and live-edge catch-up (D-019, D-022), presenter with tuning/no-signal/end cards (5×7 OSD font), wall-clock stats line. **make check 18/18** (A/V within 4.7–14 ms on every fixture, ≤ 40 ms at loop joins; field rate; caps; stall, gaps, file end). **Harness 30 min live HLS 1080p:** 1800/1800 flashes matched, mean +1.5 ms, worst +40.3 ms (at loop joins), 0 clicks/underruns/drops, 90 joins, 48 % of one core. **RetroArch 30 min, CNBC UK 1080p (public):** 59.95 ticks/s, 0 underruns, 7 drops at start, 295–299 MB flat, ~130 % CPU (whole frontend), two ad-splice discontinuities joined. **RetroArch 30 min, live 1080i (local fixture, public 1080i unsustainable: see notes):** 60.16 ticks/s, 90,480 pictures (50.3/s), 1 drop, 0 underruns, 91 joins, 279 MB flat, ~160 % CPU. Not done: A/V measured only in the harness; >1080p and AES-128 HLS untested; `retro_run` max can reach ~5 ms while the desktop is busy (make check limit 5 ms may flake). **Next: M2.** |
| M2 Playlists | ☑ Done (2026-10-22) | `xs_playlist` (VLC m3u.c port + IPTV extensions), `xs_chan` (numbering, groups, last/previous remembered), `xs_input` (RetroPad with auto-repeat, keyboard digits), `xs_tv` (zap with settle, last channel, number entry, groups, list, info), overlay in `xs_osd` drawn in place (D-024). **make check 26/26** (parser on BOM/CRLF/quoted commas/EXTGRP/EXTVLCOPT/KODIPROP/Latin-1/relative URLs; zap over a 10-channel lineup with dead, slow, DRM, gappy and unreachable channels: picture 0.18–0.45 s after each tune; 20 quick taps = 1 tune; 20 zaps 0.67 s apart = 21 tunes, retro_run p99 ≈ 1 ms; hold; groups; list; remembered across restarts). **RetroArch** (scripted through its Network RetroPad with `tests/ra_press.py`: see notes): remembered channel restored, Up/Up/Up skipping DRM, B, list, R/L groups, 10 taps = 1 tune, 1.5 s hold = 1 tune; picture 0.06–0.47 s after each tune; 60 ticks/s. Not done: remote playlists (M3), keyboard digits only tested in the harness. **Next: M3.** |
| M3 iptv-org | ☑ Done (2026-10-22) | `xs_fetch` (avio downloads, system CA bundle, close-delimited responses), `xs_iptv` (lineup thread: iptv-org lists, cache + offline fallback, filters, de-dup; channel lists at a URL), Channels options (generated tables), dead channels (24 h, auto-advance at start, D-029), favourites (X/Y), LOADING / NO CHANNELS cards. **make check 39/39** (fake iptv-org: cache, offline, TLS checked and refused, category, language, part-time, remote list, dead skip, favourites). **Live acceptance** (real iptv-org UK, 240 channels, channels 1–50 tuned by number 16 s apart): every channel ended with a picture (13) or marked dead (39); retro_run p99 0.16 ms, 12 % of one core over 13.7 min. |
| M3b HDHomeRun | ☑ Done (2026-10-22) | `src/xs_hdhr.c` (UDP discovery written from libhdhomerun's protocol, mDNS fallback, discover.json/lineup.json via libretro-common rjson), Lineup option, major.minor numbers (`XS_CHNO`), tuner-aware failures. On the user's FLEX 4K (64 channels, 5 unplayable): found by broadcast in 0.8 s, 3.1 WEDU-HD 1080i MPEG-2 + AC-3 plays (picture 2.5 s after tune: MPEG-2 GOP), "10.1" typed tunes at once. Fake tuner in make check (numbers, typed 3.2, DRM/AC-4 skipped, busy tuner not "dead", HDHomeRun + iptv-org numbering, unreachable). **RetroArch:** Start Core with no content → FLEX 4K found, 64 channels, 3.1 WEDU PBS picture + sound (−31 dBFS through PulseAudio). |
| M4 Robustness | ☑ Done (2026-10-22) | Fixtures `hls_change` (720p stereo AC-3 → 1080p 5.1 at a discontinuity), `wrap.ts` (33-bit PTS wrap), `tracks.ts` / `hls_tracks` (eng 1 kHz, spa 500 Hz, eng described 2 kHz); server faults `_throttle`, `_outage`, `_dropevery`. Variant step-down (D-033), jump fades (D-034), all audio tracks decoded at once with instant L2 switching (D-035), Volume Levelling (D-036). **make check 47/47** (format change with no re-buffer, A/V ≤ 23 ms; wrap 300/300 pictures; 150 KB/s throttle steps 1080p → 720p after two stalls, then none; every 5th request dropped; 8 s outage reconnects and rejoins; Audio Language picks Spanish; L2 changes the tone from the very next beep; levelling −23 → −18.7 dBFS). **Harness on the HDHomeRun:** WEDU 3.1 at ~−30 dBFS levelled to ~−22. **RetroArch:** WEDU 3.1 (three AC-3 tracks eng/spa/fre) cycled with L2 through the Network RetroPad, 0 underruns, 60.2 ticks/s. Not done: the 8-hour soak (D-032); a reconnect join can leave a single A/V transient of ~100 ms. **Next: M5.** |
| M5 Distribution | ☐ | |
| After v1: VRT | — | Separate phase, out of scope here. |

### Session notes / gotchas
- Build: `distrobox enter deevee-build -- make -j8`; tests: `distrobox enter deevee-build -- make check` (needs the
  fixtures: `make fixtures`, ~10 s). Deps rebuild only when their stamp in `build/` is deleted.
- FFmpeg n8.1.3: `tls_mbedtls.c` calls `ff_udp_*` (DTLS) without a configure dependency, so the `udp` protocol must be
  enabled or the core fails to link. The old `hls` *protocol* is gone; the `hls` demuxer is what we use.
- mbedTLS 3.6 plain Makefile: release tags ship the generated sources, but its Makefile still names the generator
  scripts as order-only prerequisites (copy `scripts/` and `framework/scripts/`), and its `static` target writes a
  seed file into `../tests`, so build the three `.a` targets by name. FFmpeg needs `--enable-version3` for it.
- FFmpeg's mbedTLS glue verifies only against `ca_file` (a path): CA verification (D-012, M3) needs a bundle on disk.
- libretro-common `rthreads.c` defines `_GNU_SOURCE`; with the container's glibc 2.39 that pulls `__isoc23_strtoul`/
  `__isoc23_fscanf` (GLIBC_2.38). `src/xs_glibc_compat.c` provides them inside the core (floor stays 2.35).
- `Makefile` variables used in a `:=` definition must be defined above it (FFMPEG_TLS was silently empty once).
- Live HLS fixture loop point: FFmpeg warns "Packet corrupt" and "DTS ... out of order" when live.m3u8 wraps to
  seg_000 (deliberate EXT-X-DISCONTINUITY): the M1 clock must treat it as a new segment (§5.5).
- With both HLS variants un-discarded FFmpeg downloads both (M0 behaviour); variant selection is M1 (§5.3).
- `codecpar->field_order` is unknown (`scan ?`) for H.264 inside HLS after find_stream_info, but known for the same
  streams as plain TS: M1 must take scan type from decoded frames.
- **Driving RetroArch unattended** (2026-10-22): its Network RetroPad (`network_remote_enable = "true"`,
  `network_remote_enable_user_p1 = "true"` in the test config) takes UDP packets on port
  `network_remote_base_port` (55400) + user: `struct remote_message { int port; int device; int index; int id;
  uint16_t state; }` (20 bytes, little endian; device 1 = joypad, id = RETRO_DEVICE_ID_JOYPAD_*, state 1/0). A
  small sender, `python3 -I tests/ra_press.py 55400 "8:UP,4:B,4:UP*1500"`, scripts presses. Keyboard keys can't be sent this way. RetroArch keeps
  core saves in `saves/X-Streemly/X-Streemly/` (per-core folder): remove test state files from there.
- **RetroArch test runs** (2026-10-21): `cp retroarch.cfg <scratch>/ra-test.cfg`, set `config_save_on_exit = "false"`,
  `core_options_path`, `video_fullscreen = "false"`, `libretro_log_level = "0"`; launch from the RetroArch dir with
  `SteamLinuxRuntime/steam-runtime/run.sh ./retroarch.sh --config <copy> --verbose --log-file <log> -L
  <repo>/xstreemly_libretro.so <file.strm>`; the Steam runtime reaches `localhost` (tests/server.py) fine. RetroArch
  still writes `config/X-Streemly/X-Streemly.opt` in its own dir on exit. Kill by PID (`pgrep -x retroarch`), never
  `pkill -f` from a shell whose command line contains the pattern. A `spectacle -b -n -f` screenshot captures the
  whole desktop (the user's other windows): delete it after checking.
- **Unattended RetroArch runs need `video_vsync = "false"` in the test config** (2026-10-22): with vsync on and the
  RetroArch window hidden or covered (the user working on the desktop), the GL swap inside `video_cb` waits on
  Wayland frame callbacks that never come and times out after 1 s, so `retro_run` takes 1000 ms and RetroArch runs
  at ~1 tick/s (the core logs it: "retro_run mean 982 ms … (incl. frontend callbacks)"). Audio sync paces the
  run instead (60.2 ticks/s). `ptrace` (eu-stack, gdb) is blocked inside the Steam runtime.
- Don't run several 1080p tests at once on the Deck: RetroArch fell to ~50 ticks/s while two harness runs decoded
  alongside it (the buffer then grows and catch-up absorbs it, but the results are no longer comparable).
- Real interlaced IPTV is rare: of 22 iptv-org entries labelled 1080i that answered, 3 decode as interlaced, all from
  one restream server (92.114.85.87) that delivers ~1.4 Mbit/s, below the channels' bitrate, and drops the
  connection every few minutes. The core rides it (reconnects joined, adaptive target 2.9 → 7.8 s, holds the
  picture through each stall) but it can't be a 30-minute acceptance source; the local live `hls_i1080` fixture is.
- **The core is installed in the user's Steam RetroArch** (`cores/xstreemly_libretro.so` + `.info`, 2026-10-22,
  user request): reinstall after changes the user should see, `strip --strip-debug -o
  <RetroArch>/cores/xstreemly_libretro.so xstreemly_libretro.so` and copy the `.info`.
- With Lineup = Auto the core finds the user's real HDHomeRun on the LAN: tests that start without content must
  pin `HARNESS_OPT_xstreemly_source=iptv` (or use `XSTREEMLY_HDHR`), or they tune the real tuner.
- M4 test notes (2026-10-22): `/_outage/` counts from the first request with that prefix (not server start, or it
  had passed by the time make check reached it). Remuxing `tracks.ts` to HLS with `-c copy` drops the
  `visual_impaired` disposition, so only the TS fixture has a "described" track. `tests/tone.py` lists the beeps
  in a `HARNESS_WAV` file (start, Hz, dBFS). The Network RetroPad's L2 is id 12 (`ra_press.py` knows it). RetroArch
  reads and writes per-core options in `<RetroArch>/config/X-Streemly/X-Streemly.opt`, not the test config's
  `core_options_path`: set test options there (and put the defaults back).
- iptv-org country playlists use `uk`, not ISO `gb` (`countries/gb.m3u` is a 404).
- iptv-org `streams.json` carries per-stream `referrer` and `user_agent`; the M3U lists carry them as
  `#EXTVLCOPT` lines. Some streams don't play without them.
- The user's Steam RetroArch has no FFmpeg core installed (2026-10-21); DeeVee is installed there.
- For the VRT phase later: VRT's `MessageHandler` logs core messages but doesn't show them; VRT matches content by
  folder first, and `m3u` is shared with `pcsx_rearmed`.

---

## 13. Decision log

| ID | Decision | Rationale | Status |
|---|---|---|---|
| D-001 | Network + demux on a tuner thread, video decode/convert on a video thread; `retro_run` only reads bounded rings and never blocks. | Network calls stall for seconds; 1080p decode must stay off the emulation thread. | **Accepted (user, 2026-10-21)** |
| D-002 | Software XRGB8888 output; no GL path in v1. | Works in every frontend; GL only if M1 shows the software path can't carry 1080p. | **Accepted (user, 2026-10-21)** (revised: the seed rule was for VRT) |
| D-003 | FFmpeg for all media and HTTP (avio for playlist/JSON fetches); no libcurl. | One network/TLS stack. | **Accepted (user, 2026-10-21)** |
| D-004 | Source GPL-2.0-or-later; distributed binaries GPLv3 because FFmpeg + mbedTLS needs `--enable-version3`; schannel on Windows. | Licence is the user's (seed); version3 verified in n8.1.3 configure. | **Accepted (user, 2026-10-21)** |
| D-005 | Output at the stream's **native resolution**; best HLS variant by default; a *Max resolution* option caps it. | User (2026-10-21): full resolution in the libretro core; SD is a VRT concern for later. | **Accepted (user, 2026-10-21)**; replaces the SD-output proposal |
| D-006 | Content: `.m3u/.m3u8` playlist or `.strm` URL as content, plus no-content mode for the iptv-org lineup. `.xstreemly` reserved. | Uses existing formats (M3U, Kodi `.strm`); no-content mode gives "TV out of the box". | **Accepted (user, 2026-10-21)** |
| D-007 | Live clock: tick-driven presentation, buffer-depth controller via `swr_set_compensation`, video by PTS. | Source and display clocks differ; ffplay's realtime approach adapted to libretro. | **Accepted (user, 2026-10-21)** |
| D-008 | Declare the display rate (`GET_TARGET_REFRESH_RATE`, else 60) and present frames by PTS; *Output rate* option. | Any stream rate plays at real speed on a fixed-rate display without av_info changes per channel. | **Accepted (user, 2026-10-21)**, verify in M1 (replaced the 50/59.94 auto-standard proposal, which was for a CRT) |
| D-009 | Channel number/name OSD drawn into the picture by the core. | Works in every frontend; feels like a real TV. | **Accepted (user, 2026-10-21)** |
| D-010 | iptv-org lineup built from its filtered M3U lists; the large JSON files not used in v1. | Lists are small and already carry flags and headers. | **Accepted (user, 2026-10-21)** |
| D-011 | NSFW channels always excluded (blocklist + `xxx` category); default lineup = configured country (UK), all categories, geo-blocked hidden. | Default-safe TV. No option to enable NSFW. | **Accepted (user, 2026-10-21)** |
| D-012 | TLS: verify for iptv-org metadata (bundled CA list), stream verification off by default. | Public streams, many broken certificates. | **Accepted (user, 2026-10-21)** |
| D-013 | `make check` uses only local fixtures over a local server with fault injection. | Deterministic tests (seed). | Accepted (seed, 2026-10-21) |
| D-014 | X-Streemly is a libretro core first; VRT integration is a separate phase after v1, out of scope for this plan. | User (2026-10-21). | **Accepted (user, 2026-10-21)** |
| D-015 | Linux x86_64 is the only target until M5; Android and others come with distribution. | Follows from D-014; Quest was the only reason for early Android. | **Accepted (user, 2026-10-21)** |
| D-016 | Manual and acceptance testing in the user's Steam RetroArch (not VRT), with DeeVee's safe-launch rules (§8). | User (2026-10-21). | **Accepted (user, 2026-10-21)** |
| D-017 | Decoder threads = half the CPUs; swscale and the deinterlace graph 2 threads each. | With FFmpeg's auto threading (~25 threads on the Deck's 8 CPUs) a preempted lock holder stalled `retro_run` 5–10 ms; capped: no tick over 1.5 ms in repeated runs, and less CPU. | Accepted (2026-10-21, M1) |
| D-018 | A reconnect after media flowed keeps the serial and joins the new connection's timeline after the furthest point reached; only a fresh tune or a failed first open starts a new serial. | Starting over dropped up to 20 s of buffered media and reset the picture on every hiccup. | Accepted (2026-10-21, M1) |
| D-019 | Live-edge catch-up: skip once when a full 20 s floor window is > 3 s over target, down to target + 0.5 s. | `live_start_index=-3` gives 6–18 s of backlog that ±0.5 % would take up to an hour to work off; judging by the floor keeps the skip clear of the next segment's wait (judging by the level caused underruns on CNBC UK). | Accepted (2026-10-21, M1) |
| D-020 | The audio ring is the clock (next sample's PTS); D-007's controller steers the 20 s buffer floor, not a smoothed level. | Lip sync is exact by construction (fixtures: within a few ms); the floor is what matters with segment bursts. | Accepted (2026-10-21, M1), refines D-007 |
| D-022 | Adaptive jitter buffer: floor target = option + 1 s per underrun (≤ +10 s) + half the observed burst. | A fixed 1 s floor underran on every 6 s CNBC segment (FFmpeg finds new segments up to half a target duration late); adaptive: 0 underruns. | Accepted (2026-10-22, M1) |
| D-023 | Timeline joins per stream through shared epochs; forward jumps ≤ 10 s are kept as gaps, only backward jumps > 1 s and forward > 10 s are joined. | One shared offset ping-ponged when streams crossed a jump at different times (interleaving), shifting A/V by seconds; expired segments are missing content, not a timeline break. Fixture `/_gap/` (content gaps, numbering intact) in make check. | Accepted (2026-10-22, M1) |
| D-024 | The overlay is drawn in place on the picture on screen; banner areas are saved/restored, opaque panels are pasted from a once-rendered buffer and only saved on cards (on stream frames a closed panel can linger one frame period). | Copying the whole 1080p frame (8 MB) to draw on cost 1–6 ms per new frame; in place: ~0.2 ms (banner), ~0.5 ms mean with the list open. | Accepted (2026-10-22, M2) |
| D-025 | `make check` timing: retro_run p99 ≤ 5 ms and max ≤ 50 ms (was max ≤ 5 ms). The harness reports each slow tick's thread CPU time. | On a busy desktop single ticks of 3–25 ms are the OS preempting the thread (e.g. 3.0 ms wall, 1.2 ms CPU); real blocking (network, held locks) costs tens of ms to seconds, which the max still catches. | Accepted (2026-10-22, M2) |
| D-026 | NSFW: no blocklist.json download; a defensive filter on the XXX category only. | iptv-org stopped distributing NSFW channels on 2024-01-30 (PLAYLISTS.md; the XXX category is empty). | Accepted (2026-10-22, M3), refines D-011 |
| D-027 | TLS for iptv-org downloads is checked against the system CA bundle (Linux paths); without one it warns and goes on unchecked. An embedded Mozilla bundle comes with the non-Linux builds (M5). User lists and streams aren't checked (D-012). | FFmpeg's mbedTLS glue only takes a `ca_file` path; Linux always has one. | Accepted (2026-10-22, M3) |
| D-028 | A `.strm` URL is a channel list if its path ends in `.m3u` or it has `type=m3u` (IPTV panels); everything else, `.m3u8` included, is tuned as a stream. A fetched list that turns out to be HLS plays as one channel. | `.m3u8` is HLS far more often than a channel list; fetching every URL first would delay every single-stream `.strm`. | Accepted (2026-10-22, M3) |
| D-029 | When the core picked the channel (start, new lineup) and it's dead, step on to the next, up to 20 times, until a button is pressed. | iptv-org lineups start with many dead channels (UK: channels 1–4 on 2026-10-22); landing on NO SIGNAL first is a poor start. | Accepted (2026-10-22, M3) |
| D-030 | HDHomeRun discovery is SiliconDust's UDP broadcast, written from the protocol (libhdhomerun is LGPL-2.1+, used as the reference only), with `hdhomerun.local` as a fallback; channels come from `lineup.json` (not `lineup.m3u`) for its DRM and codec fields. | No new dependency; the JSON says which channels can't play. | Accepted (2026-10-22, M3b) |
| D-031 | Channel numbers are `major * 1000 + minor` (`XS_CHNO`), shown "3.1" or "7". Number entry tunes as soon as no other number starts with what was typed. Lists keep their own numbers when every channel has a unique one. | ATSC sub-channels; one rule for "10.1" and for "24" in a 241-channel list. | Accepted (2026-10-22, M3b) |
| D-032 | No 8-hour soak test in M4; revisit later if long-run problems show up. | User (2026-10-22): "a bit much". 30-minute harness and RetroArch runs already show no drift or ring growth. | **Accepted (user, 2026-10-22)** |
| D-033 | Variant step-down: two underruns within 60 s on a live stream with a lower variant reconnect to the next variant down (keeping the serial, D-018); the learned extra buffer is dropped. Never steps back up. | FFmpeg's hls demuxer has no ABR; growing the buffer alone never catches up when the bandwidth is short. | Accepted (2026-10-22, M4) |
| D-034 | Audio timestamp jumps inside a tick get a 64-sample fade out before and in after (as at underruns). | Reconnect joins and gaps clicked (harness click detector); the jump is found in the ring by comparing each chunk's start with where reading stopped. | Accepted (2026-10-22, M4) |
| D-035 | All audio tracks (≤ 4) are decoded at once into per-track rings; L2 switches rings in the presenter. | Switching in the tuner waited for the read loop and then played out the old track's 2–6 s of buffer. Extra cost: one AC-3/AAC decode per track (~0.1 ms/packet). | Accepted (2026-10-22, M4) |
| D-036 | Volume Levelling is a core option, off by default. | Any AGC changes the programme's own dynamics; off keeps the broadcast as sent, on is one setting away for zappers. | Accepted (2026-10-22, M4) |
| D-021 | Live = no duration and (for non-hls inputs) no known size. | FFmpeg reports no duration for MPEG-TS over HTTP, so files were taken for live streams and "reconnected" in a loop. | Accepted (2026-10-21, M1) |

---

## 14. Key references
- iptv-org: https://github.com/iptv-org/iptv , API https://github.com/iptv-org/api (`https://iptv-org.github.io/api/`)
- FFmpeg: https://git.ffmpeg.org/ffmpeg.git (`fftools/ffplay.c`, `libavformat/hls.c`, `libavformat/http.c`)
- mbedTLS: https://github.com/Mbed-TLS/mbedtls
- libretro API: https://github.com/libretro/libretro-common/blob/master/include/libretro.h
- Prior art: RetroArch `cores/libretro-ffmpeg`, https://github.com/libretro/libretro-mpv , Kodi `pvr.iptvsimple`
- Sibling project: DeeVee `~/.claude/projects/DeeVee` (clock, audio, deinterlace, build, harness, RetroArch notes)
- Later phase: VRT `~/.claude/projects/VRT` (`include/vrt_libretro_ext.h`, `godot/scripts/retro/core_registry.gd`)
- Repo: https://github.com/willho89/X-Streemly (public), GPL-2.0-or-later
