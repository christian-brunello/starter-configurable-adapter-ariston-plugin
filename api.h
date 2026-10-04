/*
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARISTON_API_H
#define ARISTON_API_H

#include "ariston.h"

G_BEGIN_DECLS

gboolean ar_api_ensure_session(ArContext *ctx, GError **error);
gboolean ar_api_ensure_features(ArContext *ctx, GError **error);
gboolean ar_api_ensure_metering(ArContext *ctx, GError **error);
gboolean ar_api_refresh_live(ArContext *ctx, GError **error);

/* Set one named value (ST-side numbering for PlantMode). */
gboolean ar_api_set_value(ArContext *ctx, const gchar *name, gdouble value, GError **error);

/* TRUE for ChFlowSetpointTemp / HybridMode / PlantMode. */
gboolean ar_api_key_is_writable(const gchar *name);

/*
 * Refresh live (+ best-effort metering) subject to TTL/rate limits, then
 * fill @out with gchar* → STCAValue* (caller owns the table).
 */
gboolean ar_api_poll(ArContext *ctx, GHashTable *out, GError **error);

G_END_DECLS

#endif /* ARISTON_API_H */
