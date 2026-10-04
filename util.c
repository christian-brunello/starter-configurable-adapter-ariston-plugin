/*
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ariston.h"
#include "http.h"
#include "store.h"

#include <string.h>

void
ar_log(GLogLevelFlags level, const char *fmt, ...)
{
  va_list ap;
  gchar *msg;

  va_start(ap, fmt);
  msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);

  g_log("ariston-remotethermo", level, "%s", msg);
  g_free(msg);
}

void
ar_options_init_defaults(ArOptions *opt)
{
  memset(opt, 0, sizeof(*opt));
  opt->live_max_calls_per_minute = AR_DEFAULT_LIVE_MAX_CALLS_PER_MINUTE;
  opt->metering_max_calls_per_minute = AR_DEFAULT_METERING_MAX_CALLS_PER_MINUTE;
  opt->set_max_calls_per_minute = AR_DEFAULT_SET_MAX_CALLS_PER_MINUTE;
  opt->features_ttl_ms = AR_DEFAULT_FEATURES_TTL_MS;
  opt->session_ttl_ms = AR_DEFAULT_SESSION_TTL_MS;
  opt->metering_ttl_ms = AR_DEFAULT_METERING_TTL_MS;
  opt->poll_interval_ms = AR_DEFAULT_POLL_INTERVAL_MS;
  opt->live_ttl_ms = opt->poll_interval_ms;
}

void
ar_options_clear(ArOptions *opt)
{
  if (opt == NULL)
    return;

  g_clear_pointer(&opt->host, g_free);
  g_clear_pointer(&opt->user, g_free);
  g_clear_pointer(&opt->password, g_free);
  g_clear_pointer(&opt->gw, g_free);
}

gboolean
ar_parse_duration_ms(const char *text,
		     guint64 *out_ms,
		     gboolean allow_zero,
		     GError **error)
{
  gchar *end = NULL;
  guint64 v;
  guint64 mult = 1;

  g_return_val_if_fail(text != NULL, FALSE);
  g_return_val_if_fail(out_ms != NULL, FALSE);

  v = g_ascii_strtoull(text, &end, 10);
  if (end == text || (!allow_zero && v == 0))
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "invalid duration `%s'", text);
      return FALSE;
    }

  if (*end == '\0')
    {
      *out_ms = v;
      return TRUE;
    }

  if (g_strcmp0(end, "ms") == 0)
    mult = 1;
  else if (g_strcmp0(end, "s") == 0)
    mult = 1000;
  else if (g_strcmp0(end, "m") == 0)
    mult = 60 * 1000;
  else if (g_strcmp0(end, "h") == 0)
    mult = 3600 * 1000;
  else if (g_strcmp0(end, "d") == 0)
    mult = 24 * 3600 * 1000ULL;
  else if (g_strcmp0(end, "y") == 0)
    mult = 365 * 24 * 3600 * 1000ULL;
  else
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "invalid duration suffix in `%s' (use ms, s, m, h, d, y)", text);
      return FALSE;
    }

  if (v > G_MAXUINT64 / mult)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "duration `%s' is too large", text);
      return FALSE;
    }

  *out_ms = v * mult;
  return TRUE;
}

static gchar *
ar_normalize_host(const gchar *host)
{
  gchar *h;
  gsize len;

  h = g_strdup(host);
  g_strstrip(h);
  len = strlen(h);
  while (len > 0 && h[len - 1] == '/')
    {
      h[len - 1] = '\0';
      len--;
    }
  return h;
}

static void
ar_setup_headers(ArContext *ctx)
{
  ctx->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  g_hash_table_insert(ctx->headers, g_strdup("Accept"),
		      g_strdup("application/json, text/javascript, */*; q=0.01"));
  g_hash_table_insert(ctx->headers, g_strdup("Accept-Language"),
		      g_strdup("it-IT,it;q=0.8,en-US;q=0.5,en;q=0.3"));
  g_hash_table_insert(ctx->headers, g_strdup("Accept-Encoding"),
		      g_strdup("gzip, deflate"));
  g_hash_table_insert(ctx->headers, g_strdup("Connection"), g_strdup("keep-alive"));
  g_hash_table_insert(ctx->headers, g_strdup("Ajax-Request"), g_strdup("json"));
  g_hash_table_insert(ctx->headers, g_strdup("X-Requested-With"),
		      g_strdup("XMLHttpRequest"));
  g_hash_table_insert(ctx->headers, g_strdup("Origin"),
		      g_strdup(AR_DEFAULT_ORIGIN));
  g_hash_table_insert(ctx->headers, g_strdup("Sec-Fetch-Dest"), g_strdup("empty"));
  g_hash_table_insert(ctx->headers, g_strdup("Sec-Fetch-Mode"), g_strdup("cors"));
  g_hash_table_insert(ctx->headers, g_strdup("Sec-Fetch-Site"), g_strdup("same-origin"));
  g_hash_table_insert(ctx->headers, g_strdup("Sec-Fetch-User"), g_strdup("?1"));
}

gboolean
ar_context_open(ArContext *ctx, ArOptions *opt, GError **error)
{
  memset(ctx, 0, sizeof(*ctx));
  ctx->opt = opt;

  {
    gchar *norm = ar_normalize_host(opt->host);

    g_free(opt->host);
    opt->host = norm;
  }

  /* In-memory cookie jar (no state-dir). */
  ctx->http = ar_http_new(NULL, opt->verbose, error);
  if (ctx->http == NULL)
    return FALSE;

  ctx->store = ar_store_new();
  ar_setup_headers(ctx);
  return TRUE;
}

void
ar_context_close(ArContext *ctx)
{
  if (ctx == NULL)
    return;

  g_clear_pointer(&ctx->headers, g_hash_table_unref);
  ar_store_close(ctx->store);
  ctx->store = NULL;
  ar_http_free(ctx->http);
  ctx->http = NULL;
  ctx->opt = NULL;
}
