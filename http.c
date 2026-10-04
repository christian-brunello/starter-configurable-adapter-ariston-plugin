/*
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "http.h"
#include "ariston.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <gio/gio.h>
#include <libsoup/soup.h>

struct ArHttp {
  SoupSession *session;
  SoupCookieJar *jar;
};

void
ar_http_response_clear(ArHttpResponse *r)
{
  if (r == NULL)
    return;
  g_clear_pointer(&r->body, g_bytes_unref);
  r->status = 0;
}

static void
ar_soup_logger_printer(SoupLogger *logger,
		       SoupLoggerLogLevel level,
		       char direction,
		       const char *data,
		       gpointer user_data)
{
  struct timespec ts;
  struct tm tm;
  gchar stamp[64];

  (void) logger;
  (void) level;
  (void) user_data;

  stamp[0] = '\0';
  if (clock_gettime(CLOCK_REALTIME, &ts) == 0
      && localtime_r(&ts.tv_sec, &tm) != NULL)
    {
      snprintf(stamp, sizeof stamp,
	       "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
	       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	       tm.tm_hour, tm.tm_min, tm.tm_sec,
	       ts.tv_nsec / 1000000L);
    }

  /* Default SoupLogger printer uses stdout — never do that under
   * process-transport. direction is '>' request, '<' response, ' ' misc. */
  fprintf(stderr, "[%s] soup %c %s\n",
	  stamp[0] != '\0' ? stamp : "unknown-time",
	  direction,
	  data != NULL ? data : "");
  fflush(stderr);
}

ArHttp *
ar_http_new(const gchar *cookie_jar_path,
	    gboolean verbose,
	    GError **error)
{
  ArHttp *self;
  SoupCookieJar *jar;
  GProxyResolver *resolver;

  self = g_new0(ArHttp, 1);
  self->session = soup_session_new();

  /* Avoid GLibproxyResolver (libproxy), which can add multi-second stalls on
   * first HTTPS. Direct connections only — matches typical LAN/appliance use. */
  resolver = g_simple_proxy_resolver_new(NULL, NULL);
  soup_session_set_proxy_resolver(self->session, resolver);
  g_object_unref(resolver);

  if (cookie_jar_path != NULL && *cookie_jar_path != '\0')
    jar = soup_cookie_jar_text_new(cookie_jar_path, FALSE);
  else
    jar = soup_cookie_jar_new();

  if (jar == NULL)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		  "unable to create cookie jar at `%s'",
		  cookie_jar_path ? cookie_jar_path : "(memory)");
      g_clear_object(&self->session);
      g_free(self);
      return NULL;
    }

  soup_session_add_feature(self->session, SOUP_SESSION_FEATURE(jar));
  self->jar = jar;

  if (verbose)
    {
      /* Full BODY logging for local debugging (includes login JSON). */
      SoupLogger *logger = soup_logger_new(SOUP_LOGGER_LOG_BODY);

      soup_logger_set_printer(logger, ar_soup_logger_printer, NULL, NULL);
      soup_session_add_feature(self->session, SOUP_SESSION_FEATURE(logger));
      g_object_unref(logger);
    }

  return self;
}

void
ar_http_free(ArHttp *http)
{
  if (http == NULL)
    return;
  g_clear_object(&http->jar);
  g_clear_object(&http->session);
  g_free(http);
}

void
ar_http_persist_session_cookies(ArHttp *http, gint max_age_sec)
{
  GSList *all;
  GSList *l;
  guint promoted = 0;

  g_return_if_fail(http != NULL);
  if (max_age_sec <= 0)
    max_age_sec = 12 * 3600;

  all = soup_cookie_jar_all_cookies(http->jar);
  for (l = all; l != NULL; l = l->next)
    {
      SoupCookie *c = l->data;

      if (soup_cookie_get_expires(c) != NULL)
	continue;

      {
	SoupCookie *copy = soup_cookie_copy(c);

	soup_cookie_set_max_age(copy, max_age_sec);
	/* Replacing the cookie triggers SoupCookieJarText to rewrite the file. */
	soup_cookie_jar_add_cookie(http->jar, copy);
	promoted++;
      }
    }
  g_slist_free_full(all, (GDestroyNotify) soup_cookie_free);

  if (promoted > 0)
    ar_log(G_LOG_LEVEL_INFO,
	   "persisted %u session cookie(s) with max-age=%d",
	   promoted, max_age_sec);
}

gboolean
ar_http_has_usable_session(ArHttp *http)
{
  GSList *all;
  GSList *l;
  gboolean ok = FALSE;

  g_return_val_if_fail(http != NULL, FALSE);

  all = soup_cookie_jar_all_cookies(http->jar);
  for (l = all; l != NULL; l = l->next)
    {
      SoupCookie *c = l->data;
      const char *name = soup_cookie_get_name(c);

      /* Profile cookie alone is not enough for R2/api auth. */
      if (name != NULL && g_strcmp0(name, "ar.loggedUser") != 0)
	{
	  ok = TRUE;
	  break;
	}
    }
  g_slist_free_full(all, (GDestroyNotify) soup_cookie_free);
  return ok;
}

static SoupMessage *
ar_http_build_message(const gchar *method,
		      const gchar *url,
		      GHashTable *headers,
		      GBytes *body,
		      const gchar *content_type,
		      GError **error)
{
  SoupMessage *msg;

  msg = soup_message_new(method, url);
  if (msg == NULL)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		  "invalid HTTP URL or method: %s %s", method, url);
      return NULL;
    }

#if defined(HAVE_LIBSOUP_3)
  {
    SoupMessageHeaders *req_headers = soup_message_get_request_headers(msg);

    if (headers != NULL)
      {
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, headers);
	while (g_hash_table_iter_next(&iter, &key, &value))
	  soup_message_headers_replace(req_headers,
				       (const gchar *) key,
				       (const gchar *) value);
      }

    if (body != NULL)
      soup_message_set_request_body_from_bytes(msg, content_type, body);
  }
#else
  {
    if (headers != NULL)
      {
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, headers);
	while (g_hash_table_iter_next(&iter, &key, &value))
	  soup_message_headers_replace(msg->request_headers,
				       (const gchar *) key,
				       (const gchar *) value);
      }

    if (body != NULL)
      {
	gsize len = 0;
	gconstpointer data = g_bytes_get_data(body, &len);

	soup_message_set_request(msg,
				 content_type != NULL ? content_type : "application/octet-stream",
				 SOUP_MEMORY_COPY,
				 data,
				 len);
      }
  }
#endif

  return msg;
}

gboolean
ar_http_request(ArHttp *http,
		const gchar *method,
		const gchar *url,
		GHashTable *headers,
		GBytes *body,
		const gchar *content_type,
		ArHttpResponse *out,
		GError **error)
{
  SoupMessage *msg;

  g_return_val_if_fail(http != NULL, FALSE);
  g_return_val_if_fail(out != NULL, FALSE);

  memset(out, 0, sizeof(*out));

  msg = ar_http_build_message(method, url, headers, body, content_type, error);
  if (msg == NULL)
    return FALSE;

#if defined(HAVE_LIBSOUP_3)
  {
    GError *local_error = NULL;
    GBytes *response_body;

    response_body = soup_session_send_and_read(http->session, msg, NULL, &local_error);
    if (local_error != NULL)
      {
	g_propagate_error(error, local_error);
	g_object_unref(msg);
	return FALSE;
      }

    out->status = soup_message_get_status(msg);
    out->body = response_body;
  }
#else
  {
    guint status = soup_session_send_message(http->session, msg);

    out->status = status;
    if (msg->response_body != NULL && msg->response_body->data != NULL)
      out->body = g_bytes_new(msg->response_body->data, msg->response_body->length);
    else
      out->body = g_bytes_new_static("", 0);

    if (status == 0)
      {
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		    "HTTP request failed for %s %s", method, url);
	ar_http_response_clear(out);
	g_object_unref(msg);
	return FALSE;
      }
  }
#endif

  g_object_unref(msg);
  return TRUE;
}
