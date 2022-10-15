/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stddef.h>

#define APR_POOL_DEBUG  7

#include <httpd.h>
#include <http_core.h>
#include <http_connection.h>
#include <http_log.h>

#include <nghttp2/nghttp2.h>

#include "h2_private.h"
#include "h2_mplx.h"
#include "h2_request.h"
#include "h2_response.h"
#include "h2_stream.h"
#include "h2_task.h"
#include "h2_ctx.h"
#include "h2_task_input.h"
#include "h2_task.h"
#include "h2_util.h"


static void set_state(h2_stream *stream, h2_stream_state_t state)
{
    AP_DEBUG_ASSERT(stream);
    if (stream->state != state) {
        h2_stream_state_t oldstate = stream->state;
        stream->state = state;
    }
}

h2_stream *h2_stream_create(int id, apr_pool_t *master, 
                            apr_bucket_alloc_t *bucket_alloc, 
                            struct h2_mplx *m)
{
    apr_pool_t *spool = NULL;
    apr_status_t status = apr_pool_create(&spool, master);
    if (status != APR_SUCCESS) {
        return NULL;
    }
    
    h2_stream *stream = apr_pcalloc(spool, sizeof(h2_stream));
    if (stream != NULL) {
        stream->id = id;
        stream->state = H2_STREAM_ST_IDLE;
        stream->pool = spool;
        stream->bucket_alloc = bucket_alloc;
        stream->m = m;
        stream->request = h2_request_create(id, spool, stream->bucket_alloc, m);
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, h2_mplx_get_conn(m),
                      "h2_stream(%ld-%d): created",
                      h2_mplx_get_id(stream->m), stream->id);
    }
    return stream;
}

void h2_stream_cleanup(h2_stream *stream)
{
    h2_request_destroy(stream->request);
    h2_mplx_close_io(stream->m, stream->id);
}

apr_status_t h2_stream_destroy(h2_stream *stream)
{
    AP_DEBUG_ASSERT(stream);
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, h2_mplx_get_conn(stream->m),
                  "h2_stream(%ld-%d): destroy",
                  h2_mplx_get_id(stream->m), stream->id);
    h2_stream_cleanup(stream);
    
    stream->m = NULL;
    if (stream->task) {
        h2_task_destroy(stream->task);
        stream->task = NULL;
    }
    if (stream->pool) {
        apr_pool_destroy(stream->pool);
    }
    return APR_SUCCESS;
}

int h2_stream_get_id(h2_stream *stream)
{
    AP_DEBUG_ASSERT(stream);
    return stream->id;
}

void h2_stream_abort(h2_stream *stream)
{
    AP_DEBUG_ASSERT(stream);
    stream->aborted = 1;
}

apr_status_t h2_stream_set_response(h2_stream *stream, h2_response *response)
{
    stream->response = response;
    return APR_SUCCESS;
}

h2_task *h2_stream_create_task(h2_stream *stream, conn_rec *master)
{
    AP_DEBUG_ASSERT(stream);
    return stream->task;
}

static int set_closed(h2_stream *stream) 
{
    switch (stream->state) {
        case H2_STREAM_ST_CLOSED_INPUT:
        case H2_STREAM_ST_CLOSED:
            return 0; /* ignore, idempotent */
        case H2_STREAM_ST_CLOSED_OUTPUT:
            /* both closed now */
            set_state(stream, H2_STREAM_ST_CLOSED);
            break;
        default:
            /* everything else we jump to here */
            set_state(stream, H2_STREAM_ST_CLOSED_INPUT);
            break;
    }
    return 1;
}

apr_status_t h2_stream_rwrite(h2_stream *stream, request_rec *r)
{
    AP_DEBUG_ASSERT(stream);
    set_state(stream, H2_STREAM_ST_OPEN);
    apr_status_t status = h2_request_rwrite(stream->request, r, stream->m);
    return status;
}

apr_status_t h2_stream_write_eoh(h2_stream *stream, int eos)
{
    AP_DEBUG_ASSERT(stream);
    conn_rec *c = h2_mplx_get_conn(stream->m);
    stream->task = h2_task_create(h2_mplx_get_id(stream->m), stream->id, 
                                  c, stream->pool, stream->m);
    
    apr_status_t status = h2_request_end_headers(stream->request, 
                                                 stream->m, stream->task, eos);
    if (status == APR_SUCCESS) {
        status = h2_mplx_do_async(stream->m, stream->id, stream->task);
    }
    if (eos) {
        status = h2_stream_write_eos(stream);
    }
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, c,
                  "h2_stream(%ld-%d): end header, task %s %s (%s)",
                  h2_mplx_get_id(stream->m), stream->id,
                  stream->request->method, stream->request->path,
                  stream->request->authority);
    
    return status;
}

apr_status_t h2_stream_write_eos(h2_stream *stream)
{
    AP_DEBUG_ASSERT(stream);
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, h2_mplx_get_conn(stream->m),
                  "h2_stream(%ld-%d): closing input",
                  h2_mplx_get_id(stream->m), stream->id);
    if (set_closed(stream)) {
        return h2_request_close(stream->request, stream->m);
    }
    return APR_SUCCESS;
}

apr_status_t h2_stream_write_header(h2_stream *stream,
                                    const char *name, size_t nlen,
                                    const char *value, size_t vlen)
{
    AP_DEBUG_ASSERT(stream);
    switch (stream->state) {
        case H2_STREAM_ST_IDLE:
            set_state(stream, H2_STREAM_ST_OPEN);
            break;
        case H2_STREAM_ST_OPEN:
            break;
        default:
            return APR_EINVAL;
    }
    return h2_request_write_header(stream->request, name, nlen,
                                   value, vlen, stream->m);
}

apr_status_t h2_stream_write_data(h2_stream *stream,
                                  const char *data, size_t len)
{
    AP_DEBUG_ASSERT(stream);
    AP_DEBUG_ASSERT(stream);
    switch (stream->state) {
        case H2_STREAM_ST_OPEN:
            break;
        default:
            return APR_EINVAL;
    }
    return h2_request_write_data(stream->request, data, len, stream->m);
}

apr_status_t h2_stream_prep_read(h2_stream *stream, 
                                 apr_size_t *plen, int *peos)
{
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, 
                  h2_mplx_get_conn(stream->m),
                  "h2_stream(%ld-%d): prep_read from mplx",
                  h2_mplx_get_id(stream->m), stream->id);
    
    apr_status_t status = h2_mplx_out_read(stream->m, stream->id, 
                                           NULL, plen, peos);
    if (status == APR_SUCCESS && !*peos && !plen) {
        return APR_EAGAIN;
    }
    return status;
}

apr_status_t h2_stream_read(h2_stream *stream, char *buffer, 
                            apr_size_t *plen, int *peos)
{
    return h2_mplx_out_read(stream->m, stream->id, buffer, plen, peos);
}

void h2_stream_set_suspended(h2_stream *stream, int suspended)
{
    AP_DEBUG_ASSERT(stream);
    stream->suspended = !!suspended;
}

int h2_stream_is_suspended(h2_stream *stream)
{
    AP_DEBUG_ASSERT(stream);
    return stream->suspended;
}

