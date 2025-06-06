/*
OpenIO SDS event queue
Copyright (C) 2016-2020 OpenIO SAS, as part of OpenIO SDS
Copyright (C) 2022-2025 OVH SAS

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 3.0 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library.
*/

#include <string.h>

#include <glib.h>

#include <core/oio_core.h>
#include <core/url_ext.h>
#include <metautils/lib/metautils_resolv.h>

#include "beanstalkd.h"
#include "kafka.h"
#include "oio_events_queue.h"
#include "oio_events_queue_internals.h"
#include "oio_events_queue_shared.h"
#include "oio_events_queue_fanout.h"
#include "oio_events_queue_beanstalkd.h"
#include "oio_events_queue_kafka.h"
#include "oio_events_queue_kafka_sync.h"

#define EVTQ_CALL(self,F) VTABLE_CALL(self,struct oio_events_queue_abstract_s*,F)

void
oio_events_queue__destroy (struct oio_events_queue_s *self)
{
	if (!self) return;
	EVTQ_CALL(self,destroy)(self);
}

gboolean
oio_events_queue__send (struct oio_events_queue_s *self, gchar* key, gchar *msg)
{
	EXTRA_ASSERT (msg != NULL);
	if (event_fallback_installed() && oio_events_queue__is_stalled(self)) {
		struct _queue_with_endpoint_s *q = (struct _queue_with_endpoint_s*) self;
		_drop_event(q->queue_name, key, msg);
		return FALSE;
	}
	EVTQ_CALL(self, send)(self,key,msg);
}

void
oio_events_queue__flush_overwritable(struct oio_events_queue_s *self,
		gchar *tag)
{
	EXTRA_ASSERT (tag != NULL);
	if (VTABLE_HAS(self,struct oio_events_queue_abstract_s*,flush_overwritable)
			&& tag && *tag) {
		EVTQ_CALL(self,flush_overwritable)(self,tag);
	} else {
		g_free(tag);  // safe if key is NULL
	}
}

gboolean
oio_events_queue__send_overwritable(struct oio_events_queue_s *self,
		gchar *tag, gchar *msg)
{
	EXTRA_ASSERT (msg != NULL);
	if (VTABLE_HAS(self,struct oio_events_queue_abstract_s*,send_overwritable)
			&& tag && *tag) {
		EVTQ_CALL(self,send_overwritable)(self,tag,msg);
	} else {
		g_free(tag);  // safe if tag is NULL
		if (event_fallback_installed() && oio_events_queue__is_stalled(self)) {
			struct _queue_with_endpoint_s *q = (struct _queue_with_endpoint_s*) self;
			_drop_event(q->queue_name, NULL, msg);
			return FALSE;
		}
		EVTQ_CALL(self,send)(self, NULL, msg);
	}
}

gboolean
oio_events_queue__is_stalled (struct oio_events_queue_s *self)
{
	EVTQ_CALL(self,is_stalled)(self);
}

guint64
oio_events_queue__get_total_send_time(struct oio_events_queue_s *self)
{
	if (VTABLE_HAS(self,struct oio_events_queue_abstract_s*,get_total_send_time)) {
		EVTQ_CALL(self,get_total_send_time)(self);
	}
	return 0;
}

guint64
oio_events_queue__get_total_sent_events(struct oio_events_queue_s *self)
{
	if (VTABLE_HAS(self,struct oio_events_queue_abstract_s*,get_total_sent_events)) {
		EVTQ_CALL(self,get_total_sent_events)(self);
	}
	return 0;
}

gint64
oio_events_queue__get_health(struct oio_events_queue_s *self)
{
	if (VTABLE_HAS(self,struct oio_events_queue_abstract_s*,get_health)) {
		EVTQ_CALL(self,get_health)(self);
	}
	return 100;
}

void
oio_events_queue__set_buffering (struct oio_events_queue_s *self,
		gint64 delay)
{
	EVTQ_CALL(self,set_buffering)(self,delay);
}

GError *
oio_events_queue__start (struct oio_events_queue_s *self)
{
	EVTQ_CALL(self,start)(self);
}

static const char *
_has_prefix (const char *cfg, const char *prefix)
{
	if (g_str_has_prefix (cfg, prefix))
		return cfg + strlen(prefix);
	return NULL;
}

static GError *
_parse_and_create_multi(const char *cfg, const char *tube,
		const gboolean sync, struct oio_events_queue_s **out)
{
	gchar **tokens = g_strsplit(cfg, OIO_CSV_SEP2, -1);
	if (!tokens)
		return SYSERR("internal error");

	GError *err = NULL;
	GPtrArray *sub_queuev = g_ptr_array_new();

	for (gchar **token = tokens; *token && !err ;++token) {
		struct oio_events_queue_s *sub = NULL;
		if (!(err = oio_events_queue_factory__create(*token, tube, sync, &sub)))
			g_ptr_array_add(sub_queuev, sub);
	}

	if (!err) {
		if (sub_queuev->len <= 0) {
			err = BADREQ("empty connection string");
		} else {
			err = oio_events_queue_factory__create_fanout(
					(struct oio_events_queue_s **)sub_queuev->pdata,
					sub_queuev->len, out);
		}
	}

	if (err) {
		g_ptr_array_set_free_func(sub_queuev,
				(GDestroyNotify)oio_events_queue__destroy);
		g_ptr_array_free(sub_queuev, TRUE);
	} else {
		g_ptr_array_free(sub_queuev, FALSE);
	}

	g_strfreev(tokens);
	return err;
}

GError *
oio_events_queue_factory__create (const char *cfg, const char *tube,
		const gboolean sync, struct oio_events_queue_s **out)
{
	EXTRA_ASSERT (cfg != NULL);
	EXTRA_ASSERT (out != NULL);
	*out = NULL;

	if (NULL != strchr(cfg, OIO_CSV_SEP2_C)) {
		// Sharding over several endpoints
		return _parse_and_create_multi(cfg, tube, sync, out);
	} else {
		GError *err = NULL;
		const char *netloc;
		/* For a short period we accepted query-string parameters, hence the
		 * parsing. Notice that the "path" contains the scheme and hostname. */
		struct oio_requri_s queue_uri = {0};
		oio_requri_parse(cfg, &queue_uri);

		// Choose the right queue connector
		if ((netloc = _has_prefix(queue_uri.path, BEANSTALKD_PREFIX))) {
			err = oio_events_queue_factory__create_beanstalkd(
					netloc, tube, out);
		} else if ((netloc = _has_prefix(queue_uri.path, KAFKA_PREFIX))) {
			if (sync) {
				err = oio_events_queue_factory__create_kafka_sync(netloc, tube, out);
			} else {
				err = oio_events_queue_factory__create_kafka(netloc, tube, out);
			}
		} else {
			err = BADREQ("implementation not recognized: %s", cfg);
		}

		oio_requri_clear(&queue_uri);
		return err;
	}
}

void
oio_event__init (GString *gs, const char *type, struct oio_url_s *url)
{
	oio_str_gstring_append_json_pair(gs, "event", type);
	g_string_append_printf(gs, ",\"when\":%"G_GINT64_FORMAT, oio_ext_real_time());
	if (!url)
		g_string_append_static(gs, ",\"url\":null");
	else {
		g_string_append_static(gs, ",\"url\":{");
		/* Since the shard may disappear, all events related to the object
		 * must use the root container ID */
		oio_url_to_json(gs, url, g_str_has_prefix(type, "storage.content."));
		g_string_append_c(gs, '}');
	}
}

GString*
oio_event__create(const char *type, struct oio_url_s *url)
{
	return oio_event__create_with_id(type, url, NULL);
}

GString*
oio_event__create_with_id(const char *type, struct oio_url_s *url,
		const char *request_id)
{
	GString *gs = g_string_sized_new(512);
	g_string_append_c (gs, '{');
	oio_event__init (gs, type, url);
	if (request_id && *request_id) {
		g_string_append_c(gs, ',');
		oio_str_gstring_append_json_pair(gs, EVENT_FIELD_REQUEST_ID, request_id);
	}
	const gchar *user_agent = oio_ext_get_user_agent();
	if (user_agent != NULL) {
		g_string_append_c(gs, ',');
		oio_str_gstring_append_json_pair(gs, EVENT_FIELD_ORIGIN, user_agent);
	}
	return gs;
}


static GHashTable *_registered_events_queues = NULL;
static GMutex _registered_events_queues_lock = {0};

void
oio_events_stats_register(const gchar *key, struct oio_events_queue_s *queue)
{
	g_mutex_lock(&_registered_events_queues_lock);
	if (!_registered_events_queues) {
		_registered_events_queues = g_hash_table_new(g_str_hash, g_str_equal);
	} else {
		g_hash_table_ref(_registered_events_queues);
	}
	g_hash_table_replace(_registered_events_queues, (gpointer)key, queue);
	g_mutex_unlock(&_registered_events_queues_lock);
}

void
oio_events_stats_unregister(const gchar *key)
{
	g_mutex_lock(&_registered_events_queues_lock);
	g_hash_table_remove(_registered_events_queues, key);
	// Will free when ref count is zero.
	g_hash_table_unref(_registered_events_queues);
	g_mutex_unlock(&_registered_events_queues_lock);
}

struct _prom_stat_in_out {
	const gchar *namespace;
	const gchar *service_id;
	GString *out;
};

static void
_stat_append_to_str(const gchar *key,
		struct oio_events_queue_s *queue, struct _prom_stat_in_out *in_out)
{
	g_string_append_static(in_out->out,
			"meta_event_sent_total{service_id=\"");
	g_string_append(in_out->out, in_out->service_id);
	g_string_append_static(in_out->out, "\",event_type=\"");
	g_string_append(in_out->out, key);
	g_string_append_static(in_out->out, "\",namespace=\"");
	g_string_append(in_out->out, in_out->namespace);
	g_string_append_static(in_out->out, "\"} ");
	guint64 events = oio_events_queue__get_total_sent_events(queue);
	g_string_append_printf(in_out->out, "%"G_GUINT64_FORMAT"\n", events);

	g_string_append_static(in_out->out,
			"meta_event_send_time_seconds_total{service_id=\"");
	g_string_append(in_out->out, in_out->service_id);
	g_string_append_static(in_out->out, "\",event_type=\"");
	g_string_append(in_out->out, key);
	g_string_append_static(in_out->out, "\",namespace=\"");
	g_string_append(in_out->out, in_out->namespace);
	g_string_append_static(in_out->out, "\"} ");
	guint64 time_us = oio_events_queue__get_total_send_time(queue);
	double time_s = (double)time_us / (double)G_TIME_SPAN_SECOND;
	g_string_append_printf(in_out->out, "%.6f\n", time_s);
}

void
oio_events_stats_to_prometheus(const gchar *service_id, const gchar *namespace,
		GString *out)
{
	struct _prom_stat_in_out in_out = {
		.namespace = namespace,
		.service_id = service_id,
		.out = out
	};
	g_mutex_lock(&_registered_events_queues_lock);
	// We may have an issue here, if we create it and then empty it completely.
	if (_registered_events_queues) {
		g_hash_table_foreach(_registered_events_queues,
				(GHFunc)_stat_append_to_str, &in_out);
	}
	g_mutex_unlock(&_registered_events_queues_lock);
}
