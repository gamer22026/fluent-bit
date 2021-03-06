/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2017 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_hash.h>
#include <fluent-bit/flb_regex.h>
#include <fluent-bit/flb_io.h>
#include <fluent-bit/flb_upstream.h>
#include <fluent-bit/flb_http_client.h>
#include <fluent-bit/flb_pack.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <msgpack.h>
#include "kube_conf.h"
#include "kube_meta.h"

static int file_to_buffer(char *path, char **out_buf, size_t *out_size)
{
    int ret;
    char *buf;
    ssize_t bytes;
    FILE *fp;
    struct stat st;

    if (!(fp = fopen(path, "r"))) {
        return -1;
    }

    ret = stat(path, &st);
    if (ret == -1) {
        flb_errno();
        fclose(fp);
        return -1;
    }

    buf = flb_calloc(1, (st.st_size + 1));
    if (!buf) {
        flb_errno();
        fclose(fp);
        return -1;
    }

    bytes = fread(buf, st.st_size, 1, fp);
    if (bytes < 1) {
        flb_free(buf);
        fclose(fp);
        return -1;
    }

    fclose(fp);

    *out_buf = buf;
    *out_size = st.st_size;

    return 0;
}

/* Load local information from a POD context */
static int get_local_pod_info(struct flb_kube *ctx)
{
    int ret;
    char *ns;
    size_t ns_size;
    char *tk = NULL;
    size_t tk_size = 0;
    char *hostname;

    /* Get the namespace name */
    ret = file_to_buffer(FLB_KUBE_NAMESPACE, &ns, &ns_size);
    if (ret == -1) {
        /*
         * If it fails, it's just informational, as likely the caller
         * wanted to connect using the Proxy instead from inside a POD.
         */
        flb_error("[filter_kube] cannot open %s", FLB_KUBE_NAMESPACE);
        return FLB_FALSE;
    }

    /* If a namespace was recognized, a token is mandatory */
    ret = file_to_buffer(ctx->token_file, &tk, &tk_size);
    if (ret == -1) {
        flb_warn("[filter_kube] cannot open %s", FLB_KUBE_TOKEN);
    }

    /* Namespace */
    ctx->namespace = ns;
    ctx->namespace_len = ns_size;

    /* POD Name */
    hostname = getenv("HOSTNAME");
    if (hostname) {
        ctx->podname = flb_strdup(hostname);
        ctx->podname_len = strlen(ctx->podname);
    }
    else {
        char tmp[256];
        gethostname(tmp, 256);
        ctx->podname = flb_strdup(tmp);
        ctx->podname_len = strlen(ctx->podname);
    }

    /* Token */
    ctx->token = tk;
    ctx->token_len = tk_size;

    /* HTTP Auth Header */
    ctx->auth = flb_malloc(tk_size + 32);
    if (!ctx->auth) {
        return FLB_FALSE;
    }
    ctx->auth_len = snprintf(ctx->auth, tk_size + 32,
                             "Bearer %s",
                             tk);
    return FLB_TRUE;
}

/* Gather metadata from API Server */
static int get_api_server_info(struct flb_kube *ctx,
                               char *namespace, char *podname,
                               char **out_buf, size_t *out_size)
{
    int ret;
    size_t b_sent;
    char uri[1024];
    char *buf;
    int size;
    struct flb_http_client *c;
    struct flb_upstream_conn *u_conn;

    if (!ctx->upstream) {
        return -1;
    }

    u_conn = flb_upstream_conn_get(ctx->upstream);
    if (!u_conn) {
        flb_error("[filter_kube] upstream connection error");
        return -1;
    }

    ret = snprintf(uri, sizeof(uri) - 1,
                   FLB_KUBE_API_FMT,
                   namespace, podname);
    if (ret == -1) {
        flb_upstream_conn_release(u_conn);
        return -1;
    }

    /* Compose HTTP Client request */
    c = flb_http_client(u_conn, FLB_HTTP_GET,
                        uri,
                        NULL, 0, NULL, 0, NULL, 0);
    flb_http_add_header(c, "User-Agent", 10, "Fluent-Bit", 10);
    flb_http_add_header(c, "Connection", 10, "close", 5);
    flb_http_add_header(c, "Authorization", 13, ctx->auth, ctx->auth_len);

    /* Perform request */
    ret = flb_http_do(c, &b_sent);
    flb_debug("[filter_kube] API Server (ns=%s, pod=%s) http_do=%i, HTTP Status: %i",
              namespace, podname, ret, c->resp.status);

    if (ret != 0 || c->resp.status != 200) {
        flb_http_client_destroy(c);
        flb_upstream_conn_release(u_conn);
        return -1;
    }

    ret = flb_pack_json(c->resp.payload, c->resp.payload_size,
                        &buf, &size);

    /* release resources */
    flb_http_client_destroy(c);
    flb_upstream_conn_release(u_conn);

    /* validate pack */
    if (ret == -1) {
        return -1;
    }

    *out_buf = buf;
    *out_size = size;

    return 0;
}

static void cb_results(unsigned char *name, unsigned char *value,
                       size_t vlen, void *data)
{
    int len;
    struct flb_kube_meta *meta = data;

    if (meta->podname == NULL && strcmp((char *) name, "pod_name") == 0) {
        meta->podname = flb_strndup((char *) value, vlen);
        meta->podname_len = vlen;
    }
    else if (meta->namespace == NULL &&
             strcmp((char *) name, "namespace_name") == 0) {
        meta->namespace = flb_strndup((char *) value, vlen);
        meta->namespace_len = vlen;
    }

    len = strlen((char *)name);
    msgpack_pack_str(&meta->mp_pck, len);
    msgpack_pack_str_body(&meta->mp_pck, (char *) name, len);
    msgpack_pack_str(&meta->mp_pck, vlen);
    msgpack_pack_str_body(&meta->mp_pck, (char *) value, vlen);
}

static int merge_meta(char *reg_buf, size_t reg_size,
                      char *api_buf, size_t api_size,
                      char **out_buf, size_t *out_size)
{
    int i;
    int ret;
    int map_size;
    int meta_found = FLB_FALSE;
    int have_uid = -1;
    int have_labels = -1;
    int have_annotations = -1;
    size_t off = 0;
    msgpack_sbuffer mp_sbuf;
    msgpack_packer mp_pck;

    msgpack_unpacked result;
    msgpack_unpacked api_result;
    msgpack_unpacked meta_result;
    msgpack_object k;
    msgpack_object v;
    msgpack_object meta_val;
    msgpack_object map;
    msgpack_object api_map;

    /*
     * - reg_buf: is a msgpack Map containing meta captured using Regex
     *
     * - api_buf: metadata associated to namespace and POD Name coming from
     *            the API server.
     *
     * When merging data we aim to add the following keys from the API server:
     *
     * - pod_id
     * - labels
     * - annotations
     */

    /* Initialize output msgpack buffer */
    msgpack_sbuffer_init(&mp_sbuf);
    msgpack_packer_init(&mp_pck, &mp_sbuf, msgpack_sbuffer_write);

    /* Get current size of reg_buf map */
    msgpack_unpacked_init(&result);
    ret = msgpack_unpack_next(&result, reg_buf, reg_size, &off);
    if (ret != MSGPACK_UNPACK_SUCCESS) {
        msgpack_sbuffer_destroy(&mp_sbuf);
        msgpack_unpacked_destroy(&result);
        return -1;
    }
    map = result.data;

    /* Check map */
    if (map.type != MSGPACK_OBJECT_MAP) {
        msgpack_sbuffer_destroy(&mp_sbuf);
        msgpack_unpacked_destroy(&result);
        return -1;
    }

    /* Set map size: current + pod_id, labels and annotations */
    map_size = map.via.map.size;

    /* Iterate API server msgpack and lookup specific fields */
    off = 0;
    msgpack_unpacked_init(&api_result);
    ret = msgpack_unpack_next(&api_result, api_buf, api_size, &off);
    if (ret != MSGPACK_UNPACK_SUCCESS) {
        msgpack_sbuffer_destroy(&mp_sbuf);
        msgpack_unpacked_destroy(&result);
        msgpack_unpacked_destroy(&api_result);
        return -1;
    }

    api_map = api_result.data;

    /* At this point map points to the ROOT map, eg:
     *
     * {
     *  "kind": "Pod",
     *  "apiVersion": "v1",
     *  "metadata": {
     *    "name": "fluent-bit-rz47v",
     *    "generateName": "fluent-bit-",
     *    "namespace": "kube-system",
     *    "selfLink": "/api/v1/namespaces/kube-system/pods/fluent-bit-rz47v",
     *  ....
     * }
     *
     * We are interested into the 'metadata' map value.
     */
    for (i = 0; i < api_map.via.map.size; i++) {
        k = api_map.via.map.ptr[i].key;
        if (k.via.str.size == 8 && strncmp(k.via.str.ptr, "metadata", 8) == 0) {
            meta_val = api_map.via.map.ptr[i].val;
            meta_found = FLB_TRUE;
            break;
        }
    }

    if (meta_found == FLB_FALSE) {
        msgpack_unpacked_destroy(&result);
        msgpack_unpacked_destroy(&api_result);
        msgpack_sbuffer_destroy(&mp_sbuf);
        return -1;
    }

    /* Process metadata map value */
    msgpack_unpacked_init(&meta_result);
    for (i = 0; i < meta_val.via.map.size; i++) {
        k = meta_val.via.map.ptr[i].key;

        char *ptr = (char *) k.via.str.ptr;
        size_t size = k.via.str.size;

        if (size == 3 && strncmp(ptr, "uid", 3) == 0) {
            have_uid = i;
            map_size++;
        }
        else if (size == 6 && strncmp(ptr, "labels", 6) == 0) {
            have_labels = i;
            map_size++;
        }

        else if (size == 11 && strncmp(ptr, "annotations", 11) == 0) {
            have_annotations = i;
            map_size++;
        }

        if (have_uid >= 0 && have_labels >= 0 && have_annotations >= 0) {
            break;
        }
    }

    /* Append Regex fields */
    msgpack_pack_map(&mp_pck, map_size);
    for (i = 0; i < map.via.map.size; i++) {
        k = map.via.map.ptr[i].key;
        v = map.via.map.ptr[i].val;

        msgpack_pack_object(&mp_pck, k);
        msgpack_pack_object(&mp_pck, v);
    }

    /* Append API Server content */
    if (have_uid >= 0) {
        v = meta_val.via.map.ptr[have_uid].val;

        msgpack_pack_str(&mp_pck, 6);
        msgpack_pack_str_body(&mp_pck, "pod_id", 6);
        msgpack_pack_object(&mp_pck, v);
    }

    if (have_labels >= 0) {
        k = meta_val.via.map.ptr[have_labels].key;
        v = meta_val.via.map.ptr[have_labels].val;

        msgpack_pack_object(&mp_pck, k);
        msgpack_pack_object(&mp_pck, v);
    }

    if (have_annotations >= 0) {
        k = meta_val.via.map.ptr[have_annotations].key;
        v = meta_val.via.map.ptr[have_annotations].val;

        msgpack_pack_object(&mp_pck, k);
        msgpack_pack_object(&mp_pck, v);
    }

    msgpack_unpacked_destroy(&result);
    msgpack_unpacked_destroy(&api_result);
    msgpack_unpacked_destroy(&meta_result);

    *out_buf = mp_sbuf.data;
    *out_size = mp_sbuf.size;

    return 0;
}

static inline int tag_to_meta(struct flb_kube *ctx, char *tag, int tag_len,
                              struct flb_kube_meta *meta,
                              char **out_buf, size_t *out_size)
{
    size_t off = 0;
    ssize_t n;
    struct flb_regex_search result;

    msgpack_sbuffer_init(&meta->mp_sbuf);
    msgpack_packer_init(&meta->mp_pck, &meta->mp_sbuf, msgpack_sbuffer_write);

    meta->podname = NULL;
    meta->namespace = NULL;

    n = flb_regex_do(ctx->regex_tag, (unsigned char *) tag, tag_len, &result);
    if (n <= 0) {
        return -1;
    }

    msgpack_pack_map(&meta->mp_pck, n);

    /* Parse the regex results */
    flb_regex_parse(ctx->regex_tag, &result, cb_results, meta);

    /* Compose API server cache key */
    if (meta->podname && meta->namespace) {
        meta->cache_key_len = meta->podname_len + meta->namespace_len + 1;
        meta->cache_key = flb_malloc(meta->cache_key_len + 1);
        if (!meta->cache_key) {
            flb_errno();
            msgpack_sbuffer_destroy(&meta->mp_sbuf);
            return -1;
        }

        /* Copy namespace */
        memcpy(meta->cache_key, meta->namespace, meta->namespace_len);
        off = meta->namespace_len;

        /* Separator */
        meta->cache_key[off++] = ':';

        /* Copy podname */
        memcpy(meta->cache_key + off, meta->podname, meta->podname_len);
        off += meta->podname_len;
        meta->cache_key[off] = '\0';
    }
    else {
        meta->cache_key = NULL;
        meta->cache_key_len = 0;
    }

    /* Set outgoing buffer */
    *out_buf = meta->mp_sbuf.data;
    *out_size = meta->mp_sbuf.size;

    return 0;
}

/*
 * Given a fixed meta data (namespace and podname), get API server information
 * and merge buffers.
 */
static int get_and_merge_meta(struct flb_kube *ctx, struct flb_kube_meta *meta,
                              char *local_buf, size_t local_size,
                              char **out_buf, size_t *out_size)
{
    int ret;
    char *api_buf;
    size_t api_size;
    char *merge_buf;
    size_t merge_size;

    ret = get_api_server_info(ctx,
                              meta->namespace, meta->podname,
                              &api_buf, &api_size);
    if (ret == -1) {
        return -1;
    }

    ret = merge_meta(local_buf, local_size,
                     api_buf, api_size,
                     &merge_buf, &merge_size);
    flb_free(api_buf);

    if (ret == -1) {
        return -1;
    }

    *out_buf = merge_buf;
    *out_size = merge_size;

    return 0;
}


static int flb_kube_network_init(struct flb_kube *ctx, struct flb_config *config)
{
    int io_type = FLB_IO_TCP;

    ctx->upstream = NULL;

    if (ctx->api_https == FLB_TRUE) {
        if (!ctx->tls_ca_file) {
            ctx->tls_ca_file  = flb_strdup(FLB_KUBE_CA);
        }
        ctx->tls.context = flb_tls_context_new(FLB_TRUE,
                                               ctx->tls_ca_file,
                                               NULL, NULL, NULL);
        if (!ctx->tls.context) {
            return -1;
        }
        io_type = FLB_IO_TLS;
    }

    /* Create an Upstream context */
    ctx->upstream = flb_upstream_create(config,
                                        ctx->api_host,
                                        ctx->api_port,
                                        io_type,
                                        &ctx->tls);

    /* Remove async flag from upstream */
    ctx->upstream->flags &= ~(FLB_IO_ASYNC);

    return 0;
}

/* Initialize local context */
int flb_kube_meta_init(struct flb_kube *ctx, struct flb_config *config)
{
    int ret;
    char *meta_buf;
    size_t meta_size;

    /* Gather local info */
    ret = get_local_pod_info(ctx);
    if (ret == FLB_TRUE) {
        flb_info("[filter_kube] local POD info OK");
    }
    else {
        flb_info("[filter_kube] not running in a POD");
    }

    /* Init network */
    flb_kube_network_init(ctx, config);

    /* Gather info from API server */
    flb_info("[filter_kube] testing connectivity with API server...");
    ret = get_api_server_info(ctx, ctx->namespace, ctx->podname,
                              &meta_buf, &meta_size);
    if (ret == -1) {
        flb_error("[filter_kube] could not get meta for POD %s",
                  ctx->podname);
        return -1;
    }
    flb_info("[filter_kube] API server connectivity OK");

    flb_free(meta_buf);

    return 0;
}

int flb_kube_meta_get(struct flb_kube *ctx,
                      char *tag, int tag_len,
                      char **out_buf, size_t *out_size)
{
    int id;
    int ret;
    char *local_meta_buf;
    size_t local_meta_size;
    char *hash_meta_buf;
    size_t hash_meta_size;
    char *out_meta_buf;
    size_t out_meta_size;
    struct flb_kube_meta meta = {};

    /* Get meta from the tag (cache key is the important one) */
    ret = tag_to_meta(ctx, tag, tag_len, &meta,
                      &local_meta_buf, &local_meta_size);
    if (ret != 0) {
        return -1;
    }

    /* Check if we have some data associated to the cache key */
    ret = flb_hash_get(ctx->hash_table,
                       meta.cache_key, meta.cache_key_len,
                       &hash_meta_buf, &hash_meta_size);
    if (ret == -1) {
        /* Retrieve API server meta and merge with local meta */
        ret = get_and_merge_meta(ctx, &meta,
                                 local_meta_buf, local_meta_size,
                                 &out_meta_buf, &out_meta_size);
        if (ret == -1) {
            goto out;
        }

        id = flb_hash_add(ctx->hash_table,
                          meta.cache_key, meta.cache_key_len,
                          out_meta_buf, out_meta_size);
        if (id >= 0) {
            /*
             * Release the original buffer created on tag_to_meta() as a new
             * copy have been generated into the hash table, then re-set
             * the outgoing buffer and size.
             */
            flb_free(out_meta_buf);
            flb_hash_get_by_id(ctx->hash_table, id, out_buf, out_size);
            goto out;
        }
    }
    else {
        *out_buf = hash_meta_buf;
        *out_size = hash_meta_size;
    }

 out:
    msgpack_sbuffer_destroy(&meta.mp_sbuf);
    flb_free(meta.cache_key);
    flb_free(meta.podname);
    flb_free(meta.namespace);

    return 0;
}
