/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * M3U/M3U8 channel lists (see xs_playlist.h).
 *
 * Adapted from VLC modules/demux/playlist/m3u.c (LGPL-2.1-or-later),
 * ReadDir() and parseEXTINF*() @ master 2026-10: tags are matched
 * case-insensitively after any run of '#'; #EXTINF attributes are scanned
 * quote-aware, so "a,b" inside a value doesn't end them, and the first
 * unquoted comma starts the title; #EXTGRP carries over to later entries;
 * #EXTVLCOPT applies to the next entry; a URL line ends an entry.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "xs_playlist.h"

#define XS_PLAYLIST_MAX_BYTES (16u << 20)

/* ---- text helpers ------------------------------------------------------ */

static void copy(char *dst, size_t len, const char *src, size_t n)
{
   if (n >= len)
      n = len - 1;
   memcpy(dst, src, n);
   dst[n] = '\0';
}

static void trim(char *s)
{
   size_t n = strlen(s);
   char *p = s;
   while (n && isspace((unsigned char)s[n - 1]))
      s[--n] = '\0';
   while (isspace((unsigned char)*p))
      p++;
   if (p != s)
      memmove(s, p, strlen(p) + 1);
}

static bool starts_with_ci(const char *s, const char *prefix)
{
   size_t n = strlen(prefix);
   return !strncasecmp(s, prefix, n);
}

static bool valid_utf8(const unsigned char *s, size_t n)
{
   size_t i = 0, k;
   while (i < n)
   {
      unsigned c = s[i], need;
      if (c < 0x80)
      {
         i++;
         continue;
      }
      if (c >= 0xC2 && c <= 0xDF)
         need = 1;
      else if (c >= 0xE0 && c <= 0xEF)
         need = 2;
      else if (c >= 0xF0 && c <= 0xF4)
         need = 3;
      else
         return false;
      if (i + need >= n)
         return false;
      for (k = 1; k <= need; k++)
         if ((s[i + k] & 0xC0) != 0x80)
            return false;
      i += need + 1;
   }
   return true;
}

/* Lists that aren't UTF-8 are almost always Latin-1 (VLC guesses the same
 * way, GuessEncoding()): widen each byte. */
static char *to_utf8(const char *text, size_t len)
{
   char *out;
   size_t i, o = 0;
   if (valid_utf8((const unsigned char *)text, len))
   {
      out = malloc(len + 1);
      if (out)
      {
         memcpy(out, text, len);
         out[len] = '\0';
      }
      return out;
   }
   out = malloc(len * 2 + 1);
   if (!out)
      return NULL;
   for (i = 0; i < len; i++)
   {
      unsigned char c = (unsigned char)text[i];
      if (c < 0x80)
         out[o++] = (char)c;
      else
      {
         out[o++] = (char)(0xC0 | (c >> 6));
         out[o++] = (char)(0x80 | (c & 0x3F));
      }
   }
   out[o] = '\0';
   return out;
}

static bool has_scheme(const char *s)
{
   const char *p = s;
   while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')
      p++;
   return p != s && p[0] == ':' && p[1] == '/' && p[2] == '/';
}

/* Resolve a playlist entry against the playlist's location. */
static void resolve(const char *base, const char *ref, char *out, size_t len, bool *local)
{
   const char *slash;
   *local = false;
   if (has_scheme(ref))
   {
      copy(out, len, ref, strlen(ref));
      return;
   }
   if (has_scheme(base))
   {
      const char *host = strstr(base, "://") + 3;
      if (ref[0] == '/' && ref[1] == '/')         /* scheme-relative */
         snprintf(out, len, "%.*s%s", (int)(host - base - 2), base, ref);
      else if (ref[0] == '/')                     /* host-relative */
         snprintf(out, len, "%.*s%s", (int)(host - base + strcspn(host, "/?#")), base, ref);
      else
      {
         const char *end = base + strcspn(base, "?#");
         slash = end;
         while (slash > host && slash[-1] != '/')
            slash--;
         snprintf(out, len, "%.*s%s", (int)(slash - base), base, ref);
      }
      return;
   }
   *local = true;
   if (ref[0] == '/' || !base[0])
   {
      copy(out, len, ref, strlen(ref));
      return;
   }
   slash = strrchr(base, '/');
   if (slash)
      snprintf(out, len, "%.*s%s", (int)(slash - base + 1), base, ref);
   else
      copy(out, len, ref, strlen(ref));
}

/* ---- channel numbers ---------------------------------------------------- */

unsigned xs_chno_parse(const char *s)
{
   char *end;
   unsigned long major, minor = 0;
   while (*s == ' ')
      s++;
   if (!isdigit((unsigned char)*s))
      return 0;
   major = strtoul(s, &end, 10);
   if (*end == '.' || *end == '-' || *end == '_')
   {
      if (!isdigit((unsigned char)end[1]))
         return 0;
      minor = strtoul(end + 1, &end, 10);
   }
   while (*end == ' ')
      end++;
   if (*end || major == 0 || major > 4000000 || minor > 999)
      return 0;
   return XS_CHNO((unsigned)major, (unsigned)minor);
}

void xs_chno_format(unsigned n, char *out, size_t len)
{
   if (XS_CHNO_MINOR(n))
      snprintf(out, len, "%u.%u", XS_CHNO_MAJOR(n), XS_CHNO_MINOR(n));
   else
      snprintf(out, len, "%u", XS_CHNO_MAJOR(n));
}

/* ---- #EXTINF -------------------------------------------------------------- */

static void set_attr(xs_entry_t *e, const char *key, size_t klen, const char *val, size_t vlen)
{
#define KEY(k) (klen == sizeof(k) - 1 && !strncasecmp(key, k, klen))
   if (KEY("tvg-name"))
   {
      if (!e->name[0])
         copy(e->name, sizeof(e->name), val, vlen);
   }
   else if (KEY("tvg-id"))
      copy(e->tvg_id, sizeof(e->tvg_id), val, vlen);
   else if (KEY("tvg-logo"))
      copy(e->logo, sizeof(e->logo), val, vlen);
   else if (KEY("tvg-language"))
      copy(e->language, sizeof(e->language), val, vlen);
   else if (KEY("tvg-chno") || KEY("channel-number"))
   {
      char num[16];
      copy(num, sizeof(num), val, vlen);
      e->chno = xs_chno_parse(num);
   }
   else if (KEY("group-title"))
      copy(e->group, sizeof(e->group), val, vlen);
   else if (KEY("http-user-agent") || KEY("user-agent"))
      copy(e->user_agent, sizeof(e->user_agent), val, vlen);
   else if (KEY("http-referrer") || KEY("referrer"))
      copy(e->referrer, sizeof(e->referrer), val, vlen);
#undef KEY
}

/* "#EXTINF:-1 tvg-id="a" group-title="News, UK",Title" (after the colon). */
static void parse_extinf(const char *s, xs_entry_t *e, char *title, size_t tlen)
{
   char *end;
   strtod(s, &end);   /* duration: -1 for live */
   s = end;
   for (;;)
   {
      const char *key, *val;
      size_t klen, vlen;
      while (*s == ' ' || *s == '\t')
         s++;
      if (!*s)
         return;
      if (*s == ',')
      {
         copy(title, tlen, s + 1, strlen(s + 1));
         trim(title);
         return;
      }
      key = s;
      while (*s && *s != '=' && *s != ',' && *s != ' ' && *s != '\t')
         s++;
      klen = (size_t)(s - key);
      if (*s != '=')
         continue;   /* a bare word: skip it */
      s++;
      if (*s == '"')
      {
         val = ++s;
         while (*s && *s != '"')
            s++;
         vlen = (size_t)(s - val);
         if (*s == '"')
            s++;
      }
      else
      {
         val = s;
         while (*s && *s != ',' && *s != ' ' && *s != '\t')
            s++;
         vlen = (size_t)(s - val);
      }
      set_attr(e, key, klen, val, vlen);
   }
}

/* iptv-org appends " (720p)", " [Geo-blocked]", " [Not 24/7]" to titles:
 * strip them from the display name, keep them as data. */
static void strip_suffixes(xs_entry_t *e)
{
   for (;;)
   {
      size_t n = strlen(e->name);
      char *open;
      if (n < 3)
         return;
      if (e->name[n - 1] == ']' && (open = strrchr(e->name, '[')))
      {
         if (!strcasecmp(open, "[Geo-blocked]"))
            e->flags |= XS_CH_GEOBLOCKED;
         else if (!strcasecmp(open, "[Not 24/7]"))
            e->flags |= XS_CH_NOT_247;
         else
            return;
      }
      else if (e->name[n - 1] == ')' && (open = strrchr(e->name, '(')) &&
               isdigit((unsigned char)open[1]))
      {
         char *q = open + 1;
         unsigned h = (unsigned)strtoul(q, &q, 10);
         if ((*q != 'p' && *q != 'i') || q[1] != ')')
            return;
         e->height = h;
      }
      else
         return;
      *open = '\0';
      trim(e->name);
   }
}

/* ---- parser ---------------------------------------------------------------- */

static void entry_init(xs_entry_t *e)
{
   memset(e, 0, sizeof(*e));
}

bool xs_playlist_push(xs_playlist_t *pl, const xs_entry_t *e)
{
   if (pl->n == pl->cap)
   {
      unsigned cap = pl->cap ? pl->cap * 2 : 64;
      xs_entry_t *ne = realloc(pl->e, cap * sizeof(*ne));
      if (!ne)
         return false;
      pl->e = ne;
      pl->cap = cap;
   }
   pl->e[pl->n++] = *e;
   return true;
}

/* An HLS playlist given as content is one channel, not a list (§5.1). */
static bool is_hls(const char *text)
{
   return strstr(text, "#EXT-X-TARGETDURATION") || strstr(text, "#EXT-X-STREAM-INF") ||
          strstr(text, "#EXT-X-MEDIA-SEQUENCE");
}

bool xs_playlist_parse(const char *text, size_t len, const char *base, xs_playlist_t *out,
                       char *err, size_t errlen)
{
   char *buf, *line, *next;
   char group[128] = "", title[256] = "";
   xs_entry_t cur;

   memset(out, 0, sizeof(*out));
   if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3))
   {
      text += 3;
      len  -= 3;
   }
   if (!(buf = to_utf8(text, len)))
   {
      snprintf(err, errlen, "out of memory");
      return false;
   }

   if (is_hls(buf))
   {
      entry_init(&cur);
      copy(cur.url, sizeof(cur.url), base, strlen(base));
      cur.local = !has_scheme(base);
      out->hls = true;
      free(buf);
      return xs_playlist_push(out, &cur);
   }

   entry_init(&cur);
   for (line = buf; line; line = next)
   {
      char *p;
      /* CR, LF or CRLF (the empty line between CR and LF is skipped) */
      next = line + strcspn(line, "\r\n");
      if (*next)
         *next++ = '\0';
      else
         next = NULL;
      trim(line);
      if (!*line)
         continue;

      if (*line == '#')
      {
         p = line;
         while (*p == '#')
            p++;
         if (starts_with_ci(p, "EXTINF:"))
         {
            title[0] = '\0';
            parse_extinf(p + 7, &cur, title, sizeof(title));
         }
         else if (starts_with_ci(p, "EXTGRP:"))
         {
            copy(group, sizeof(group), p + 7, strlen(p + 7));
            trim(group);
         }
         else if (starts_with_ci(p, "EXTVLCOPT:"))
         {
            const char *o = p + 10;
            if (starts_with_ci(o, "http-user-agent="))
               copy(cur.user_agent, sizeof(cur.user_agent), o + 16, strlen(o + 16));
            else if (starts_with_ci(o, "http-referrer="))
               copy(cur.referrer, sizeof(cur.referrer), o + 14, strlen(o + 14));
         }
         else if (starts_with_ci(p, "KODIPROP:"))
         {
            /* inputstream.adaptive.license_type / license_key: DRM. */
            if (strstr(p, "license_type") || strstr(p, "license_key"))
               cur.flags |= XS_CH_DRM;
         }
         continue;
      }

      /* a URL line ends the entry */
      resolve(base, line, cur.url, sizeof(cur.url), &cur.local);
      if (title[0])
         copy(cur.name, sizeof(cur.name), title, strlen(title));   /* the title wins over tvg-name */
      if (!cur.name[0])
      {
         const char *b = strrchr(line, '/');
         copy(cur.name, sizeof(cur.name), b ? b + 1 : line, strlen(b ? b + 1 : line));
      }
      if (!cur.group[0] && group[0])
         copy(cur.group, sizeof(cur.group), group, strlen(group));
      strip_suffixes(&cur);
      if (!xs_playlist_push(out, &cur))
         break;
      entry_init(&cur);
      title[0] = '\0';
   }
   free(buf);
   if (!out->n)
   {
      snprintf(err, errlen, "no channels in the playlist");
      return false;
   }
   return true;
}

bool xs_playlist_load(const char *path, xs_playlist_t *out, char *err, size_t errlen)
{
   FILE *f = fopen(path, "rb");
   char *text;
   long size;
   size_t got;
   bool ok;

   memset(out, 0, sizeof(*out));
   if (!f)
   {
      snprintf(err, errlen, "can't open %s", path);
      return false;
   }
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (size < 0 || (unsigned long)size > XS_PLAYLIST_MAX_BYTES)
   {
      fclose(f);
      snprintf(err, errlen, "%s is too big for a playlist", path);
      return false;
   }
   text = malloc((size_t)size + 1);
   got = text ? fread(text, 1, (size_t)size, f) : 0;
   fclose(f);
   if (!text)
   {
      snprintf(err, errlen, "out of memory");
      return false;
   }
   ok = xs_playlist_parse(text, got, path, out, err, errlen);
   free(text);
   return ok;
}

void xs_playlist_free(xs_playlist_t *pl)
{
   free(pl->e);
   memset(pl, 0, sizeof(*pl));
}
