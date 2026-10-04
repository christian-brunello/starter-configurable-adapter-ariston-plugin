/*
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARISTON_HTTP_H
#define ARISTON_HTTP_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct ArHttp ArHttp;

typedef struct {
  guint status;
  GBytes *body; /* owned when set */
} ArHttpResponse;

void ar_http_response_clear(ArHttpResponse *r);

ArHttp *ar_http_new(const gchar *cookie_jar_path,
		    gboolean verbose,
		    GError **error);
void ar_http_free(ArHttp *http);

/* SoupCookieJarText does not store session cookies (no Expires/Max-Age).
 * Promote any such cookies so they survive process-transport respawns. */
void ar_http_persist_session_cookies(ArHttp *http, gint max_age_sec);

/* True if the jar has an auth-related cookie (not only ar.loggedUser). */
gboolean ar_http_has_usable_session(ArHttp *http);

gboolean ar_http_request(ArHttp *http,
			 const gchar *method,
			 const gchar *url,
			 GHashTable *headers,
			 GBytes *body,
			 const gchar *content_type,
			 ArHttpResponse *out,
			 GError **error);

G_END_DECLS

#endif /* ARISTON_HTTP_H */
