/*
 * Copyright (c) 2017, 2021 ADLINK Technology Inc.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Apache License, Version 2.0
 * which is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
 *
 * Contributors:
 *   ADLINK zenoh team, <zenoh@adlink-labs.tech>
 */

#include "zenoh-pico/protocol/msg.h"
#include "zenoh-pico/protocol/msgcodec.h"
#include "zenoh-pico/protocol/utils.h"
#include "zenoh-pico/protocol/utils.h"
#include "zenoh-pico/utils/logging.h"
#include "zenoh-pico/session/query.h"
#include "zenoh-pico/session/resource.h"
#include "zenoh-pico/session/session.h"
#include "zenoh-pico/system/platform.h"

/*------------------ Query ------------------*/
z_zint_t _zn_get_query_id(zn_session_t *zn)
{
    return zn->query_id++;
}

/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_inner
 */
_zn_pending_query_t *__unsafe_zn_get_pending_query_by_id(zn_session_t *zn, z_zint_t id)
{
    _zn_pending_query_list_t *queries = zn->pending_queries;
    while (queries)
    {
        _zn_pending_query_t *query = _zn_pending_query_list_head(queries);

        if (query->id == id)
            return query;

        queries = _zn_pending_query_list_tail(queries);
    }

    return NULL;
}

int _zn_register_pending_query(zn_session_t *zn, _zn_pending_query_t *pen_qry)
{
    _Z_DEBUG_VA(">>> Allocating query for (%lu,%s,%s)\n", pen_qry->key.rid, pen_qry->key.rname, pen_qry->predicate);
    // Acquire the lock on the queries
    z_mutex_lock(&zn->mutex_inner);
    int res;
    _zn_pending_query_t *q = __unsafe_zn_get_pending_query_by_id(zn, pen_qry->id);
    if (q)
    {
        // A query for this id already exists, return error
        res = -1;
    }
    else
    {
        // Register the query
        zn->pending_queries = _zn_pending_query_list_push(zn->pending_queries, pen_qry);
        res = 0;
    }

    // Release the lock
    z_mutex_unlock(&zn->mutex_inner);

    return res;
}

int _zn_pending_reply_eq(const _zn_pending_reply_t *this, const _zn_pending_reply_t *other)
{
    return this->tstamp.time == other->tstamp.time; // FIXME: pending reply comparison
}

void _zn_pending_reply_clear(_zn_pending_reply_t *pr)
{
    // Free the sample
    if (pr->reply.data.data.key.val)
        free((z_str_t)pr->reply.data.data.key.val);
    if (pr->reply.data.data.value.val)
        _z_bytes_clear(&pr->reply.data.data.value);

    // Free the source info
    if (pr->reply.data.replier_id.val)
        _z_bytes_clear(&pr->reply.data.replier_id);

    // Free the timestamp
    if (pr->tstamp.id.val)
        _z_bytes_clear(&pr->tstamp.id);
}

void _zn_pending_query_clear(_zn_pending_query_t *pen_qry)
{
    _zn_reskey_clear(&pen_qry->key);
    if (pen_qry->predicate)
        free((z_str_t)pen_qry->predicate);

    _zn_pending_reply_list_free(&pen_qry->pending_replies);
}

int _zn_pending_query_eq(const _zn_pending_query_t *this, const _zn_pending_query_t *other)
{
    return this->id == other->id;
}

/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_inner
 */
void __unsafe_zn_unregister_pending_query(zn_session_t *zn, _zn_pending_query_t *pen_qry)
{
    zn->pending_queries = _zn_pending_query_list_drop_filter(zn->pending_queries, _zn_pending_query_eq, pen_qry);
    free(pen_qry);
}

void _zn_unregister_pending_query(zn_session_t *zn, _zn_pending_query_t *pen_qry)
{
    z_mutex_lock(&zn->mutex_inner);
    __unsafe_zn_unregister_pending_query(zn, pen_qry);
    z_mutex_unlock(&zn->mutex_inner);
}

void _zn_flush_pending_queries(zn_session_t *zn)
{
    // Lock the resources data struct
    z_mutex_lock(&zn->mutex_inner);

    _zn_pending_query_list_free(&zn->pending_queries);

    // Release the lock
    z_mutex_unlock(&zn->mutex_inner);
}

void _zn_trigger_query_reply_partial(zn_session_t *zn,
                                     const _zn_reply_context_t *reply_context,
                                     const zn_reskey_t reskey,
                                     const z_bytes_t payload,
                                     const _zn_data_info_t data_info)
{
    // Acquire the lock on the queries
    z_mutex_lock(&zn->mutex_inner);

    if (_ZN_HAS_FLAG(reply_context->header, _ZN_FLAG_Z_F))
    {
        _Z_DEBUG(">>> Partial reply received with invalid final flag\n");
        goto EXIT_QRY_TRIG_PAR;
    }

    _zn_pending_query_t *pen_qry = __unsafe_zn_get_pending_query_by_id(zn, reply_context->qid);
    if (pen_qry == NULL)
    {
        _Z_DEBUG_VA(">>> Partial reply received for unkwon query id (%zu)\n", reply_context->qid);
        goto EXIT_QRY_TRIG_PAR;
    }

    if (pen_qry->target.kind != ZN_QUERYABLE_ALL_KINDS && (pen_qry->target.kind & reply_context->replier_kind) == 0)
    {
        _Z_DEBUG_VA(">>> Partial reply received from an unknown target (%zu)\n", reply_context->replier_kind);
        goto EXIT_QRY_TRIG_PAR;
    }

    // Take the right timestamp, or default to none
    z_timestamp_t ts;
    if _ZN_HAS_FLAG (data_info.flags, _ZN_DATA_INFO_TSTAMP)
        ts = data_info.tstamp;
    else
        z_timestamp_reset(&ts);

    // Build the reply
    zn_reply_t reply;
    reply.tag = zn_reply_t_Tag_DATA;
    reply.data.data.value = payload;
    if (reskey.rid == ZN_RESOURCE_ID_NONE)
    {
        reply.data.data.key.val = reskey.rname;
    }
    else
    {
        reply.data.data.key.val = __unsafe_zn_get_resource_name_from_key(zn, _ZN_IS_REMOTE, &reskey);
    }
    reply.data.data.key.len = strlen(reply.data.data.key.val);
    reply.data.replier_id = reply_context->replier_id;
    reply.data.replier_kind = reply_context->replier_kind;

    // Verify if this is a newer reply, free the old one in case it is
    _zn_pending_reply_t *latest = NULL;
    switch (pen_qry->consolidation.reception)
    {
    case zn_consolidation_mode_t_FULL:
    case zn_consolidation_mode_t_LAZY:
    {
        // Check if this is a newer reply
        _zn_pending_reply_list_t *pen_rps = pen_qry->pending_replies;
        while (pen_rps)
        {
            _zn_pending_reply_t *pen_rep = _zn_pending_reply_list_head(pen_rps);

            // Check if this is the same resource key
            if (_z_str_eq(reply.data.data.key.val, pen_rep->reply.data.data.key.val))
            {
                if (ts.time <= pen_rep->tstamp.time)
                {
                    _Z_DEBUG(">>> Reply received with old timestamp\n");
                    if (reskey.rid != ZN_RESOURCE_ID_NONE)
                        free((z_str_t)reply.data.data.key.val);
                    goto EXIT_QRY_TRIG_PAR;
                }
                else
                {
                    // We are going to have a more recent reply, free the old one
                    pen_qry->pending_replies = _zn_pending_reply_list_pop(pen_qry->pending_replies);
                    latest = _zn_pending_reply_list_head(pen_qry->pending_replies);
                    break;
                }
            }
            else
            {
                pen_rps = _zn_pending_reply_list_tail(pen_rps);
            }
        }
        break;
    }
    case zn_consolidation_mode_t_NONE:
    {
        // Do nothing. Replies are not stored with no consolidation
        break;
    }
    }

    // Store the reply and trigger the callback if needed
    switch (pen_qry->consolidation.reception)
    {
    // Store the reply but do not trigger the callback
    case zn_consolidation_mode_t_FULL:
    {
        // Allocate a pending reply if needed
        _zn_pending_reply_t *pen_rep;
        if (latest == NULL)
            pen_rep = (_zn_pending_reply_t *)malloc(sizeof(_zn_pending_reply_t));
        else
            pen_rep = latest;

        // Copy the reply tag
        pen_rep->reply.tag = reply.tag;

        // Make a copy of the sample if needed
        _z_bytes_copy((z_bytes_t *)&pen_rep->reply.data.data.value, (z_bytes_t *)&reply.data.data.value);
        if (reskey.rid == ZN_RESOURCE_ID_NONE)
            pen_rep->reply.data.data.key.val = _z_str_clone(reply.data.data.key.val);
        else
            pen_rep->reply.data.data.key.val = reply.data.data.key.val;
        pen_rep->reply.data.data.key.len = reply.data.data.key.len;

        // Make a copy of the source info
        _z_bytes_copy((z_bytes_t *)&pen_rep->reply.data.replier_id, (z_bytes_t *)&reply.data.replier_id);
        pen_rep->reply.data.replier_kind = reply.data.replier_kind;

        // Make a copy of the data info timestamp if present
        pen_rep->tstamp = z_timestamp_duplicate(&ts);

        // Add it to the list of pending replies if new
        if (latest == NULL)
            pen_qry->pending_replies = _zn_pending_reply_list_push(pen_qry->pending_replies, pen_rep);

        break;
    }
    // Trigger the callback, store only the timestamp of the reply
    case zn_consolidation_mode_t_LAZY:
    {
        // Allocate a pending reply if needed
        _zn_pending_reply_t *pen_rep;
        if (latest == NULL)
            pen_rep = (_zn_pending_reply_t *)malloc(sizeof(_zn_pending_reply_t));
        else
            pen_rep = latest;

        // Copy the reply tag
        pen_rep->reply.tag = reply.tag;

        // Do not copy the payload, we are triggering the handler straight away
        // Copy the resource key
        pen_rep->reply.data.data.value = payload;
        if (reskey.rid == ZN_RESOURCE_ID_NONE)
            pen_rep->reply.data.data.key.val = _z_str_clone(reply.data.data.key.val);
        else
            pen_rep->reply.data.data.key.val = reply.data.data.key.val;
        pen_rep->reply.data.data.key.len = reply.data.data.key.len;

        // Do not copy the source info, we are triggering the handler straight away
        pen_rep->reply.data.replier_id = reply.data.replier_id;
        pen_rep->reply.data.replier_kind = reply.data.replier_kind;

        // Do not sotre the replier ID, we are triggering the handler straight away
        // Make a copy of the timestamp
        _z_bytes_reset(&pen_rep->tstamp.id);
        pen_rep->tstamp.time = ts.time;

        // Add it to the list of pending replies
        if (latest == NULL)
            pen_qry->pending_replies = _zn_pending_reply_list_push(pen_qry->pending_replies, pen_rep);

        // Trigger the handler
        pen_qry->callback(pen_rep->reply, pen_qry->arg);

        // Set to null the data and replier id
        _z_bytes_reset(&pen_rep->reply.data.data.value);
        _z_bytes_reset(&pen_rep->reply.data.replier_id);

        break;
    }
    // Trigger only the callback, do not store the reply
    case zn_consolidation_mode_t_NONE:
    {
        // Trigger the handler
        pen_qry->callback(reply, pen_qry->arg);

        // Free the resource name if allocated
        if (reskey.rid != ZN_RESOURCE_ID_NONE)
            free((z_str_t)reply.data.data.key.val);

        break;
    }
    default:
        break;
    }

EXIT_QRY_TRIG_PAR:
    // Release the lock
    z_mutex_unlock(&zn->mutex_inner);
}

void _zn_trigger_query_reply_final(zn_session_t *zn, const _zn_reply_context_t *reply_context)
{
    // Acquire the lock on the queries
    z_mutex_lock(&zn->mutex_inner);

    if (!_ZN_HAS_FLAG(reply_context->header, _ZN_FLAG_Z_F))
    {
        _Z_DEBUG(">>> Final reply received with invalid final flag\n");
        goto EXIT_QRY_TRIG_FIN;
    }

    _zn_pending_query_t *pen_qry = __unsafe_zn_get_pending_query_by_id(zn, reply_context->qid);
    if (pen_qry == NULL)
    {
        _Z_DEBUG_VA(">>> Final reply received for unkwon query id (%zu)\n", reply_context->qid);
        goto EXIT_QRY_TRIG_FIN;
    }

    if (pen_qry->target.kind != ZN_QUERYABLE_ALL_KINDS && (pen_qry->target.kind & reply_context->replier_kind) == 0)
    {
        _Z_DEBUG_VA(">>> Final reply received from an unknown target (%zu)\n", reply_context->replier_kind);
        goto EXIT_QRY_TRIG_FIN;
    }

    // The reply is the final one, apply consolidation if needed
    while (pen_qry->pending_replies)
    {
        _zn_pending_reply_t *pen_rep = (_zn_pending_reply_t *)_zn_pending_reply_list_head(pen_qry->pending_replies);
        if (pen_qry->consolidation.reception == zn_consolidation_mode_t_FULL)
        {
            // Trigger the query handler
            pen_qry->callback(pen_rep->reply, pen_qry->arg);
        }

        // Free the element
        pen_qry->pending_replies = _zn_pending_reply_list_pop(pen_qry->pending_replies);
    }

    // Build the final reply
    zn_reply_t fin_rep;
    memset(&fin_rep, 0, sizeof(zn_reply_t));
    fin_rep.tag = zn_reply_t_Tag_FINAL;
    // Trigger the final query handler
    pen_qry->callback(fin_rep, pen_qry->arg);

    __unsafe_zn_unregister_pending_query(zn, pen_qry);

EXIT_QRY_TRIG_FIN:
    // Release the lock
    z_mutex_unlock(&zn->mutex_inner);
}
