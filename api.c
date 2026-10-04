/*
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "api.h"
#include "http.h"
#include "store.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <json-glib/json-glib.h>

#include "starter/configurable-adapter/plugin.h"

#define AR_GROUP_LIVE "live"
#define AR_GROUP_FEATURES "features"
#define AR_GROUP_SESSION "session"
#define AR_GROUP_METERING "metering"

/* ConsumptionType (python-ariston-api) + provisional k24/k25 from web portal. */
typedef struct {
  int k;
  const char *prefix; /* snapshot key prefix, e.g. Metering_ChGas */
} ArMeterKind;

static const ArMeterKind ar_meter_kinds[] = {
  { 1, "Metering_ChTotal" },
  { 2, "Metering_DhwTotal" },
  { 7, "Metering_ChGas" },
  { 10, "Metering_DhwGas" },
  { 20, "Metering_ChElec" },
  { 21, "Metering_DhwElec" },
  { 24, "Metering_ChK24" }, /* provisional; not in official enum */
  { 25, "Metering_DhwK25" },
};

typedef struct {
  const gchar *name;
  const gchar *param_id;
  gboolean plant_mode; /* ST 0/1/2 ↔ cloud 0/1/5 */
} ArWritable;

static const ArWritable ar_writables[] = {
  { "ChFlowSetpointTemp", "U6_3_0_0", FALSE },
  { "HybridMode", "U6_12_1", FALSE },
  { "PlantMode", "U3", TRUE },
};

static gdouble
ar_plant_mode_to_st(gdouble cloud)
{
  switch ((int) cloud)
    {
    case 0:
    case 1:
      return cloud;
    case 5:
      return 2.0;
    default:
      return cloud;
    }
}

static gdouble
ar_plant_mode_from_st(gdouble st)
{
  switch ((int) st)
    {
    case 0:
    case 1:
      return st;
    case 2:
      return 5.0;
    default:
      return st;
    }
}

/* Setup (login / features) is infrequent and not rate-limited. Live,
 * metering and set each have their own per-kind budget (see ArOptions). */
static gboolean
ar_cloud_call(ArContext *ctx,
	      const gchar *method,
	      const gchar *url,
	      GBytes *body,
	      const gchar *content_type,
	      ArHttpResponse *out,
	      GError **error)
{
  ar_log(G_LOG_LEVEL_INFO, "%s %s", method, url);
  return ar_http_request(ctx->http, method, url, ctx->headers, body, content_type, out, error);
}

/* History was disk-backed in the CLI tool; the STCA plugin keeps no archive. */
static void
ar_maybe_history(ArContext *ctx, gboolean enabled, const gchar *kind, const gchar *json)
{
  (void) ctx;
  (void) enabled;
  (void) kind;
  (void) json;
}

static void
ar_maybe_history_bytes(ArContext *ctx, gboolean enabled, const gchar *kind, GBytes *bytes)
{
  (void) ctx;
  (void) enabled;
  (void) kind;
  (void) bytes;
}

static gboolean
ar_parse_json_bytes(GBytes *bytes, JsonParser **out_parser, JsonNode **out_root, GError **error)
{
  JsonParser *parser;
  gconstpointer data;
  gsize len = 0;

  parser = json_parser_new();
  data = g_bytes_get_data(bytes, &len);
  if (data == NULL || len == 0)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "empty JSON response");
      g_object_unref(parser);
      return FALSE;
    }

  if (!json_parser_load_from_data(parser, data, (gssize) len, error))
    {
      g_object_unref(parser);
      return FALSE;
    }

  *out_parser = parser;
  *out_root = json_parser_get_root(parser);
  return TRUE;
}

static gboolean
ar_http_ok(ArHttpResponse *rsp, GError **error)
{
  if (rsp->status < 200 || rsp->status >= 300)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		  "HTTP status %u", rsp->status);
      return FALSE;
    }
  return TRUE;
}

static gboolean
ar_http_needs_relogin(guint status)
{
  return status == 401 || status == 403 || status == 405;
}

static gboolean
ar_api_ensure_session_ex(ArContext *ctx, gboolean force_login, GError **error)
{
  ArHttpResponse rsp = { 0 };
  JsonParser *parser = NULL;
  JsonNode *root = NULL;
  GString *url;
  GString *body;
  GBytes *bytes;
  gboolean ok = FALSE;
  gboolean login_ok = FALSE;

  if (!force_login
      && ar_store_group_fresh(ctx->store, AR_GROUP_SESSION, ctx->opt->session_ttl_ms, NULL)
      && ar_http_has_usable_session(ctx->http))
    return TRUE;

  url = g_string_new(ctx->opt->host);
  g_string_append(url, "/R2/Account/Login?returnUrl=%2FR2%2FHome");

  {
    JsonBuilder *jb = json_builder_new();
    JsonGenerator *jg;
    JsonNode *jn;
    gchar *json;

    json_builder_begin_object(jb);
    json_builder_set_member_name(jb, "email");
    json_builder_add_string_value(jb, ctx->opt->user);
    json_builder_set_member_name(jb, "password");
    json_builder_add_string_value(jb, ctx->opt->password);
    /* Persistent cookies so SoupCookieJarText can store them across
     * process-transport respawns (session cookies are not written to disk). */
    json_builder_set_member_name(jb, "rememberMe");
    json_builder_add_boolean_value(jb, TRUE);
    json_builder_set_member_name(jb, "language");
    json_builder_add_string_value(jb, "Italian");
    json_builder_end_object(jb);
    jn = json_builder_get_root(jb);
    jg = json_generator_new();
    json_generator_set_root(jg, jn);
    json = json_generator_to_data(jg, NULL);
    body = g_string_new(json);
    g_free(json);
    g_object_unref(jg);
    json_node_unref(jn);
    g_object_unref(jb);
  }
  bytes = g_bytes_new(body->str, body->len);

  if (!ar_cloud_call(ctx, "POST", url->str, bytes,
		     "application/json; charset=utf-8", &rsp, error))
    goto out;

  if (!ar_http_ok(&rsp, error))
    goto out;

  if (!ar_parse_json_bytes(rsp.body, &parser, &root, error))
    goto out;

  if (!JSON_NODE_HOLDS_OBJECT(root))
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "login: expected JSON object");
      goto out;
    }

  {
    JsonObject *obj = json_node_get_object(root);

    if (json_object_has_member(obj, "ok"))
      login_ok = json_object_get_boolean_member(obj, "ok");
  }

  if (!login_ok)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "login rejected (ok=false)");
      goto out;
    }

  /* Belt-and-suspenders: even if the server ignores rememberMe, force Max-Age
   * so the text jar keeps auth cookies for the next short-lived process. */
  {
    gint max_age = (gint) (ctx->opt->session_ttl_ms / 1000);

    if (max_age < 60)
      max_age = 12 * 3600;
    ar_http_persist_session_cookies(ctx->http, max_age);
  }

  if (!ar_store_group_touch(ctx->store, AR_GROUP_SESSION, error))
    goto out;

  ok = TRUE;

out:
  if (parser)
    g_object_unref(parser);
  ar_http_response_clear(&rsp);
  g_bytes_unref(bytes);
  g_string_free(url, TRUE);
  g_string_free(body, TRUE);
  return ok;
}

gboolean
ar_api_ensure_session(ArContext *ctx, GError **error)
{
  return ar_api_ensure_session_ex(ctx, FALSE, error);
}

static gboolean
ar_invalidate_auth_cache(ArContext *ctx)
{
  GError *local = NULL;

  if (!ar_store_group_invalidate(ctx->store, AR_GROUP_SESSION, &local))
    {
      ar_log(G_LOG_LEVEL_WARNING, "session invalidate failed: %s",
	     local ? local->message : "?");
      g_clear_error(&local);
      return FALSE;
    }
  if (!ar_store_group_invalidate(ctx->store, AR_GROUP_FEATURES, &local))
    {
      ar_log(G_LOG_LEVEL_WARNING, "features invalidate failed: %s",
	     local ? local->message : "?");
      g_clear_error(&local);
      return FALSE;
    }
  if (!ar_store_group_invalidate(ctx->store, AR_GROUP_METERING, &local))
    {
      ar_log(G_LOG_LEVEL_WARNING, "metering invalidate failed: %s",
	     local ? local->message : "?");
      g_clear_error(&local);
      return FALSE;
    }
  (void) ar_store_meta_set(ctx->store, "features_json", "", NULL);
  (void) ar_store_meta_set(ctx->store, "metering_json", "", NULL);
  return TRUE;
}

gboolean
ar_api_ensure_features(ArContext *ctx, GError **error)
{
  ArHttpResponse rsp = { 0 };
  JsonParser *parser = NULL;
  JsonNode *root = NULL;
  GString *url = NULL;
  gchar *features_json = NULL;
  gboolean ok = FALSE;
  guint attempt;

  for (attempt = 0; attempt < 2; attempt++)
    {
      if (error && *error)
	g_clear_error(error);
      ar_http_response_clear(&rsp);
      if (parser)
	{
	  g_object_unref(parser);
	  parser = NULL;
	  root = NULL;
	}
      g_clear_pointer(&features_json, g_free);
      if (url)
	{
	  g_string_free(url, TRUE);
	  url = NULL;
	}

      if (!ar_api_ensure_session_ex(ctx, attempt > 0, error))
	return FALSE;

      if (!ctx->opt->force
	  && ar_store_group_fresh(ctx->store, AR_GROUP_FEATURES, ctx->opt->features_ttl_ms, NULL))
	{
	  if (ar_store_meta_get(ctx->store, "features_json", &features_json, NULL, NULL)
	      && features_json != NULL && *features_json != '\0')
	    {
	      g_free(features_json);
	      return TRUE;
	    }
	  g_free(features_json);
	  features_json = NULL;
	}

      url = g_string_new(ctx->opt->host);
      g_string_append_printf(url,
			     "/api/v2/remote/plants/%s/features?eagerMode=True",
			     ctx->opt->gw);

      if (!ar_cloud_call(ctx, "GET", url->str, NULL, NULL, &rsp, error))
	return FALSE;

      if (!ar_http_ok(&rsp, error))
	{
	  if (attempt == 0 && ar_http_needs_relogin(rsp.status))
	    {
	      ar_log(G_LOG_LEVEL_WARNING,
		     "features HTTP %u; re-login and retry", rsp.status);
	      ar_invalidate_auth_cache(ctx);
	      continue;
	    }
	  goto out;
	}

      if (!ar_parse_json_bytes(rsp.body, &parser, &root, error))
	goto out;

      if (!JSON_NODE_HOLDS_OBJECT(root))
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		      "features: expected JSON object");
	  goto out;
	}

      {
	JsonObject *obj = json_node_get_object(root);
	JsonNode *gw_node = json_node_new(JSON_NODE_VALUE);

	json_node_set_string(gw_node, ctx->opt->gw);
	json_object_set_member(obj, "gatewayId", gw_node);
	features_json = json_to_string(root, FALSE);
      }

      if (!ar_store_meta_set(ctx->store, "features_json", features_json, error))
	goto out;
      if (!ar_store_group_touch(ctx->store, AR_GROUP_FEATURES, error))
	goto out;

      ar_maybe_history(ctx, FALSE, "features", features_json);
      ok = TRUE;
      break;
    }

out:
  g_free(features_json);
  if (parser)
    g_object_unref(parser);
  ar_http_response_clear(&rsp);
  if (url)
    g_string_free(url, TRUE);
  return ok;
}

static void
ar_live_item_cb(JsonArray *array, guint index_, JsonNode *element_node, gpointer user_data)
{
  ArContext *ctx = user_data;
  JsonObject *obj;
  const gchar *id;
  gdouble value;
  GError *local = NULL;

  (void) array;
  (void) index_;

  if (!JSON_NODE_HOLDS_OBJECT(element_node))
    return;

  obj = json_node_get_object(element_node);
  if (!json_object_has_member(obj, "id") || !json_object_has_member(obj, "value"))
    return;

  id = json_object_get_string_member(obj, "id");
  value = json_object_get_double_member(obj, "value");

  if (g_strcmp0(id, "PlantMode") == 0)
    value = ar_plant_mode_to_st(value);

  if (!ar_store_snapshot_set(ctx->store, AR_GROUP_LIVE, id, value, &local))
    {
      ar_log(G_LOG_LEVEL_WARNING, "snapshot set %s failed: %s",
	     id, local ? local->message : "?");
      g_clear_error(&local);
    }
}

gboolean
ar_api_refresh_live(ArContext *ctx, GError **error)
{
  ArHttpResponse rsp = { 0 };
  JsonParser *parser = NULL;
  JsonNode *root = NULL;
  GString *url = NULL;
  GString *body = NULL;
  GBytes *bytes = NULL;
  gchar *features = NULL;
  gboolean ok = FALSE;
  guint attempt;

  /* One live budget slot per refresh, even if we re-login and retry. */
  if (!ar_store_rate_acquire(ctx->store, AR_RATE_LIVE,
			     ctx->opt->live_max_calls_per_minute, error))
    return FALSE;

  for (attempt = 0; attempt < 2; attempt++)
    {
      if (error && *error)
	g_clear_error(error);
      ar_http_response_clear(&rsp);
      if (parser)
	{
	  g_object_unref(parser);
	  parser = NULL;
	  root = NULL;
	}
      g_clear_pointer(&features, g_free);
      if (bytes)
	{
	  g_bytes_unref(bytes);
	  bytes = NULL;
	}
      if (body)
	{
	  g_string_free(body, TRUE);
	  body = NULL;
	}
      if (url)
	{
	  g_string_free(url, TRUE);
	  url = NULL;
	}

      if (!ar_api_ensure_features(ctx, error))
	return FALSE;

      if (!ar_store_meta_get(ctx->store, "features_json", &features, NULL, error))
	return FALSE;
      if (features == NULL || *features == '\0')
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "features_json missing");
	  g_free(features);
	  return FALSE;
	}

      url = g_string_new(ctx->opt->host);
      g_string_append_printf(url, "/R2/PlantHome/GetData/%s?umsys=si", ctx->opt->gw);

      body = g_string_new(NULL);
      g_string_append_printf(body,
			     "{\"features\": %s, "
			     "\"filter\":{\"notEssentials\":true,\"plant\":true,\"zone\":true,"
			     "\"dhw\":true,\"useCache\":false,\"zone\":1}}",
			     features);
      bytes = g_bytes_new(body->str, body->len);

      if (!ar_cloud_call(ctx, "POST", url->str, bytes,
			 "application/json; charset=utf-8", &rsp, error))
	goto out;

      if (!ar_http_ok(&rsp, error))
	{
	  if (attempt == 0 && ar_http_needs_relogin(rsp.status))
	    {
	      ar_log(G_LOG_LEVEL_WARNING,
		     "GetData HTTP %u; re-login and retry", rsp.status);
	      ar_invalidate_auth_cache(ctx);
	      continue;
	    }
	  goto out;
	}

      if (!ar_parse_json_bytes(rsp.body, &parser, &root, error))
	goto out;

      if (!JSON_NODE_HOLDS_OBJECT(root))
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "GetData: expected object");
	  goto out;
	}

      {
	JsonObject *obj = json_node_get_object(root);
	JsonObject *data;
	JsonArray *items;

	if (!json_object_has_member(obj, "data"))
	  {
	    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "GetData: missing data");
	    goto out;
	  }

	data = json_object_get_object_member(obj, "data");
	if (data == NULL || !json_object_has_member(data, "items"))
	  {
	    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "GetData: missing items");
	    goto out;
	  }

	items = json_object_get_array_member(data, "items");
	if (!ar_store_snapshot_clear_group(ctx->store, AR_GROUP_LIVE, error))
	  goto out;

	json_array_foreach_element(items, ar_live_item_cb, ctx);
      }

      if (!ar_store_group_touch(ctx->store, AR_GROUP_LIVE, error))
	goto out;

      ar_maybe_history_bytes(ctx, FALSE, "live", rsp.body);
      ok = TRUE;
      break;
    }

out:
  g_free(features);
  if (parser)
    g_object_unref(parser);
  ar_http_response_clear(&rsp);
  if (bytes)
    g_bytes_unref(bytes);
  if (url)
    g_string_free(url, TRUE);
  if (body)
    g_string_free(body, TRUE);
  return ok;
}

static const ArWritable *
ar_find_writable(const gchar *name)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS(ar_writables); i++)
    {
      if (g_strcmp0(ar_writables[i].name, name) == 0)
	return &ar_writables[i];
    }
  return NULL;
}

static gboolean
ar_get_param_cloud(ArContext *ctx, const gchar *param_id, gdouble *out, GError **error)
{
  ArHttpResponse rsp = { 0 };
  JsonParser *parser = NULL;
  JsonNode *root = NULL;
  GString *url = NULL;
  gboolean ok = FALSE;
  guint attempt;

  for (attempt = 0; attempt < 2; attempt++)
    {
      if (error && *error)
	g_clear_error(error);
      ar_http_response_clear(&rsp);
      if (parser)
	{
	  g_object_unref(parser);
	  parser = NULL;
	  root = NULL;
	}
      if (url)
	{
	  g_string_free(url, TRUE);
	  url = NULL;
	}

      if (attempt > 0)
	{
	  ar_invalidate_auth_cache(ctx);
	  if (!ar_api_ensure_session_ex(ctx, TRUE, error))
	    return FALSE;
	}

      url = g_string_new(ctx->opt->host);
      g_string_append_printf(url, "/R2/PlantMenu/Refresh?id=%s&paramIds=%s",
			     ctx->opt->gw, param_id);

      /* Official web client / chaffolink use POST (empty body). */
      if (!ar_cloud_call(ctx, "POST", url->str, NULL, NULL, &rsp, error))
	return FALSE;

      if (!ar_http_ok(&rsp, error))
	{
	  if (attempt == 0 && ar_http_needs_relogin(rsp.status))
	    {
	      ar_log(G_LOG_LEVEL_WARNING,
		     "Refresh HTTP %u; re-login and retry", rsp.status);
	      continue;
	    }
	  goto out;
	}

      if (!ar_parse_json_bytes(rsp.body, &parser, &root, error))
	goto out;

      /* Response: { "data": [ { "id":"U6_…", "value": … }, … ] } */
      if (JSON_NODE_HOLDS_OBJECT(root))
	{
	  JsonObject *obj = json_node_get_object(root);
	  JsonNode *data_node = json_object_get_member(obj, "data");
	  JsonArray *data;
	  JsonObject *item;

	  if (data_node == NULL || !JSON_NODE_HOLDS_ARRAY(data_node))
	    {
	      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			  "Refresh: unexpected JSON for %s", param_id);
	      goto out;
	    }

	  data = json_node_get_array(data_node);
	  if (json_array_get_length(data) < 1)
	    {
	      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			  "Refresh: empty data for %s", param_id);
	      goto out;
	    }

	  item = json_array_get_object_element(data, 0);
	  if (item == NULL || !json_object_has_member(item, "value"))
	    {
	      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			  "Refresh: no value for %s", param_id);
	      goto out;
	    }

	  *out = json_object_get_double_member(item, "value");
	  ok = TRUE;
	  break;
	}

      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		  "Refresh: expected object for %s", param_id);
      goto out;
    }

out:
  if (parser)
    g_object_unref(parser);
  ar_http_response_clear(&rsp);
  if (url)
    g_string_free(url, TRUE);
  return ok;
}

static gboolean
ar_value_has_fraction(gdouble value)
{
  double ip;

  return fabs(modf(value, &ip)) > 1e-9;
}

gboolean
ar_api_set_value(ArContext *ctx, const gchar *name, gdouble value, GError **error)
{
  const ArWritable *w;
  ArHttpResponse rsp = { 0 };
  JsonParser *parser = NULL;
  JsonNode *root = NULL;
  GString *url = NULL;
  GString *body = NULL;
  GBytes *bytes = NULL;
  gdouble prev_cloud = 0;
  gdouble new_cloud;
  gboolean ok = FALSE;
  guint attempt;

  w = ar_find_writable(name);
  if (w == NULL)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		  "unsupported set key `%s' (use ChFlowSetpointTemp|HybridMode|PlantMode)",
		  name);
      return FALSE;
    }

  /* One set budget slot per writable (Refresh+Submit share it; independent of live). */
  if (!ar_store_rate_acquire(ctx->store, AR_RATE_SET,
			     ctx->opt->set_max_calls_per_minute, error))
    return FALSE;

  if (!ar_api_ensure_features(ctx, error))
    return FALSE;

  if (!ar_get_param_cloud(ctx, w->param_id, &prev_cloud, error))
    return FALSE;

  new_cloud = w->plant_mode ? ar_plant_mode_from_st(value) : value;

  for (attempt = 0; attempt < 2; attempt++)
    {
      gboolean submit_ok = FALSE;

      if (error && *error)
	g_clear_error(error);
      ar_http_response_clear(&rsp);
      if (parser)
	{
	  g_object_unref(parser);
	  parser = NULL;
	  root = NULL;
	}
      if (bytes)
	{
	  g_bytes_unref(bytes);
	  bytes = NULL;
	}
      if (body)
	{
	  g_string_free(body, TRUE);
	  body = NULL;
	}
      if (url)
	{
	  g_string_free(url, TRUE);
	  url = NULL;
	}

      if (attempt > 0)
	{
	  ar_invalidate_auth_cache(ctx);
	  if (!ar_api_ensure_features(ctx, error))
	    return FALSE;
	}

      url = g_string_new(ctx->opt->host);
      g_string_append_printf(url, "/R2/PlantMenu/Submit/%s?userActivity=SaveOtherSettings",
			     ctx->opt->gw);

      body = g_string_new(NULL);
      if (ar_value_has_fraction(new_cloud) || ar_value_has_fraction(prev_cloud))
	g_string_append_printf(body,
			       "[{\"id\":\"%s\",\"value\":\"%.2f\",\"prevValue\":%.2f}]",
			       w->param_id, new_cloud, prev_cloud);
      else
	g_string_append_printf(body,
			       "[{\"id\":\"%s\",\"value\":\"%d\",\"prevValue\":%d}]",
			       w->param_id, (int) new_cloud, (int) prev_cloud);

      bytes = g_bytes_new(body->str, body->len);

      if (!ar_cloud_call(ctx, "POST", url->str, bytes,
			 "application/json; charset=utf-8", &rsp, error))
	goto out;

      if (!ar_http_ok(&rsp, error))
	{
	  if (attempt == 0 && ar_http_needs_relogin(rsp.status))
	    {
	      ar_log(G_LOG_LEVEL_WARNING,
		     "Submit HTTP %u; re-login and retry", rsp.status);
	      continue;
	    }
	  goto out;
	}

      if (!ar_parse_json_bytes(rsp.body, &parser, &root, error))
	goto out;

      if (JSON_NODE_HOLDS_OBJECT(root))
	{
	  JsonObject *obj = json_node_get_object(root);

	  if (json_object_has_member(obj, "ok"))
	    submit_ok = json_object_get_boolean_member(obj, "ok");
	}

      if (!submit_ok)
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Submit ok=false for %s", name);
	  goto out;
	}

      /* Invalidate live cache so next get refreshes. */
      (void) ar_store_meta_set(ctx->store, "group.live.fetched_at", "0", NULL);
      (void) ar_store_snapshot_set(ctx->store, AR_GROUP_LIVE, name, value, NULL);

      ok = TRUE;
      break;
    }

out:
  if (parser)
    g_object_unref(parser);
  ar_http_response_clear(&rsp);
  if (bytes)
    g_bytes_unref(bytes);
  if (url)
    g_string_free(url, TRUE);
  if (body)
    g_string_free(body, TRUE);
  return ok;
}

static const char *
ar_meter_prefix(int k)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS(ar_meter_kinds); i++)
    {
      if (ar_meter_kinds[i].k == k)
	return ar_meter_kinds[i].prefix;
    }
  return NULL;
}

static gdouble
ar_json_array_sum(JsonArray *v)
{
  guint i;
  guint n;
  gdouble sum = 0;

  n = json_array_get_length(v);
  for (i = 0; i < n; i++)
    sum += json_array_get_double_element(v, i);
  return sum;
}

static gboolean
ar_metering_apply_series(ArContext *ctx,
			 int k,
			 int p,
			 JsonArray *v,
			 GError **error)
{
  const char *prefix;
  gchar *key;
  guint n;
  gdouble last;
  gdouble sum;

  prefix = ar_meter_prefix(k);
  if (prefix == NULL || v == NULL)
    return TRUE; /* unknown k: ignore for now */

  n = json_array_get_length(v);
  if (n == 0)
    return TRUE;

  sum = ar_json_array_sum(v);
  last = json_array_get_double_element(v, n - 1);

  /* p=1: 24 × 2h slots (~48h). p=2: 7 days. p=4: long buckets (~year). */
  if (p == 1)
    {
      key = g_strdup_printf("%s_Last2h", prefix);
      if (!ar_store_snapshot_set(ctx->store, AR_GROUP_METERING, key, last, error))
	{
	  g_free(key);
	  return FALSE;
	}
      g_free(key);

      key = g_strdup_printf("%s_Last48h", prefix);
      if (!ar_store_snapshot_set(ctx->store, AR_GROUP_METERING, key, sum, error))
	{
	  g_free(key);
	  return FALSE;
	}
      g_free(key);
    }
  else if (p == 2)
    {
      key = g_strdup_printf("%s_Last7d", prefix);
      if (!ar_store_snapshot_set(ctx->store, AR_GROUP_METERING, key, sum, error))
	{
	  g_free(key);
	  return FALSE;
	}
      g_free(key);
    }
  else if (p == 4)
    {
      key = g_strdup_printf("%s_LongTerm", prefix);
      if (!ar_store_snapshot_set(ctx->store, AR_GROUP_METERING, key, sum, error))
	{
	  g_free(key);
	  return FALSE;
	}
      g_free(key);
    }

  return TRUE;
}

gboolean
ar_api_ensure_metering(ArContext *ctx, GError **error)
{
  ArHttpResponse rsp = { 0 };
  JsonParser *parser = NULL;
  JsonNode *root = NULL;
  GString *url = NULL;
  GBytes *body = NULL;
  gchar *raw_json = NULL;
  gboolean ok = FALSE;
  guint attempt;

  if (!ctx->opt->force
      && ar_store_group_fresh(ctx->store, AR_GROUP_METERING, ctx->opt->metering_ttl_ms, NULL))
    return TRUE;

  /* One metering budget slot per network refresh (independent of live/set). */
  if (!ar_store_rate_acquire(ctx->store, AR_RATE_METERING,
			     ctx->opt->metering_max_calls_per_minute, error))
    return FALSE;

  for (attempt = 0; attempt < 2; attempt++)
    {
      if (error && *error)
	g_clear_error(error);
      ar_http_response_clear(&rsp);
      if (parser)
	{
	  g_object_unref(parser);
	  parser = NULL;
	  root = NULL;
	}
      g_clear_pointer(&raw_json, g_free);
      if (body)
	{
	  g_bytes_unref(body);
	  body = NULL;
	}
      if (url)
	{
	  g_string_free(url, TRUE);
	  url = NULL;
	}

      if (!ar_api_ensure_session_ex(ctx, attempt > 0, error))
	return FALSE;

      url = g_string_new(ctx->opt->host);
      g_string_append_printf(url,
			     "/api/v2/remote/reports/%s/consSequencesApi8"
			     "?usages=Ch%%2CDhw&hasSlp=False&umSys=si",
			     ctx->opt->gw);

      body = g_bytes_new_static("{\"useCache\": false}", 19);

      if (!ar_cloud_call(ctx, "GET", url->str, body,
			 "application/json; charset=utf-8", &rsp, error))
	return FALSE;

      if (!ar_http_ok(&rsp, error))
	{
	  if (attempt == 0 && ar_http_needs_relogin(rsp.status))
	    {
	      ar_log(G_LOG_LEVEL_WARNING,
		     "metering HTTP %u; re-login and retry", rsp.status);
	      ar_invalidate_auth_cache(ctx);
	      continue;
	    }
	  goto out;
	}

      if (!ar_parse_json_bytes(rsp.body, &parser, &root, error))
	goto out;

      if (!JSON_NODE_HOLDS_ARRAY(root))
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		      "metering: expected JSON array");
	  goto out;
	}

      {
	JsonArray *arr = json_node_get_array(root);
	guint i;
	guint n = json_array_get_length(arr);

	if (!ar_store_snapshot_clear_group(ctx->store, AR_GROUP_METERING, error))
	  goto out;

	for (i = 0; i < n; i++)
	  {
	    JsonNode *el = json_array_get_element(arr, i);
	    JsonObject *obj;
	    JsonArray *v;
	    int k, p;

	    if (!JSON_NODE_HOLDS_OBJECT(el))
	      continue;
	    obj = json_node_get_object(el);
	    if (!json_object_has_member(obj, "k")
		|| !json_object_has_member(obj, "p")
		|| !json_object_has_member(obj, "v"))
	      continue;
	    if (!JSON_NODE_HOLDS_ARRAY(json_object_get_member(obj, "v")))
	      continue;

	    k = json_object_get_int_member(obj, "k");
	    p = json_object_get_int_member(obj, "p");
	    v = json_object_get_array_member(obj, "v");

	    if (!ar_metering_apply_series(ctx, k, p, v, error))
	      goto out;
	  }
      }

      raw_json = json_to_string(root, FALSE);
      if (!ar_store_meta_set(ctx->store, "metering_json", raw_json, error))
	goto out;
      if (!ar_store_group_touch(ctx->store, AR_GROUP_METERING, error))
	goto out;

      ar_maybe_history(ctx, FALSE, "metering", raw_json);
      ok = TRUE;
      break;
    }

out:
  g_free(raw_json);
  if (parser)
    g_object_unref(parser);
  ar_http_response_clear(&rsp);
  if (body)
    g_bytes_unref(body);
  if (url)
    g_string_free(url, TRUE);
  return ok;
}

/* ---- STCA poll helpers ---- */

gboolean
ar_api_key_is_writable(const gchar *name)
{
  return ar_find_writable(name) != NULL;
}

static void
ar_snapshot_to_stca_cb(const gchar *group,
		       const gchar *key,
		       gdouble value,
		       gint64 updated_at,
		       gpointer user_data)
{
  GHashTable *out = user_data;
  guint access = STCA_ACCESS_READ;

  (void) group;
  (void) updated_at;

  if (ar_api_key_is_writable(key))
    access |= STCA_ACCESS_WRITE;

  g_hash_table_replace(out, g_strdup(key), stca_value_new(value, access));
}

gboolean
ar_api_poll(ArContext *ctx, GHashTable *out, GError **error)
{
  gint64 age_ms = -1;

  g_return_val_if_fail(ctx != NULL, FALSE);
  g_return_val_if_fail(out != NULL, FALSE);

  if (ctx->opt->force
      || !ar_store_group_fresh(ctx->store, AR_GROUP_LIVE, ctx->opt->live_ttl_ms, &age_ms))
    {
      GError *refresh_error = NULL;

      if (!ar_api_refresh_live(ctx, &refresh_error))
	{
	  if (age_ms >= 0)
	    {
	      ar_log(G_LOG_LEVEL_WARNING, "live refresh failed (%s); serving cache",
		     refresh_error ? refresh_error->message : "?");
	      g_clear_error(&refresh_error);
	    }
	  else
	    {
	      g_propagate_error(error, refresh_error);
	      return FALSE;
	    }
	}
    }

  /* Metering is best-effort: live still succeeds if metering fails. */
  {
    GError *meter_error = NULL;
    gint64 meter_age = -1;
    gboolean meter_fresh = ar_store_group_fresh(ctx->store, AR_GROUP_METERING,
						ctx->opt->metering_ttl_ms, &meter_age);

    if (ctx->opt->force || !meter_fresh)
      {
	if (!ar_api_ensure_metering(ctx, &meter_error))
	  {
	    if (meter_age >= 0)
	      ar_log(G_LOG_LEVEL_WARNING, "metering refresh failed (%s); serving cache",
		     meter_error ? meter_error->message : "?");
	    else
	      ar_log(G_LOG_LEVEL_WARNING, "metering unavailable: %s",
		     meter_error ? meter_error->message : "?");
	    g_clear_error(&meter_error);
	  }
      }
  }

  if (!ar_store_snapshot_foreach(ctx->store, ar_snapshot_to_stca_cb, out, error))
    return FALSE;

  return TRUE;
}
