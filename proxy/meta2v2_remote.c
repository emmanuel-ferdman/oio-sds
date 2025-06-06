/*
OpenIO SDS meta2v2
Copyright (C) 2014 Worldline, as part of Redcurrant
Copyright (C) 2015-2019 OpenIO SAS, as part of OpenIO SDS
Copyright (C) 2021-2025 OVH SAS

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

#include <strings.h>

#include <metautils/lib/metautils.h>

#include <meta2v2/meta2_macros.h>
#include <meta2v2/meta2_utils.h>
#include <meta2v2/meta2_bean.h>
#include <meta2v2/generic.h>
#include <meta2v2/autogen.h>

#include "meta2v2_remote.h"
#include "common.h"

static MESSAGE
_m2v2_build_request_with_extra_fields(const char *name, struct oio_url_s *url,
		GByteArray *body, const gchar **fields, gint64 deadline)
{
	EXTRA_ASSERT(name != NULL);
	EXTRA_ASSERT(url != NULL);

	MESSAGE msg = metautils_message_create_named(name, deadline);
	metautils_message_add_url (msg, url);
	metautils_message_add_fields_str(msg, fields);
	if (body)
		metautils_message_add_body_unref (msg, body);
	return msg;
}

static MESSAGE
_m2v2_build_request(const char *name, struct oio_url_s *url,
		GByteArray *body, gint64 deadline)
{
	return _m2v2_build_request_with_extra_fields(
			name, url, body, NULL, deadline);
}

static GByteArray *
_m2v2_pack_request_with_flags(const char *name, struct oio_url_s *url,
		GByteArray *body, guint32 flags, gint64 deadline)
{
	MESSAGE msg = _m2v2_build_request(name, url, body, deadline);
	flags = g_htonl(flags);
	metautils_message_add_field(msg, NAME_MSGKEY_FLAGS, &flags, sizeof(flags));
	return message_marshall_gba_and_clean(msg);
}

static GByteArray *
_m2v2_pack_request(const char *name, struct oio_url_s *url, GByteArray *body,
		gint64 deadline)
{
	MESSAGE msg = _m2v2_build_request(name, url, body, deadline);
	return message_marshall_gba_and_clean(msg);
}

//------------------------------------------------------------------------------

void
m2v2_list_result_init (struct list_result_s *p)
{
	EXTRA_ASSERT(p != NULL);
	p->props = g_tree_new_full (metautils_strcmp3, NULL, g_free, g_free);
}

void
m2v2_list_result_clean (struct list_result_s *p)
{
	if (!p) return;
	_bean_cleanl2(p->beans);
	p->beans = NULL;
	if (p->props) g_tree_destroy(p->props);
	p->props = NULL;
	oio_str_clean(&p->next_marker);
	oio_str_clean(&p->next_version_marker);
	p->truncated = FALSE;
}

gboolean
m2v2_list_result_extract(gpointer ctx, guint status, MESSAGE reply)
{
	struct list_result_s *out = ctx;
	EXTRA_ASSERT(out != NULL);

	if (status != CODE_REDIRECT_SHARD) {
		/* Extract replied aliases */
		GSList *l = NULL;
		GError *e = metautils_message_extract_body_encoded(reply, FALSE, &l,
				bean_sequence_decoder);
		if (e) {
			GRID_DEBUG("Callback error: (%d) %s", e->code, e->message);
			return FALSE;
		}

		out->beans = metautils_gslist_precat(out->beans, l);

		/* Extract list flags */
		e = metautils_message_extract_boolean(reply,
				NAME_MSGKEY_TRUNCATED, FALSE, &out->truncated);
		if (e) {
			GRID_ERROR("Failed to extract '"NAME_MSGKEY_TRUNCATED"': (%d) %s "
					"(reqid=%s)", e->code, e->message, oio_ext_get_reqid());
			g_clear_error(&e);
		}
		gchar *marker = metautils_message_extract_string_copy(reply,
				NAME_MSGKEY_NEXTMARKER);
		oio_str_reuse(&out->next_marker, marker);
		gchar *version_marker = metautils_message_extract_string_copy(reply,
				NAME_MSGKEY_NEXTVERSIONMARKER);
		oio_str_reuse(&out->next_version_marker, version_marker);
	}

	if (g_tree_nnodes(out->props) == 0) {
		/* Extract properties and merge them into the temporary TreeSet. */
		gchar **names = metautils_message_get_field_names(reply);
		for (gchar **n = names; names && *n; ++n) {
			if (!g_str_has_prefix(*n, NAME_MSGKEY_PREFIX_PROPERTY))
				continue;
			g_tree_replace(out->props,
					g_strdup((*n) + sizeof(NAME_MSGKEY_PREFIX_PROPERTY) - 1),
					metautils_message_extract_string_copy(reply, *n));
		}
		g_strfreev(names);
	}

	return TRUE;
}

gboolean
m2v2_boolean_truncated_extract(gpointer ctx, guint status UNUSED, MESSAGE reply)
{
	gboolean *truncated = ctx;
	EXTRA_ASSERT (truncated != NULL);

	GError *err = metautils_message_extract_boolean(reply,
			NAME_MSGKEY_TRUNCATED, FALSE, truncated);
	if (err) {
		GRID_ERROR("Failed to extract '"NAME_MSGKEY_TRUNCATED"': (%d) %s "
				"(reqid=%s)", err->code, err->message, oio_ext_get_reqid());
		g_clear_error(&err);
	}

	return TRUE;
}

gboolean
m2v2_offset_extract(gpointer ctx, guint status UNUSED, MESSAGE reply)
{
	gchar **offset = ctx;
	EXTRA_ASSERT (offset != NULL);

	*offset = metautils_message_extract_string_copy(reply,
			NAME_MSGKEY_INCR_OFFSET);

	return TRUE;
}

GByteArray* m2v2_remote_pack_CREATE(
		struct oio_url_s *url,
		struct m2v2_create_params_s *pols,
		const gchar **headers,
		gint64 dl)
{
	const gchar *region = oio_ext_get_region();
	MESSAGE msg = _m2v2_build_request_with_extra_fields(
			NAME_MSGNAME_M2V2_CREATE, url, NULL, headers, dl);
	if (oio_str_is_set(region))
		metautils_message_add_field_str(msg, NAME_MSGKEY_REGION, region);
	if (pols && pols->storage_policy)
		metautils_message_add_field_str(msg, NAME_MSGKEY_STGPOLICY, pols->storage_policy);
	if (pols && pols->version_policy)
		metautils_message_add_field_str(msg, NAME_MSGKEY_VERPOLICY, pols->version_policy);
	if (pols && pols->properties) {
		GString *gs = KV_encode_gstr(pols->properties);
		metautils_message_set_BODY (msg, gs->str, gs->len);
		g_string_free (gs, TRUE);
	}

	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_DESTROY(struct oio_url_s *url, guint32 flags, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_DESTROY, url, NULL, dl);
	if (flags & M2V2_DESTROY_FORCE)
		metautils_message_add_field_str(msg, NAME_MSGKEY_FORCE, "1");
	if (flags & M2V2_DESTROY_EVENT)
		metautils_message_add_field_str(msg, NAME_MSGKEY_EVENT, "1");

	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_container_DRAIN(struct oio_url_s *url, const char *limit_str,
		gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_CONTAINER_DRAIN, url,
			NULL, dl);
	metautils_message_add_field_str(msg, NAME_MSGKEY_LIMIT, limit_str);

	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_FLUSH(struct oio_url_s *url, gint64 dl)
{
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_FLUSH, url, NULL, dl);
}

GByteArray*
m2v2_remote_pack_PURGEC(struct oio_url_s *url,
		const char *maxvers_str, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(
			NAME_MSGNAME_M2V2_PURGE_CONTENT, url, NULL, dl);
	metautils_message_add_field_str(msg, NAME_MSGKEY_MAXVERS, maxvers_str);
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_PURGEB(struct oio_url_s *url,
		const char *maxvers_str, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(
			NAME_MSGNAME_M2V2_PURGE_CONTAINER, url, NULL, dl);
	metautils_message_add_field_str(msg, NAME_MSGKEY_MAXVERS, maxvers_str);
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_DEDUP(struct oio_url_s *url, gint64 dl)
{
	return _m2v2_pack_request (NAME_MSGNAME_M2V2_DEDUP, url, NULL, dl);
}


GByteArray*
m2v2_remote_pack_PUT(struct oio_url_s *url, GSList *beans, const char *destinations,
	const char *replicator_id, const char *role_project_id, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	MESSAGE msg = _m2v2_build_request (NAME_MSGNAME_M2V2_PUT, url, body, dl);
	const gchar *force_versioning = oio_ext_get_force_versioning();
	if (force_versioning != NULL) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_FORCE_VERSIONING,
				force_versioning);
	}
	if (destinations) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_DESTS,
				destinations);
	}
	if (replicator_id) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_ID,
				replicator_id);
	}
	if (role_project_id) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_PROJECT_ID,
				role_project_id);
	}
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_OVERWRITE(struct oio_url_s *url, GSList *beans, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_PUT, url, body, dl);
	metautils_message_add_field_str(msg, NAME_MSGKEY_OVERWRITE, "1");
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_UPDATE(struct oio_url_s *url, GSList *beans, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_PUT, url, body, dl);
	metautils_message_add_field_str(msg, NAME_MSGKEY_UPDATE, "1");
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_CHANGE_POLICY(struct oio_url_s *url, GSList *beans, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_PUT, url, body, dl);
	metautils_message_add_field_str(msg, NAME_MSGKEY_CHANGE_POLICY, "1");
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_POLICY_TRANSITION(struct oio_url_s *url, const gchar* policy, gboolean skip_data_move, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(
		NAME_MSGNAME_M2V2_POLICY_TRANSITION, url, NULL, dl);
	metautils_message_add_field_str(msg, NAME_MSGKEY_CHANGE_POLICY, policy);
	if	(skip_data_move) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_SKIP_DATA_MOVE, "1");
	}
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_RESTORE_DRAINED(struct oio_url_s *url, GSList *beans, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_PUT, url, body, dl);
	metautils_message_add_field_str(msg, NAME_MSGKEY_RESTORE_DRAINED, "1");
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_APPEND(struct oio_url_s *url, GSList *beans, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_APPEND, url, body, dl);
}

GByteArray*
m2v2_remote_pack_content_DRAIN(struct oio_url_s *url, gint64 dl)
{
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_CONTENT_DRAIN, url, NULL, dl);
}

GByteArray*
m2v2_remote_pack_DEL(struct oio_url_s *url, gboolean bypass_governance,
		gboolean create_delete_marker, gboolean dryrun, gboolean slo_manifest,
		const char *destinations, const char *replicator_id,
		const char *role_project_id, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_DEL, url, NULL, dl);
	if (bypass_governance) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_BYPASS_GOVERNANCE,
			"1");
	}
	if (create_delete_marker) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_DELETE_MARKER, "1");
	}
	if (dryrun) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_DRYRUN, "1");
	}
	if (slo_manifest) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_SLO_MANIFEST, "1");
	}
	const gchar *force_versioning = oio_ext_get_force_versioning();
	if (force_versioning != NULL) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_FORCE_VERSIONING,
				force_versioning);
	}
	if (destinations) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_DESTS,
				destinations);
	}
	if (replicator_id) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_ID,
				replicator_id);
	}
	if (role_project_id) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_PROJECT_ID,
				role_project_id);
	}
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_TRUNC(struct oio_url_s *url, gint64 size, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_TRUNC, url, NULL, dl);
	metautils_message_add_field_strint64(msg, NAME_MSGKEY_CONTENTLENGTH, size);
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_RAW_DEL(struct oio_url_s *url, GSList *beans, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_RAW_DEL, url, body, dl);
}

GByteArray*
m2v2_remote_pack_RAW_ADD(struct oio_url_s *url, GSList *beans, gboolean frozen,
		gboolean force, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_RAW_ADD, url, body, dl);
	if (force)
		metautils_message_add_field_str(msg, NAME_MSGKEY_FORCE, "1");
	if (frozen)
		metautils_message_add_field_str(msg, NAME_MSGKEY_FROZEN, "1");
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_RAW_SUBST(struct oio_url_s *url,
		GSList *new_chunks, GSList *old_chunks, gboolean frozen, gint64 dl)
{
	GByteArray *new_chunks_gba = bean_sequence_marshall(new_chunks);
	GByteArray *old_chunks_gba = bean_sequence_marshall(old_chunks);
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_RAW_SUBST, url, NULL, dl);
	metautils_message_add_field_gba(msg, NAME_MSGKEY_NEW, new_chunks_gba);
	metautils_message_add_field_gba(msg, NAME_MSGKEY_OLD, old_chunks_gba);
	if (frozen)
		metautils_message_add_field_str(msg, NAME_MSGKEY_FROZEN, "1");
	g_byte_array_unref (new_chunks_gba);
	g_byte_array_unref (old_chunks_gba);
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_GET(struct oio_url_s *url, guint32 flags, gint64 dl)
{
	return _m2v2_pack_request_with_flags(NAME_MSGNAME_M2V2_GET, url, NULL, flags, dl);
}

static void
_pack_list_params(MESSAGE msg, struct list_params_s *p)
{
	guint32 flags = 0;
	if (p->flag_allversion) flags |= M2V2_FLAG_ALLVERSION;
	if (p->flag_headers) flags |= M2V2_FLAG_HEADERS;
	if (p->flag_nodeleted) flags |= M2V2_FLAG_NODELETED;
	if (p->flag_properties) flags |= M2V2_FLAG_ALLPROPS;
	if (p->flag_mpu_marker_only) flags |= M2V2_FLAG_MPUMARKER_ONLY;
	// Beware of the negation of the flag
	if (!p->flag_recursion) flags |= M2V2_FLAG_NORECURSION;
	flags = g_htonl(flags);
	metautils_message_add_field(msg, NAME_MSGKEY_FLAGS, &flags, sizeof(flags));

	metautils_message_add_field_str(msg, NAME_MSGKEY_PREFIX, p->prefix);
	metautils_message_add_field_str(msg, NAME_MSGKEY_DELIMITER, p->delimiter);
	metautils_message_add_field_str(msg, NAME_MSGKEY_MARKER, p->marker_start);
	metautils_message_add_field_str(msg, NAME_MSGKEY_VERSIONMARKER, p->version_marker);
	metautils_message_add_field_str(msg, NAME_MSGKEY_MARKER_END, p->marker_end);
	if (p->maxkeys > 0)
		metautils_message_add_field_strint64(msg, NAME_MSGKEY_MAX_KEYS, p->maxkeys);
}

GByteArray*
m2v2_remote_pack_LIST(struct oio_url_s *url,
		struct list_params_s *p, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_LIST, url, NULL, dl);
	_pack_list_params(msg, p);
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_LIST_BY_CHUNKID(struct oio_url_s *url,
		struct list_params_s *p, const char *chunk, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_LCHUNK, url, NULL, dl);
	_pack_list_params(msg, p);
	metautils_message_add_field_str (msg, NAME_MSGKEY_KEY, chunk);
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_LIST_BY_HEADERHASH(struct oio_url_s *url,
		struct list_params_s *p, GBytes *h, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_LHHASH, url, NULL, dl);
	_pack_list_params(msg, p);
	metautils_message_add_field (msg, NAME_MSGKEY_KEY,
			g_bytes_get_data(h,NULL), g_bytes_get_size(h));
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_LIST_BY_HEADERID(struct oio_url_s *url,
		struct list_params_s *p, GBytes *h, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_LHID, url, NULL, dl);
	_pack_list_params(msg, p);
	metautils_message_add_field (msg, NAME_MSGKEY_KEY,
			g_bytes_get_data(h,NULL), g_bytes_get_size(h));
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_PROP_DEL(struct oio_url_s *url, gchar **names,
		const char *destinations, const char *replicator_id,
		const char *role_project_id, gint64 dl)
{
	GByteArray *body = g_bytes_unref_to_array(
			g_string_free_to_bytes(STRV_encode_gstr(names)));

	MESSAGE msg = _m2v2_build_request (NAME_MSGNAME_M2V2_PROP_DEL, url, body, dl);
	if (destinations) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_DESTS,
				destinations);
	}
	if (replicator_id) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_ID,
				replicator_id);
	}
	if (role_project_id) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_PROJECT_ID,
				role_project_id);
	}
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_PROP_SET(struct oio_url_s *url, guint32 flags,
		GSList *beans, const char *destinations, const char *replicator_id,
		const char *role_project_id, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	MESSAGE msg = _m2v2_build_request (NAME_MSGNAME_M2V2_PROP_SET, url, body, dl);
	flags = g_htonl(flags);
	metautils_message_add_field(msg, NAME_MSGKEY_FLAGS, &flags, sizeof(flags));

	if (destinations) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_DESTS,
				destinations);
	}
	if (replicator_id) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_ID,
				replicator_id);
	}
	if (role_project_id) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_REPLI_PROJECT_ID,
				role_project_id);
	}
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_PROP_GET(struct oio_url_s *url, gint64 dl)
{
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_PROP_GET, url, NULL, dl);
}

GByteArray*
m2v2_remote_pack_TOUCHC(struct oio_url_s *url, gint64 dl)
{
	return _m2v2_pack_request(NAME_MSGNAME_M2V1_TOUCH_CONTENT, url, NULL, dl);
}

GByteArray*
m2v2_remote_pack_TOUCHB(struct oio_url_s *url, guint32 flags, gint64 dl,
		gboolean recompute)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V1_TOUCH_CONTAINER,
			url, NULL, dl);
	flags = g_htonl(flags);
	metautils_message_add_field(msg, NAME_MSGKEY_FLAGS,
			&flags, sizeof(flags));
	if (recompute)
		metautils_message_add_field_str(msg, NAME_MSGKEY_RECOMPUTE, "1");
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_ISEMPTY (struct oio_url_s *url, gint64 dl)
{
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_ISEMPTY, url, NULL, dl);
}

/* ------------------------------------------------------------------------- */

GError*
m2v2_remote_execute_DESTROY(const char *target, struct oio_url_s *url,
		guint32 flags)
{
	return gridd_client_exec(target,
			oio_clamp_timeout(proxy_timeout_common, oio_ext_get_deadline()),
			m2v2_remote_pack_DESTROY(url, flags, oio_ext_get_deadline()));
}

GError*
m2v2_remote_execute_DESTROY_many(gchar **targets, struct oio_url_s *url, guint32 flags)
{
	if (!targets)
		return NEWERROR(CODE_INTERNAL_ERROR, "invalid target array (NULL)");

	// TODO: factorize with sqlx_remote_execute_DESTROY_many
	GByteArray *req = m2v2_remote_pack_DESTROY(url, flags, oio_ext_get_deadline());
	struct gridd_client_s **clients = gridd_client_create_many(targets, req, NULL, NULL);
	metautils_gba_unref(req);
	req = NULL;

	if (clients == NULL)
		return NEWERROR(0, "Failed to create gridd clients");

	gridd_clients_start(clients);
	GError *err = gridd_clients_loop(clients);
	for (struct gridd_client_s **p = clients; !err && p && *p ;p++) {
		if (!(err = gridd_client_error(*p)))
			continue;
		GRID_DEBUG("Database destruction attempts failed: (%d) %s",
				err->code, err->message);
		if (err->code == CODE_CONTAINER_NOTFOUND || err->code == CODE_NOT_FOUND) {
			g_clear_error(&err);
			continue;
		}
	}

	gridd_clients_free(clients);
	return err;
}

/* Sharding ----------------------------------------------------------------- */

GByteArray*
m2v2_remote_pack_FIND_SHARDS(struct oio_url_s *url, const gchar* strategy,
		GByteArray *strategy_params, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_FIND_SHARDS, url,
			NULL, dl);
	metautils_message_add_field_str(msg, NAME_MSGKEY_SHARDING_STRATEGY,
			strategy);
	if (strategy_params) {
		metautils_message_set_BODY(msg, strategy_params->data,
				strategy_params->len);
	}
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_PREPARE_SHARDING(struct oio_url_s *url, const gchar* action,
		GSList *beans, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_PREPARE_SHARDING, url,
			body, dl);
	if (action && *action) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_SHARDING_ACTION,
				action);
	}
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_MERGE_SHARDING(struct oio_url_s *url, GSList *beans, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_MERGE_SHARDING, url, body,
			dl);
}

GByteArray*
m2v2_remote_pack_UPDATE_SHARD(struct oio_url_s *url, gchar **queries, gint64 dl)
{
	GByteArray *body = g_bytes_unref_to_array(
			g_string_free_to_bytes(STRV_encode_gstr(queries)));
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_UPDATE_SHARD, url, body, dl);
}

GByteArray*
m2v2_remote_pack_LOCK_SHARDING(struct oio_url_s *url, gint64 dl)
{
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_LOCK_SHARDING, url, NULL, dl);
}

GByteArray*
m2v2_remote_pack_REPLACE_SHARDING(struct oio_url_s *url, GSList *beans,
		gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_REPLACE_SHARDING, url, body,
			dl);
}

GByteArray*
m2v2_remote_pack_CLEAN_SHARDING(struct oio_url_s *url, GSList *beans,
		gboolean local, gboolean urgent, gint64 dl)
{
	GByteArray *body = bean_sequence_marshall(beans);
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_CLEAN_SHARDING, url,
			body, dl);
	if (local) {
		metautils_message_add_field_strint(msg, NAME_MSGKEY_LOCAL, 1);
	}
	if (urgent) {
		metautils_message_add_field_strint(msg, NAME_MSGKEY_URGENT, 1);
	}
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_SHOW_SHARDING(struct oio_url_s *url,
		struct list_params_s *params, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_SHOW_SHARDING,
			url, NULL, dl);
	_pack_list_params(msg, params);
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_ABORT_SHARDING(struct oio_url_s *url, gint64 dl)
{
	return _m2v2_pack_request(NAME_MSGNAME_M2V2_ABORT_SHARDING, url, NULL, dl);
}

GByteArray*
m2v2_remote_pack_CHECKPOINT(struct oio_url_s *url, const char* suffix, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_CHECKPOINT, url, NULL, dl);
	metautils_message_add_field_str(msg, NAME_MSGKEY_SUFFIX, suffix);
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_GET_SHARDS_IN_RANGE(struct oio_url_s *url,
	GByteArray *bounds_params, gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_SHARDS_IN_RANGE,
		url, NULL, dl);
	if (bounds_params) {
		metautils_message_set_BODY(msg, bounds_params->data,
				bounds_params->len);
	}
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_CREATE_LIFECYCLE_VIEWS(struct oio_url_s *url, GByteArray *params,
		gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_CREATE_LIFECYCLE_VIEWS, url,
			NULL, dl);
	if (params) {
		metautils_message_set_BODY(msg, params->data,
				params->len);
	}
	return message_marshall_gba_and_clean(msg);
}

GByteArray*
m2v2_remote_pack_APPLY_LIFECYCLE(struct oio_url_s *url, const gchar* action_type, GByteArray *params,
		gint64 dl)
{
	MESSAGE msg = _m2v2_build_request(NAME_MSGNAME_M2V2_APPLY_LIFECYCLE, url,
			NULL, dl);
	if (action_type) {
		metautils_message_add_field_str(msg, NAME_MSGKEY_ACTION_TYPE, action_type);
	}
	if (params) {
		metautils_message_set_BODY(msg, params->data,
				params->len);
	}
	return message_marshall_gba_and_clean(msg);
}
