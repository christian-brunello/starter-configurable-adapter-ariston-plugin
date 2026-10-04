/*
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARISTON_STORE_H
#define ARISTON_STORE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct ArStore ArStore;

/* In-memory cache (no disk / sqlite). */
ArStore *ar_store_new(void);
void ar_store_close(ArStore *store);

gboolean ar_store_meta_get(ArStore *store, const gchar *key, gchar **out_value, gint64 *out_updated_at, GError **error);
gboolean ar_store_meta_set(ArStore *store, const gchar *key, const gchar *value, GError **error);

gboolean ar_store_snapshot_clear_group(ArStore *store, const gchar *group, GError **error);
gboolean ar_store_snapshot_set(ArStore *store, const gchar *group, const gchar *key, gdouble value, GError **error);
gboolean ar_store_snapshot_foreach(ArStore *store,
				   void (*cb)(const gchar *group, const gchar *key, gdouble value, gint64 updated_at, gpointer user_data),
				   gpointer user_data,
				   GError **error);

gboolean ar_store_group_fresh(ArStore *store, const gchar *group, guint64 ttl_ms, gint64 *out_age_ms);
gboolean ar_store_group_touch(ArStore *store, const gchar *group, GError **error);
gboolean ar_store_group_invalidate(ArStore *store, const gchar *group, GError **error);

/* Per-kind sliding 60s window. max_per_minute 0 = unlimited. */
gboolean ar_store_rate_acquire(ArStore *store,
			       const gchar *kind,
			       guint max_per_minute,
			       GError **error);

/* Kept for API compatibility with the former CLI; always no-ops. */
gboolean ar_store_history_append(ArStore *store,
				 const gchar *kind,
				 const gchar *json_payload,
				 GError **error);
gboolean ar_store_history_prune(ArStore *store,
				guint64 retain_ms,
				guint64 max_bytes,
				GError **error);

G_END_DECLS

#endif /* ARISTON_STORE_H */
