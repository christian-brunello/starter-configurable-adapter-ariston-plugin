/*
 * ariston-remotethermo — Ariston / Remotethermo cloud plugin for STCA.
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARISTON_REMOTETHERMO_H
#define ARISTON_REMOTETHERMO_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Rate limits unused for the self-paced plugin (0 = unlimited). */
#define AR_DEFAULT_LIVE_MAX_CALLS_PER_MINUTE 0
#define AR_DEFAULT_METERING_MAX_CALLS_PER_MINUTE 0
#define AR_DEFAULT_SET_MAX_CALLS_PER_MINUTE 0
#define AR_DEFAULT_FEATURES_TTL_MS ((guint64) 12 * 3600 * 1000)
#define AR_DEFAULT_SESSION_TTL_MS ((guint64) 12 * 3600 * 1000)
#define AR_DEFAULT_METERING_TTL_MS ((guint64) 2 * 3600 * 1000) /* 2h slots */
#define AR_DEFAULT_POLL_INTERVAL_MS ((guint) 60 * 1000)
/* live_ttl tracks poll_interval (set in configure). */
/* Brand Origin used by the official web clients (not necessarily == host). */
#define AR_DEFAULT_ORIGIN "https://www.chaffolink.remotethermo.com/"

/* Rate-limit bucket ids. Independent counters. */
#define AR_RATE_LIVE "live"
#define AR_RATE_METERING "metering"
#define AR_RATE_SET "set"

typedef struct {
  gchar *host; /* https://… without trailing slash */
  gchar *user;
  gchar *password;
  gchar *gw;
  guint live_max_calls_per_minute;     /* internal; 0 = off */
  guint metering_max_calls_per_minute; /* internal; 0 = off */
  guint set_max_calls_per_minute;      /* internal; 0 = off */
  guint64 live_ttl_ms;                 /* = poll_interval_ms */
  guint64 features_ttl_ms;
  guint64 session_ttl_ms;
  guint64 metering_ttl_ms;
  guint poll_interval_ms;
  gboolean force; /* ignore TTL for live/metering (debug) */
  gboolean verbose;
} ArOptions;

typedef struct ArHttp ArHttp;
typedef struct ArStore ArStore;

typedef struct {
  ArOptions *opt;
  ArHttp *http;
  ArStore *store;
  GHashTable *headers; /* string→string, owned */
} ArContext;

gboolean ar_parse_duration_ms(const char *text,
			      guint64 *out_ms,
			      gboolean allow_zero,
			      GError **error);

void ar_options_clear(ArOptions *opt);
void ar_options_init_defaults(ArOptions *opt);

gboolean ar_context_open(ArContext *ctx, ArOptions *opt, GError **error);
void ar_context_close(ArContext *ctx);

void ar_log(GLogLevelFlags level, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

G_END_DECLS

#endif /* ARISTON_REMOTETHERMO_H */
