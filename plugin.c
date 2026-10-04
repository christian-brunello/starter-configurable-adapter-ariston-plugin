/*
 * starter-configurable-adapter - ariston-remotethermo plugin
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "starter/configurable-adapter/plugin.h"

#include "ariston.h"
#include "api.h"

#define LOGI(msg...) g_log("ariston-remotethermo", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log("ariston-remotethermo", G_LOG_LEVEL_WARNING, msg)

#define STCA_TYPE_ARISTON (stca_ariston_get_type())
G_DECLARE_FINAL_TYPE(STCAAriston, stca_ariston, STCA, ARISTON, STCAPlugin)

struct _STCAAriston {
  STCAPlugin parent_instance;

  ArOptions opt;
  ArContext ctx;

  guint timer_id;

  /* Dedicated cloud thread: owns SoupSession / ArContext I/O. */
  GThread *cloud_thread;
  GMainContext *cloud_ctx;
  GMainLoop *cloud_loop;
  gboolean cloud_started;
  gboolean ctx_open;

  GMutex state_mutex;
  GCond start_cond;
  gboolean start_done;
  GError *start_error;

  /* Poll coalescing (main + cloud). */
  gboolean poll_queued;
  gboolean poll_inflight;
  gboolean poll_wanted;
};

G_DEFINE_TYPE(STCAAriston, stca_ariston, STCA_TYPE_PLUGIN)

/* ---- XML helpers ---- */

static gchar *
xml_child_text(xmlNodePtr node, const char *name)
{
  xmlNodePtr child;

  for (child = node->children; child != NULL; child = child->next)
    {
      xmlChar *content;
      gchar *out;

      if (child->type != XML_ELEMENT_NODE)
	continue;
      if (xmlStrcmp(child->name, (const xmlChar *) name) != 0)
	continue;

      content = xmlNodeGetContent(child);
      if (content == NULL)
	return NULL;
      out = g_strdup((const char *) content);
      xmlFree(content);
      g_strstrip(out);
      return out;
    }

  return NULL;
}

static gboolean
xml_child_duration(xmlNodePtr node, const char *name, guint64 *out_ms, GError **error)
{
  gchar *text;
  gboolean ok;

  text = xml_child_text(node, name);
  if (text == NULL)
    return TRUE;

  ok = ar_parse_duration_ms(text, out_ms, FALSE, error);
  g_free(text);
  return ok;
}

static gboolean
xml_child_flag(xmlNodePtr node, const char *name, gboolean *out)
{
  gchar *text;

  text = xml_child_text(node, name);
  if (text == NULL)
    return TRUE;

  *out = !(g_strcmp0(text, "0") == 0
	   || g_ascii_strcasecmp(text, "false") == 0
	   || g_ascii_strcasecmp(text, "no") == 0
	   || *text == '\0');
  g_free(text);
  return TRUE;
}

static gboolean
stca_ariston_configure(STCAPlugin *plugin, xmlNodePtr node, GError **error)
{
  STCAAriston *self = STCA_ARISTON(plugin);
  gchar *tmp;
  guint64 poll_ms = 0;

  ar_options_clear(&self->opt);
  ar_options_init_defaults(&self->opt);

  self->opt.host = xml_child_text(node, "host");
  self->opt.user = xml_child_text(node, "user");
  self->opt.password = xml_child_text(node, "password");
  self->opt.gw = xml_child_text(node, "gw");

  if (self->opt.host == NULL || *self->opt.host == '\0'
      || self->opt.user == NULL || *self->opt.user == '\0'
      || self->opt.password == NULL || *self->opt.password == '\0'
      || self->opt.gw == NULL || *self->opt.gw == '\0')
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		  "ariston-remotethermo requires <host>, <user>, <password>, <gw>");
      return FALSE;
    }

  if (!xml_child_duration(node, "features-ttl", &self->opt.features_ttl_ms, error)
      || !xml_child_duration(node, "session-ttl", &self->opt.session_ttl_ms, error)
      || !xml_child_duration(node, "metering-ttl", &self->opt.metering_ttl_ms, error))
    return FALSE;

  tmp = xml_child_text(node, "poll-interval");
  if (tmp != NULL)
    {
      if (!ar_parse_duration_ms(tmp, &poll_ms, FALSE, error))
	{
	  g_free(tmp);
	  return FALSE;
	}
      g_free(tmp);
      if (poll_ms > G_MAXUINT)
	{
	  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		      "poll-interval too large");
	  return FALSE;
	}
      self->opt.poll_interval_ms = (guint) poll_ms;
    }

  /* Live refresh follows the poll cadence; no separate live-ttl knobs. */
  self->opt.live_ttl_ms = self->opt.poll_interval_ms;

  xml_child_flag(node, "force", &self->opt.force);
  xml_child_flag(node, "verbose", &self->opt.verbose);

  LOGI("configured host=%s gw=%s poll=%ums metering-ttl=%" G_GUINT64_FORMAT "ms",
       self->opt.host, self->opt.gw, self->opt.poll_interval_ms,
       self->opt.metering_ttl_ms);
  return TRUE;
}

/* ---- Main-thread emit after cloud poll ---- */

typedef struct {
  STCAAriston *self;
  GHashTable *batch;
  GError *error;
} ArPollResult;

static void
ar_poll_result_free(ArPollResult *r)
{
  if (r == NULL)
    return;
  if (r->batch)
    g_hash_table_unref(r->batch);
  g_clear_error(&r->error);
  g_clear_object(&r->self);
  g_free(r);
}

static gboolean
ar_poll_emit_idle(gpointer user_data)
{
  ArPollResult *r = user_data;

  if (r->error != NULL)
    {
      LOGW("poll failed: %s", r->error->message);
    }
  else if (r->batch != NULL && g_hash_table_size(r->batch) > 0)
    {
      stca_plugin_emit_data_ready(STCA_PLUGIN(r->self), r->batch);
      LOGI("emitted %u value(s)", g_hash_table_size(r->batch));
    }

  ar_poll_result_free(r);
  return G_SOURCE_REMOVE;
}

/* ---- Cloud-thread jobs (serialized on cloud_ctx) ---- */

static void stca_ariston_schedule_poll(STCAAriston *self);

static gboolean
ar_cloud_run_poll(gpointer user_data)
{
  STCAAriston *self = user_data;
  ArPollResult *result;
  gboolean ok;
  gboolean again = FALSE;

  g_mutex_lock(&self->state_mutex);
  self->poll_queued = FALSE;
  self->poll_inflight = TRUE;
  g_mutex_unlock(&self->state_mutex);

  result = g_new0(ArPollResult, 1);
  result->self = g_object_ref(self);
  result->batch = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					(GDestroyNotify) stca_value_free);

  ok = ar_api_poll(&self->ctx, result->batch, &result->error);
  if (!ok)
    g_clear_pointer(&result->batch, g_hash_table_unref);

  g_mutex_lock(&self->state_mutex);
  self->poll_inflight = FALSE;
  if (self->poll_wanted)
    {
      self->poll_wanted = FALSE;
      again = TRUE;
    }
  g_mutex_unlock(&self->state_mutex);

  g_idle_add(ar_poll_emit_idle, result);

  if (again)
    stca_ariston_schedule_poll(self);

  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

typedef struct {
  STCAAriston *self;
  gchar *key;
  gdouble value;
  gboolean ok;
  GError *error;
  GMutex mutex;
  GCond cond;
  gboolean done;
} ArWriteJob;

static gboolean
ar_cloud_run_write(gpointer user_data)
{
  ArWriteJob *job = user_data;

  job->ok = ar_api_set_value(&job->self->ctx, job->key, job->value, &job->error);

  g_mutex_lock(&job->mutex);
  job->done = TRUE;
  g_cond_signal(&job->cond);
  g_mutex_unlock(&job->mutex);
  return G_SOURCE_REMOVE;
}

static gboolean
ar_cloud_quit(gpointer user_data)
{
  STCAAriston *self = user_data;

  if (self->cloud_loop != NULL)
    g_main_loop_quit(self->cloud_loop);
  return G_SOURCE_REMOVE;
}

static gpointer
ar_cloud_thread_func(gpointer user_data)
{
  STCAAriston *self = user_data;
  GError *error = NULL;

  g_main_context_push_thread_default(self->cloud_ctx);

  if (!ar_context_open(&self->ctx, &self->opt, &error))
    {
      g_mutex_lock(&self->state_mutex);
      self->start_error = error;
      self->ctx_open = FALSE;
      self->start_done = TRUE;
      g_cond_signal(&self->start_cond);
      g_mutex_unlock(&self->state_mutex);

      g_main_context_pop_thread_default(self->cloud_ctx);
      return NULL;
    }

  g_mutex_lock(&self->state_mutex);
  self->ctx_open = TRUE;
  self->start_done = TRUE;
  g_cond_signal(&self->start_cond);
  g_mutex_unlock(&self->state_mutex);

  self->cloud_loop = g_main_loop_new(self->cloud_ctx, FALSE);
  g_main_loop_run(self->cloud_loop);

  ar_context_close(&self->ctx);

  g_mutex_lock(&self->state_mutex);
  self->ctx_open = FALSE;
  g_mutex_unlock(&self->state_mutex);

  g_clear_pointer(&self->cloud_loop, g_main_loop_unref);
  g_main_context_pop_thread_default(self->cloud_ctx);
  return NULL;
}

static void
stca_ariston_schedule_poll(STCAAriston *self)
{
  g_return_if_fail(self != NULL);

  g_mutex_lock(&self->state_mutex);
  if (!self->ctx_open || self->cloud_ctx == NULL)
    {
      g_mutex_unlock(&self->state_mutex);
      return;
    }

  if (self->poll_queued || self->poll_inflight)
    {
      self->poll_wanted = TRUE;
      g_mutex_unlock(&self->state_mutex);
      return;
    }

  self->poll_queued = TRUE;
  g_mutex_unlock(&self->state_mutex);

  g_main_context_invoke_full(self->cloud_ctx,
			     G_PRIORITY_DEFAULT,
			     ar_cloud_run_poll,
			     g_object_ref(self),
			     g_object_unref);
}

static gboolean
stca_ariston_tick(gpointer user_data)
{
  stca_ariston_schedule_poll(STCA_ARISTON(user_data));
  return G_SOURCE_CONTINUE;
}

static gboolean
stca_ariston_start(STCAPlugin *plugin, GError **error)
{
  STCAAriston *self = STCA_ARISTON(plugin);

  if (self->cloud_started)
    return TRUE;

  self->cloud_ctx = g_main_context_new();
  self->start_done = FALSE;
  g_clear_error(&self->start_error);

  self->cloud_thread = g_thread_new("ariston-cloud", ar_cloud_thread_func, self);
  self->cloud_started = TRUE;

  g_mutex_lock(&self->state_mutex);
  while (!self->start_done)
    g_cond_wait(&self->start_cond, &self->state_mutex);

  if (self->start_error != NULL)
    {
      g_propagate_error(error, self->start_error);
      self->start_error = NULL;
      g_mutex_unlock(&self->state_mutex);

      /* Thread already exited after open failure; just join and drop context. */
      g_thread_join(self->cloud_thread);
      self->cloud_thread = NULL;
      g_clear_pointer(&self->cloud_ctx, g_main_context_unref);
      self->cloud_started = FALSE;
      return FALSE;
    }
  g_mutex_unlock(&self->state_mutex);

  if (self->timer_id == 0)
    self->timer_id = g_timeout_add(self->opt.poll_interval_ms, stca_ariston_tick, self);

  stca_ariston_schedule_poll(self);
  LOGI("plugin started (cloud thread; poll every %ums)", self->opt.poll_interval_ms);
  return TRUE;
}

static void
stca_ariston_shutdown_cloud(STCAAriston *self)
{
  if (self->timer_id != 0)
    {
      g_source_remove(self->timer_id);
      self->timer_id = 0;
    }

  if (!self->cloud_started)
    return;

  if (self->cloud_ctx != NULL)
    g_main_context_invoke(self->cloud_ctx, ar_cloud_quit, self);

  if (self->cloud_thread != NULL)
    {
      g_thread_join(self->cloud_thread);
      self->cloud_thread = NULL;
    }

  g_clear_pointer(&self->cloud_ctx, g_main_context_unref);
  self->cloud_started = FALSE;

  g_mutex_lock(&self->state_mutex);
  self->poll_queued = FALSE;
  self->poll_inflight = FALSE;
  self->poll_wanted = FALSE;
  g_mutex_unlock(&self->state_mutex);
}

static gboolean
stca_ariston_stop(STCAPlugin *plugin, GError **error)
{
  STCAAriston *self = STCA_ARISTON(plugin);

  (void) error;
  stca_ariston_shutdown_cloud(self);
  LOGI("plugin stopped");
  return TRUE;
}

static gboolean
stca_ariston_write_value(STCAPlugin *plugin, const gchar *key, gdouble value, GError **error)
{
  STCAAriston *self = STCA_ARISTON(plugin);
  ArWriteJob job;

  if (!self->cloud_started || self->cloud_ctx == NULL)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "plugin not started");
      return FALSE;
    }

  if (!ar_api_key_is_writable(key))
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		  "key `%s' is not writable", key);
      return FALSE;
    }

  memset(&job, 0, sizeof job);
  job.self = self;
  job.key = g_strdup(key);
  job.value = value;
  g_mutex_init(&job.mutex);
  g_cond_init(&job.cond);

  /* Serialized on the cloud context after any in-flight poll/write. */
  g_main_context_invoke(self->cloud_ctx, ar_cloud_run_write, &job);

  g_mutex_lock(&job.mutex);
  while (!job.done)
    g_cond_wait(&job.cond, &job.mutex);
  g_mutex_unlock(&job.mutex);

  g_free(job.key);
  g_mutex_clear(&job.mutex);
  g_cond_clear(&job.cond);

  if (!job.ok)
    {
      if (job.error != NULL)
	g_propagate_error(error, job.error);
      else
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "write failed");
      return FALSE;
    }

  LOGI("write %s=%.3f ok", key, value);
  stca_ariston_schedule_poll(self);
  return TRUE;
}

static void
stca_ariston_dispose(GObject *object)
{
  STCAAriston *self = STCA_ARISTON(object);

  stca_ariston_shutdown_cloud(self);
  ar_options_clear(&self->opt);
  g_clear_error(&self->start_error);

  G_OBJECT_CLASS(stca_ariston_parent_class)->dispose(object);
}

static void
stca_ariston_finalize(GObject *object)
{
  STCAAriston *self = STCA_ARISTON(object);

  g_cond_clear(&self->start_cond);
  g_mutex_clear(&self->state_mutex);
  G_OBJECT_CLASS(stca_ariston_parent_class)->finalize(object);
}

static void
stca_ariston_class_init(STCAAristonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  STCAPluginClass *plugin_class = STCA_PLUGIN_CLASS(klass);

  object_class->dispose = stca_ariston_dispose;
  object_class->finalize = stca_ariston_finalize;

  plugin_class->configure = stca_ariston_configure;
  plugin_class->start = stca_ariston_start;
  plugin_class->stop = stca_ariston_stop;
  plugin_class->write_value = stca_ariston_write_value;
}

static void
stca_ariston_init(STCAAriston *self)
{
  ar_options_init_defaults(&self->opt);
  self->timer_id = 0;
  self->cloud_thread = NULL;
  self->cloud_ctx = NULL;
  self->cloud_loop = NULL;
  self->cloud_started = FALSE;
  self->ctx_open = FALSE;
  self->start_done = FALSE;
  self->start_error = NULL;
  self->poll_queued = FALSE;
  self->poll_inflight = FALSE;
  self->poll_wanted = FALSE;
  g_mutex_init(&self->state_mutex);
  g_cond_init(&self->start_cond);
}

G_MODULE_EXPORT STCAPluginInfo *
stca_plugin_get_info(void)
{
  return stca_plugin_info_new("ariston-remotethermo", "1.0.0");
}

G_MODULE_EXPORT GObject *
stca_plugin_create(void)
{
  return g_object_new(STCA_TYPE_ARISTON, NULL);
}
