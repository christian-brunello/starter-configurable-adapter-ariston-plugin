/*
 * In-memory ArStore (TTL / rate-limit / snapshot cache).
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "store.h"
#include "ariston.h"

#include <string.h>

typedef struct {
  gchar *value;
  gint64 updated_at;
} ArMetaEntry;

typedef struct {
  gdouble value;
  gint64 updated_at;
} ArSnapEntry;

typedef struct {
  gint64 window_start;
  gint64 calls;
} ArRateEntry;

struct ArStore {
  GHashTable *meta;	  /* key → ArMetaEntry* */
  GHashTable *snapshot;   /* "group\0key" → ArSnapEntry*  (encoded as "group\x1fkey") */
  GHashTable *rate;	  /* kind → ArRateEntry* */
};

static void
ar_meta_entry_free(gpointer p)
{
  ArMetaEntry *e = p;

  if (e == NULL)
    return;
  g_free(e->value);
  g_free(e);
}

static void
ar_snap_entry_free(gpointer p)
{
  g_free(p);
}

static void
ar_rate_entry_free(gpointer p)
{
  g_free(p);
}

static gchar *
ar_snap_composite_key(const gchar *group, const gchar *key)
{
  return g_strdup_printf("%s\x1f%s", group, key);
}

ArStore *
ar_store_new(void)
{
  ArStore *store;

  store = g_new0(ArStore, 1);
  store->meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ar_meta_entry_free);
  store->snapshot = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ar_snap_entry_free);
  store->rate = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ar_rate_entry_free);
  return store;
}

void
ar_store_close(ArStore *store)
{
  if (store == NULL)
    return;

  g_clear_pointer(&store->meta, g_hash_table_unref);
  g_clear_pointer(&store->snapshot, g_hash_table_unref);
  g_clear_pointer(&store->rate, g_hash_table_unref);
  g_free(store);
}

gboolean
ar_store_meta_get(ArStore *store,
		  const gchar *key,
		  gchar **out_value,
		  gint64 *out_updated_at,
		  GError **error)
{
  ArMetaEntry *e;

  (void) error;

  if (out_value)
    *out_value = NULL;

  e = g_hash_table_lookup(store->meta, key);
  if (e == NULL)
    return TRUE;

  if (out_value)
    *out_value = g_strdup(e->value);
  if (out_updated_at)
    *out_updated_at = e->updated_at;
  return TRUE;
}

gboolean
ar_store_meta_set(ArStore *store, const gchar *key, const gchar *value, GError **error)
{
  ArMetaEntry *e;

  (void) error;

  e = g_new0(ArMetaEntry, 1);
  e->value = g_strdup(value != NULL ? value : "");
  e->updated_at = g_get_real_time() / 1000;
  g_hash_table_replace(store->meta, g_strdup(key), e);
  return TRUE;
}

gboolean
ar_store_snapshot_clear_group(ArStore *store, const gchar *group, GError **error)
{
  GHashTableIter iter;
  gpointer k;
  gchar *prefix;
  gsize prefix_len;

  (void) error;

  prefix = g_strdup_printf("%s\x1f", group);
  prefix_len = strlen(prefix);

  g_hash_table_iter_init(&iter, store->snapshot);
  while (g_hash_table_iter_next(&iter, &k, NULL))
    {
      const gchar *ck = k;

      if (strncmp(ck, prefix, prefix_len) == 0)
	g_hash_table_iter_remove(&iter);
    }

  g_free(prefix);
  return TRUE;
}

gboolean
ar_store_snapshot_set(ArStore *store,
		      const gchar *group,
		      const gchar *key,
		      gdouble value,
		      GError **error)
{
  ArSnapEntry *e;
  gchar *ck;

  (void) error;

  e = g_new0(ArSnapEntry, 1);
  e->value = value;
  e->updated_at = g_get_real_time() / 1000;
  ck = ar_snap_composite_key(group, key);
  g_hash_table_replace(store->snapshot, ck, e);
  return TRUE;
}

gboolean
ar_store_snapshot_foreach(ArStore *store,
			  void (*cb)(const gchar *group, const gchar *key, gdouble value, gint64 updated_at, gpointer user_data),
			  gpointer user_data,
			  GError **error)
{
  GHashTableIter iter;
  gpointer k, v;

  (void) error;

  g_hash_table_iter_init(&iter, store->snapshot);
  while (g_hash_table_iter_next(&iter, &k, &v))
    {
      const gchar *ck = k;
      const ArSnapEntry *e = v;
      const gchar *sep;
      gchar *group;
      const gchar *key;

      sep = strchr(ck, '\x1f');
      if (sep == NULL)
	continue;

      group = g_strndup(ck, (gsize) (sep - ck));
      key = sep + 1;
      cb(group, key, e->value, e->updated_at, user_data);
      g_free(group);
    }

  return TRUE;
}

gboolean
ar_store_group_fresh(ArStore *store, const gchar *group, guint64 ttl_ms, gint64 *out_age_ms)
{
  gchar *key;
  gchar *value = NULL;
  gint64 fetched_at = 0;
  gint64 now;
  gint64 age;

  key = g_strdup_printf("group.%s.fetched_at", group);
  if (!ar_store_meta_get(store, key, &value, NULL, NULL))
    {
      g_free(key);
      return FALSE;
    }
  g_free(key);

  if (value != NULL && *value != '\0')
    fetched_at = g_ascii_strtoll(value, NULL, 10);
  g_free(value);

  if (fetched_at <= 0)
    return FALSE;

  now = g_get_real_time() / 1000;
  age = now - fetched_at;
  if (out_age_ms)
    *out_age_ms = age;

  if (ttl_ms == 0)
    return FALSE;

  return age >= 0 && (guint64) age < ttl_ms;
}

gboolean
ar_store_group_touch(ArStore *store, const gchar *group, GError **error)
{
  gchar *key;
  gchar *val;
  gboolean ok;

  key = g_strdup_printf("group.%s.fetched_at", group);
  val = g_strdup_printf("%" G_GINT64_FORMAT, g_get_real_time() / 1000);
  ok = ar_store_meta_set(store, key, val, error);
  g_free(key);
  g_free(val);
  return ok;
}

gboolean
ar_store_group_invalidate(ArStore *store, const gchar *group, GError **error)
{
  gchar *key;
  gboolean ok;

  key = g_strdup_printf("group.%s.fetched_at", group);
  ok = ar_store_meta_set(store, key, "0", error);
  g_free(key);
  return ok;
}

gboolean
ar_store_rate_acquire(ArStore *store,
		      const gchar *kind,
		      guint max_per_minute,
		      GError **error)
{
  ArRateEntry *e;
  gint64 now = g_get_real_time() / 1000;

  if (max_per_minute == 0)
    return TRUE;

  if (kind == NULL || *kind == '\0')
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		  "rate limit: missing kind");
      return FALSE;
    }

  e = g_hash_table_lookup(store->rate, kind);
  if (e == NULL)
    {
      e = g_new0(ArRateEntry, 1);
      e->window_start = now;
      e->calls = 0;
      g_hash_table_insert(store->rate, g_strdup(kind), e);
    }

  if (now - e->window_start >= 60 * 1000)
    {
      e->window_start = now;
      e->calls = 0;
    }

  if (e->calls >= (gint64) max_per_minute)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
		  "rate limit (%s): max %u call(s) per minute",
		  kind, max_per_minute);
      return FALSE;
    }

  e->calls++;
  return TRUE;
}

gboolean
ar_store_history_append(ArStore *store,
			const gchar *kind,
			const gchar *json_payload,
			GError **error)
{
  (void) store;
  (void) kind;
  (void) json_payload;
  (void) error;
  return TRUE;
}

gboolean
ar_store_history_prune(ArStore *store,
		       guint64 retain_ms,
		       guint64 max_bytes,
		       GError **error)
{
  (void) store;
  (void) retain_ms;
  (void) max_bytes;
  (void) error;
  return TRUE;
}
