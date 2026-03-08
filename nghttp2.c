#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "Zend/zend_exceptions.h"

#include "php_nghttp2.h"

#include <nghttp2/nghttp2.h>

#include <stdint.h>

/* Class entries */
static zend_class_entry *ce_session;
static zend_class_entry *ce_session_options;
static zend_class_entry *ce_request_head;
static zend_class_entry *ce_response_head;

static zend_class_entry *ce_event;
static zend_class_entry *ce_event_headers_received;
static zend_class_entry *ce_event_data_received;
static zend_class_entry *ce_event_stream_closed;
static zend_class_entry *ce_event_stream_reset;
static zend_class_entry *ce_event_goaway_received;
static zend_class_entry *ce_event_settings_received;
static zend_class_entry *ce_event_settings_acked;

static zend_class_entry *ce_exception;
static zend_class_entry *ce_runtime_exception;
static zend_class_entry *ce_protocol_exception;

/* Session role constants */
#define PHP_NGHTTP2_ROLE_CLIENT 1
#define PHP_NGHTTP2_ROLE_SERVER 2

typedef struct _php_nghttp2_event_node {
	zval event;
	struct _php_nghttp2_event_node *next;
} php_nghttp2_event_node;

typedef struct _php_nghttp2_output_node {
	zend_string *chunk;
	struct _php_nghttp2_output_node *next;
} php_nghttp2_output_node;

typedef struct _php_nghttp2_header_bucket {
	int32_t stream_id;
	zval headers;
	struct _php_nghttp2_header_bucket *next;
} php_nghttp2_header_bucket;

typedef struct _php_nghttp2_data_source_ctx {
	int32_t stream_id;
	zend_string *payload;
	size_t offset;
	struct _php_nghttp2_data_source_ctx *next;
} php_nghttp2_data_source_ctx;

typedef struct _php_nghttp2_session_obj {
	nghttp2_session *session;
	nghttp2_session_callbacks *callbacks;
	zend_long role;
	zend_bool is_shutdown;

	php_nghttp2_event_node *event_head;
	php_nghttp2_event_node *event_tail;

	php_nghttp2_output_node *output_head;
	php_nghttp2_output_node *output_tail;

	php_nghttp2_header_bucket *header_buckets;
	php_nghttp2_data_source_ctx *data_sources;

	zend_object std;
} php_nghttp2_session_obj;

static zend_object_handlers php_nghttp2_session_handlers;

static inline php_nghttp2_session_obj *php_nghttp2_session_from_obj(zend_object *obj)
{
	return (php_nghttp2_session_obj *) ((char *) obj - XtOffsetOf(php_nghttp2_session_obj, std));
}

#define Z_NGHTTP2_SESSION_P(zv) php_nghttp2_session_from_obj(Z_OBJ_P((zv)))

static void php_nghttp2_throw_error(zend_class_entry *error_ce, const char *context, int code)
{
	zend_throw_exception_ex(error_ce, code, "%s: (%d) %s", context, code, nghttp2_strerror(code));
}

static void php_nghttp2_enqueue_output(php_nghttp2_session_obj *obj, const uint8_t *data, size_t len)
{
	php_nghttp2_output_node *node;

	if (len == 0) {
		return;
	}

	node = emalloc(sizeof(*node));
	node->chunk = zend_string_init((const char *) data, len, 0);
	node->next = NULL;

	if (obj->output_tail) {
		obj->output_tail->next = node;
	} else {
		obj->output_head = node;
	}
	obj->output_tail = node;
}

static void php_nghttp2_enqueue_event(php_nghttp2_session_obj *obj, zval *event)
{
	php_nghttp2_event_node *node = emalloc(sizeof(*node));
	node->next = NULL;
	ZVAL_COPY_VALUE(&node->event, event);
	ZVAL_UNDEF(event);

	if (obj->event_tail) {
		obj->event_tail->next = node;
	} else {
		obj->event_head = node;
	}
	obj->event_tail = node;
}

static php_nghttp2_header_bucket *php_nghttp2_find_header_bucket(php_nghttp2_session_obj *obj, int32_t stream_id)
{
	php_nghttp2_header_bucket *bucket = obj->header_buckets;
	while (bucket) {
		if (bucket->stream_id == stream_id) {
			return bucket;
		}
		bucket = bucket->next;
	}
	return NULL;
}

static php_nghttp2_header_bucket *php_nghttp2_get_or_create_header_bucket(php_nghttp2_session_obj *obj, int32_t stream_id)
{
	php_nghttp2_header_bucket *bucket = php_nghttp2_find_header_bucket(obj, stream_id);

	if (bucket) {
		return bucket;
	}

	bucket = ecalloc(1, sizeof(*bucket));
	bucket->stream_id = stream_id;
	array_init(&bucket->headers);
	bucket->next = obj->header_buckets;
	obj->header_buckets = bucket;
	return bucket;
}

static php_nghttp2_header_bucket *php_nghttp2_take_header_bucket(php_nghttp2_session_obj *obj, int32_t stream_id)
{
	php_nghttp2_header_bucket *prev = NULL;
	php_nghttp2_header_bucket *cur = obj->header_buckets;

	while (cur) {
		if (cur->stream_id == stream_id) {
			if (prev) {
				prev->next = cur->next;
			} else {
				obj->header_buckets = cur->next;
			}
			cur->next = NULL;
			return cur;
		}
		prev = cur;
		cur = cur->next;
	}

	return NULL;
}

static void php_nghttp2_free_header_bucket(php_nghttp2_header_bucket *bucket)
{
	if (!bucket) {
		return;
	}
	zval_ptr_dtor(&bucket->headers);
	efree(bucket);
}

static void php_nghttp2_data_source_free(php_nghttp2_data_source_ctx *ctx)
{
	if (!ctx) {
		return;
	}
	if (ctx->payload) {
		zend_string_release(ctx->payload);
	}
	efree(ctx);
}

static void php_nghttp2_data_source_remove(php_nghttp2_session_obj *obj, php_nghttp2_data_source_ctx *target)
{
	php_nghttp2_data_source_ctx *prev = NULL;
	php_nghttp2_data_source_ctx *cur = obj->data_sources;

	while (cur) {
		if (cur == target) {
			if (prev) {
				prev->next = cur->next;
			} else {
				obj->data_sources = cur->next;
			}
			php_nghttp2_data_source_free(cur);
			return;
		}
		prev = cur;
		cur = cur->next;
	}
}

static void php_nghttp2_data_source_cleanup_stream(php_nghttp2_session_obj *obj, int32_t stream_id)
{
	php_nghttp2_data_source_ctx *cur = obj->data_sources;
	php_nghttp2_data_source_ctx *next;
	php_nghttp2_data_source_ctx *prev = NULL;

	while (cur) {
		next = cur->next;
		if (cur->stream_id == stream_id) {
			if (prev) {
				prev->next = next;
			} else {
				obj->data_sources = next;
			}
			php_nghttp2_data_source_free(cur);
		} else {
			prev = cur;
		}
		cur = next;
	}
}

static void php_nghttp2_data_source_clear_all(php_nghttp2_session_obj *obj)
{
	php_nghttp2_data_source_ctx *cur = obj->data_sources;
	php_nghttp2_data_source_ctx *next;
	while (cur) {
		next = cur->next;
		php_nghttp2_data_source_free(cur);
		cur = next;
	}
	obj->data_sources = NULL;
}

static zend_result php_nghttp2_flush_send(php_nghttp2_session_obj *obj, const char *context)
{
	int rv;

	if (!obj->session) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "Session is not initialized");
		return FAILURE;
	}

	rv = nghttp2_session_send(obj->session);
	if (rv != 0) {
		php_nghttp2_throw_error(ce_runtime_exception, context, rv);
		return FAILURE;
	}

	return SUCCESS;
}

static zend_result php_nghttp2_require_open_session(php_nghttp2_session_obj *obj)
{
	if (!obj->session) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "Session is not initialized");
		return FAILURE;
	}

	if (obj->is_shutdown) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "Session is already shut down");
		return FAILURE;
	}

	return SUCCESS;
}

static const char *php_nghttp2_stream_state_to_string(nghttp2_stream_proto_state state)
{
	switch (state) {
		case NGHTTP2_STREAM_STATE_IDLE:
			return "idle";
		case NGHTTP2_STREAM_STATE_RESERVED_LOCAL:
			return "reserved_local";
		case NGHTTP2_STREAM_STATE_RESERVED_REMOTE:
			return "reserved_remote";
		case NGHTTP2_STREAM_STATE_OPEN:
			return "open";
		case NGHTTP2_STREAM_STATE_HALF_CLOSED_LOCAL:
			return "half_closed_local";
		case NGHTTP2_STREAM_STATE_HALF_CLOSED_REMOTE:
			return "half_closed_remote";
		case NGHTTP2_STREAM_STATE_CLOSED:
			return "closed";
		default:
			return NULL;
	}
}

static zend_long php_nghttp2_count_active_streams_recursive(nghttp2_stream *stream)
{
	nghttp2_stream *child;
	zend_long total = 0;

	for (child = nghttp2_stream_get_first_child(stream); child; child = nghttp2_stream_get_next_sibling(child)) {
		nghttp2_stream_proto_state state = nghttp2_stream_get_state(child);

		if (state == NGHTTP2_STREAM_STATE_OPEN ||
		    state == NGHTTP2_STREAM_STATE_HALF_CLOSED_LOCAL ||
		    state == NGHTTP2_STREAM_STATE_HALF_CLOSED_REMOTE) {
			total++;
		}

		total += php_nghttp2_count_active_streams_recursive(child);
	}

	return total;
}

static ssize_t php_nghttp2_send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
	php_nghttp2_session_obj *obj = (php_nghttp2_session_obj *) user_data;
	(void) session;
	(void) flags;

	if (!obj) {
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}

	php_nghttp2_enqueue_output(obj, data, length);
	return (ssize_t) length;
}

static ssize_t php_nghttp2_data_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,
	uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
	php_nghttp2_data_source_ctx *ctx = (php_nghttp2_data_source_ctx *) source->ptr;
	php_nghttp2_session_obj *obj = (php_nghttp2_session_obj *) user_data;
	size_t remain;
	size_t ncopy;
	(void) session;
	(void) stream_id;

	if (!ctx || !obj) {
		return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
	}

	remain = ZSTR_LEN(ctx->payload) - ctx->offset;
	ncopy = remain < length ? remain : length;

	if (ncopy > 0) {
		memcpy(buf, ZSTR_VAL(ctx->payload) + ctx->offset, ncopy);
		ctx->offset += ncopy;
	}

	if (ctx->offset >= ZSTR_LEN(ctx->payload)) {
		*data_flags |= NGHTTP2_DATA_FLAG_EOF;
		php_nghttp2_data_source_remove(obj, ctx);
	}

	return (ssize_t) ncopy;
}

static void php_nghttp2_event_add_common_stream_id(zval *event, zend_class_entry *ce, int32_t stream_id)
{
	zend_update_property_long(ce, Z_OBJ_P(event), ZEND_STRL("streamId"), (zend_long) stream_id);
}

static int php_nghttp2_on_header_callback(nghttp2_session *session, const nghttp2_frame *frame,
	const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen,
	uint8_t flags, void *user_data)
{
	php_nghttp2_session_obj *obj = (php_nghttp2_session_obj *) user_data;
	php_nghttp2_header_bucket *bucket;
	zval zv;
	(void) session;
	(void) flags;

	if (!obj || frame->hd.type != NGHTTP2_HEADERS) {
		return 0;
	}

	bucket = php_nghttp2_get_or_create_header_bucket(obj, frame->hd.stream_id);
	ZVAL_STRINGL(&zv, (const char *) value, valuelen);
	zend_hash_str_update(Z_ARRVAL(bucket->headers), (const char *) name, namelen, &zv);

	return 0;
}

static int php_nghttp2_on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id,
	const uint8_t *data, size_t len, void *user_data)
{
	php_nghttp2_session_obj *obj = (php_nghttp2_session_obj *) user_data;
	zval event;
	(void) session;

	if (!obj) {
		return 0;
	}

	object_init_ex(&event, ce_event_data_received);
	php_nghttp2_event_add_common_stream_id(&event, ce_event_data_received, stream_id);
	zend_update_property_stringl(ce_event_data_received, Z_OBJ(event), ZEND_STRL("data"), (const char *) data, len);
	zend_update_property_bool(ce_event_data_received, Z_OBJ(event), ZEND_STRL("endStream"), (flags & NGHTTP2_FLAG_END_STREAM) != 0);
	php_nghttp2_enqueue_event(obj, &event);

	return 0;
}

static int php_nghttp2_on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code,
	void *user_data)
{
	php_nghttp2_session_obj *obj = (php_nghttp2_session_obj *) user_data;
	php_nghttp2_header_bucket *bucket;
	zval event;
	(void) session;

	if (!obj) {
		return 0;
	}

	object_init_ex(&event, ce_event_stream_closed);
	php_nghttp2_event_add_common_stream_id(&event, ce_event_stream_closed, stream_id);
	zend_update_property_long(ce_event_stream_closed, Z_OBJ(event), ZEND_STRL("errorCode"), (zend_long) error_code);
	php_nghttp2_enqueue_event(obj, &event);

	php_nghttp2_data_source_cleanup_stream(obj, stream_id);
	bucket = php_nghttp2_take_header_bucket(obj, stream_id);
	php_nghttp2_free_header_bucket(bucket);

	return 0;
}

static int php_nghttp2_on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
	php_nghttp2_session_obj *obj = (php_nghttp2_session_obj *) user_data;
	zval event;
	(void) session;

	if (!obj) {
		return 0;
	}

	switch (frame->hd.type) {
		case NGHTTP2_HEADERS: {
			php_nghttp2_header_bucket *bucket = php_nghttp2_take_header_bucket(obj, frame->hd.stream_id);
			object_init_ex(&event, ce_event_headers_received);
			php_nghttp2_event_add_common_stream_id(&event, ce_event_headers_received, frame->hd.stream_id);
			zend_update_property_bool(ce_event_headers_received, Z_OBJ(event), ZEND_STRL("endStream"), (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0);
			if (bucket) {
				zend_update_property(ce_event_headers_received, Z_OBJ(event), ZEND_STRL("headers"), &bucket->headers);
				php_nghttp2_free_header_bucket(bucket);
			} else {
				zval empty;
				array_init(&empty);
				zend_update_property(ce_event_headers_received, Z_OBJ(event), ZEND_STRL("headers"), &empty);
				zval_ptr_dtor(&empty);
			}
			php_nghttp2_enqueue_event(obj, &event);
			break;
		}
		case NGHTTP2_RST_STREAM:
			object_init_ex(&event, ce_event_stream_reset);
			php_nghttp2_event_add_common_stream_id(&event, ce_event_stream_reset, frame->hd.stream_id);
			zend_update_property_long(ce_event_stream_reset, Z_OBJ(event), ZEND_STRL("errorCode"), (zend_long) frame->rst_stream.error_code);
			php_nghttp2_enqueue_event(obj, &event);
			break;
		case NGHTTP2_GOAWAY:
			object_init_ex(&event, ce_event_goaway_received);
			zend_update_property_long(ce_event_goaway_received, Z_OBJ(event), ZEND_STRL("lastStreamId"), (zend_long) frame->goaway.last_stream_id);
			zend_update_property_long(ce_event_goaway_received, Z_OBJ(event), ZEND_STRL("errorCode"), (zend_long) frame->goaway.error_code);
			php_nghttp2_enqueue_event(obj, &event);
			break;
		case NGHTTP2_SETTINGS:
			if ((frame->hd.flags & NGHTTP2_FLAG_ACK) != 0) {
				object_init_ex(&event, ce_event_settings_acked);
				php_nghttp2_enqueue_event(obj, &event);
			} else {
				zval settings;
				size_t i;
				object_init_ex(&event, ce_event_settings_received);
				array_init(&settings);
				for (i = 0; i < frame->settings.niv; i++) {
					zval entry;
					array_init(&entry);
					add_assoc_long(&entry, "id", (zend_long) frame->settings.iv[i].settings_id);
					add_assoc_long(&entry, "value", (zend_long) frame->settings.iv[i].value);
					add_next_index_zval(&settings, &entry);
				}
				zend_update_property(ce_event_settings_received, Z_OBJ(event), ZEND_STRL("settings"), &settings);
				zval_ptr_dtor(&settings);
				php_nghttp2_enqueue_event(obj, &event);
			}
			break;
		default:
			break;
	}

	return 0;
}

static void php_nghttp2_free_nv(nghttp2_nv *nva, size_t nvlen)
{
	size_t i;

	if (!nva) {
		return;
	}

	for (i = 0; i < nvlen; i++) {
		if (nva[i].name) {
			efree(nva[i].name);
		}
		if (nva[i].value) {
			efree(nva[i].value);
		}
	}
	efree(nva);
}

static zend_result php_nghttp2_nv_set(nghttp2_nv *nv, zend_string *name, zend_string *value)
{
	nv->name = (uint8_t *) estrndup(ZSTR_VAL(name), ZSTR_LEN(name));
	nv->value = (uint8_t *) estrndup(ZSTR_VAL(value), ZSTR_LEN(value));

	if (!nv->name || !nv->value) {
		if (nv->name) {
			efree(nv->name);
			nv->name = NULL;
		}
		if (nv->value) {
			efree(nv->value);
			nv->value = NULL;
		}
		return FAILURE;
	}

	nv->namelen = ZSTR_LEN(name);
	nv->valuelen = ZSTR_LEN(value);
	nv->flags = NGHTTP2_NV_FLAG_NONE;
	return SUCCESS;
}

static zend_result php_nghttp2_append_headers_from_array(nghttp2_nv *nva, size_t *index, zval *headers)
{
	zval *entry;
	zend_string *key;
	zend_ulong idx;

	if (Z_TYPE_P(headers) != IS_ARRAY) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "headers must be an array");
		return FAILURE;
	}

	ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(headers), idx, key, entry) {
		zval tmp;
		if (!key) {
			zend_throw_exception_ex(ce_runtime_exception, 0, "headers key must be a string, integer key %lu given", idx);
			return FAILURE;
		}

		ZVAL_COPY(&tmp, entry);
		convert_to_string(&tmp);
		if (php_nghttp2_nv_set(&nva[*index], key, Z_STR(tmp)) != SUCCESS) {
			zval_ptr_dtor(&tmp);
			zend_throw_exception_ex(ce_runtime_exception, 0, "Failed to allocate header memory");
			return FAILURE;
		}
		(*index)++;
		zval_ptr_dtor(&tmp);
	} ZEND_HASH_FOREACH_END();

	return SUCCESS;
}

static zend_result php_nghttp2_build_request_nva(zval *head, nghttp2_nv **out_nva, size_t *out_len, zend_bool *end_stream)
{
	zval rv;
	zval *method;
	zval *scheme;
	zval *authority;
	zval *path;
	zval *headers;
	zval *z_end_stream;
	size_t extra_headers;
	size_t total;
	size_t index = 0;
	nghttp2_nv *nva;
	zend_string *zs_name;

	*out_nva = NULL;
	*out_len = 0;
	*end_stream = 0;

	method = zend_read_property(ce_request_head, Z_OBJ_P(head), ZEND_STRL("method"), 1, &rv);
	scheme = zend_read_property(ce_request_head, Z_OBJ_P(head), ZEND_STRL("scheme"), 1, &rv);
	authority = zend_read_property(ce_request_head, Z_OBJ_P(head), ZEND_STRL("authority"), 1, &rv);
	path = zend_read_property(ce_request_head, Z_OBJ_P(head), ZEND_STRL("path"), 1, &rv);
	headers = zend_read_property(ce_request_head, Z_OBJ_P(head), ZEND_STRL("headers"), 1, &rv);
	z_end_stream = zend_read_property(ce_request_head, Z_OBJ_P(head), ZEND_STRL("endStream"), 1, &rv);

	if (Z_TYPE_P(method) != IS_STRING || Z_TYPE_P(scheme) != IS_STRING || Z_TYPE_P(authority) != IS_STRING || Z_TYPE_P(path) != IS_STRING) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "RequestHead properties must be initialized with strings");
		return FAILURE;
	}

	if (Z_TYPE_P(headers) != IS_ARRAY) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "RequestHead::headers must be array");
		return FAILURE;
	}

	extra_headers = zend_hash_num_elements(Z_ARRVAL_P(headers));
	total = 4 + extra_headers;
	nva = ecalloc(total, sizeof(*nva));

	zs_name = zend_string_init(":method", sizeof(":method") - 1, 0);
	if (php_nghttp2_nv_set(&nva[index++], zs_name, Z_STR_P(method)) != SUCCESS) {
		zend_string_release(zs_name);
		php_nghttp2_free_nv(nva, total);
		zend_throw_exception_ex(ce_runtime_exception, 0, "Failed to allocate pseudo header");
		return FAILURE;
	}
	zend_string_release(zs_name);

	zs_name = zend_string_init(":scheme", sizeof(":scheme") - 1, 0);
	if (php_nghttp2_nv_set(&nva[index++], zs_name, Z_STR_P(scheme)) != SUCCESS) {
		zend_string_release(zs_name);
		php_nghttp2_free_nv(nva, total);
		zend_throw_exception_ex(ce_runtime_exception, 0, "Failed to allocate pseudo header");
		return FAILURE;
	}
	zend_string_release(zs_name);

	zs_name = zend_string_init(":authority", sizeof(":authority") - 1, 0);
	if (php_nghttp2_nv_set(&nva[index++], zs_name, Z_STR_P(authority)) != SUCCESS) {
		zend_string_release(zs_name);
		php_nghttp2_free_nv(nva, total);
		zend_throw_exception_ex(ce_runtime_exception, 0, "Failed to allocate pseudo header");
		return FAILURE;
	}
	zend_string_release(zs_name);

	zs_name = zend_string_init(":path", sizeof(":path") - 1, 0);
	if (php_nghttp2_nv_set(&nva[index++], zs_name, Z_STR_P(path)) != SUCCESS) {
		zend_string_release(zs_name);
		php_nghttp2_free_nv(nva, total);
		zend_throw_exception_ex(ce_runtime_exception, 0, "Failed to allocate pseudo header");
		return FAILURE;
	}
	zend_string_release(zs_name);

	if (php_nghttp2_append_headers_from_array(nva, &index, headers) != SUCCESS) {
		php_nghttp2_free_nv(nva, total);
		return FAILURE;
	}

	*end_stream = zend_is_true(z_end_stream);
	*out_nva = nva;
	*out_len = total;
	return SUCCESS;
}

static zend_result php_nghttp2_build_response_nva(zval *head, nghttp2_nv **out_nva, size_t *out_len, zend_bool *end_stream)
{
	zval rv;
	zval *status;
	zval *headers;
	zval *z_end_stream;
	size_t extra_headers;
	size_t total;
	size_t index = 0;
	nghttp2_nv *nva;
	char status_buf[4];
	zend_string *status_value;
	zend_string *status_name;

	*out_nva = NULL;
	*out_len = 0;
	*end_stream = 0;

	status = zend_read_property(ce_response_head, Z_OBJ_P(head), ZEND_STRL("status"), 1, &rv);
	headers = zend_read_property(ce_response_head, Z_OBJ_P(head), ZEND_STRL("headers"), 1, &rv);
	z_end_stream = zend_read_property(ce_response_head, Z_OBJ_P(head), ZEND_STRL("endStream"), 1, &rv);

	if (Z_TYPE_P(status) != IS_LONG) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "ResponseHead::status must be int");
		return FAILURE;
	}

	if (Z_LVAL_P(status) < 100 || Z_LVAL_P(status) > 999) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "ResponseHead::status must be between 100 and 999");
		return FAILURE;
	}

	if (Z_TYPE_P(headers) != IS_ARRAY) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "ResponseHead::headers must be array");
		return FAILURE;
	}

	extra_headers = zend_hash_num_elements(Z_ARRVAL_P(headers));
	total = 1 + extra_headers;
	nva = ecalloc(total, sizeof(*nva));

	snprintf(status_buf, sizeof(status_buf), "%03ld", Z_LVAL_P(status));
	status_name = zend_string_init(":status", sizeof(":status") - 1, 0);
	status_value = zend_string_init(status_buf, sizeof(status_buf) - 1, 0);
	if (php_nghttp2_nv_set(&nva[index++], status_name, status_value) != SUCCESS) {
		zend_string_release(status_name);
		zend_string_release(status_value);
		php_nghttp2_free_nv(nva, total);
		zend_throw_exception_ex(ce_runtime_exception, 0, "Failed to allocate pseudo header");
		return FAILURE;
	}
	zend_string_release(status_name);
	zend_string_release(status_value);

	if (php_nghttp2_append_headers_from_array(nva, &index, headers) != SUCCESS) {
		php_nghttp2_free_nv(nva, total);
		return FAILURE;
	}

	*end_stream = zend_is_true(z_end_stream);
	*out_nva = nva;
	*out_len = total;
	return SUCCESS;
}

static zend_result php_nghttp2_apply_session_options(php_nghttp2_session_obj *obj, zval *options)
{
	nghttp2_settings_entry iv[4];
	size_t niv = 0;
	zval rv;
	zval *value;

	if (options && Z_TYPE_P(options) == IS_OBJECT) {
		value = zend_read_property(ce_session_options, Z_OBJ_P(options), ZEND_STRL("initialWindowSize"), 1, &rv);
		if (Z_TYPE_P(value) == IS_LONG && Z_LVAL_P(value) > 0) {
			iv[niv].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
			iv[niv].value = (uint32_t) Z_LVAL_P(value);
			niv++;
		}

		value = zend_read_property(ce_session_options, Z_OBJ_P(options), ZEND_STRL("maxConcurrentStreams"), 1, &rv);
		if (Z_TYPE_P(value) == IS_LONG && Z_LVAL_P(value) > 0) {
			iv[niv].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
			iv[niv].value = (uint32_t) Z_LVAL_P(value);
			niv++;
		}

		value = zend_read_property(ce_session_options, Z_OBJ_P(options), ZEND_STRL("headerTableSize"), 1, &rv);
		if (Z_TYPE_P(value) == IS_LONG && Z_LVAL_P(value) >= 0) {
			iv[niv].settings_id = NGHTTP2_SETTINGS_HEADER_TABLE_SIZE;
			iv[niv].value = (uint32_t) Z_LVAL_P(value);
			niv++;
		}

		value = zend_read_property(ce_session_options, Z_OBJ_P(options), ZEND_STRL("enablePush"), 1, &rv);
		if (Z_TYPE_P(value) != IS_NULL) {
			iv[niv].settings_id = NGHTTP2_SETTINGS_ENABLE_PUSH;
			iv[niv].value = zend_is_true(value) ? 1 : 0;
			niv++;
		}

		/* TODO: strictValidation maps to nghttp2 option flags in a later phase. */
	}

	if (nghttp2_submit_settings(obj->session, NGHTTP2_FLAG_NONE, iv, niv) != 0) {
		php_nghttp2_throw_error(ce_runtime_exception, "Failed to submit initial settings", -1);
		return FAILURE;
	}

	return SUCCESS;
}

static zend_result php_nghttp2_submit_data_frame(php_nghttp2_session_obj *obj, int32_t stream_id, zend_string *payload, zend_bool end_stream)
{
	php_nghttp2_data_source_ctx *ctx;
	nghttp2_data_provider provider;
	int rv;
	int flags = end_stream ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE;

	ctx = ecalloc(1, sizeof(*ctx));
	ctx->stream_id = stream_id;
	ctx->payload = zend_string_copy(payload);
	ctx->offset = 0;
	ctx->next = obj->data_sources;
	obj->data_sources = ctx;

	provider.source.ptr = ctx;
	provider.read_callback = php_nghttp2_data_read_callback;

	rv = nghttp2_submit_data(obj->session, flags, stream_id, &provider);
	if (rv != 0) {
		php_nghttp2_data_source_remove(obj, ctx);
		php_nghttp2_throw_error(ce_runtime_exception, "Failed to submit DATA frame", rv);
		return FAILURE;
	}

	return php_nghttp2_flush_send(obj, "Failed to flush DATA frame");
}

/* Session methods */
PHP_METHOD(Session, __construct)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	zend_long role;
	zval *options = NULL;
	int rv;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_LONG(role)
		Z_PARAM_OPTIONAL
		Z_PARAM_OBJECT_OF_CLASS_OR_NULL(options, ce_session_options)
	ZEND_PARSE_PARAMETERS_END();

	if (obj->session) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "Session is already initialized");
		RETURN_THROWS();
	}

	if (role != PHP_NGHTTP2_ROLE_CLIENT && role != PHP_NGHTTP2_ROLE_SERVER) {
		zend_argument_value_error(1, "must be Session::ROLE_CLIENT or Session::ROLE_SERVER");
		RETURN_THROWS();
	}

	obj->role = role;

	rv = nghttp2_session_callbacks_new(&obj->callbacks);
	if (rv != 0) {
		php_nghttp2_throw_error(ce_runtime_exception, "Failed to allocate callbacks", rv);
		RETURN_THROWS();
	}

	nghttp2_session_callbacks_set_send_callback(obj->callbacks, php_nghttp2_send_callback);
	nghttp2_session_callbacks_set_on_header_callback(obj->callbacks, php_nghttp2_on_header_callback);
	nghttp2_session_callbacks_set_on_data_chunk_recv_callback(obj->callbacks, php_nghttp2_on_data_chunk_recv_callback);
	nghttp2_session_callbacks_set_on_frame_recv_callback(obj->callbacks, php_nghttp2_on_frame_recv_callback);
	nghttp2_session_callbacks_set_on_stream_close_callback(obj->callbacks, php_nghttp2_on_stream_close_callback);

	if (role == PHP_NGHTTP2_ROLE_CLIENT) {
		rv = nghttp2_session_client_new(&obj->session, obj->callbacks, obj);
	} else {
		rv = nghttp2_session_server_new(&obj->session, obj->callbacks, obj);
	}

	if (rv != 0) {
		php_nghttp2_throw_error(ce_runtime_exception, "Failed to create nghttp2 session", rv);
		RETURN_THROWS();
	}

	if (php_nghttp2_apply_session_options(obj, options) != SUCCESS) {
		RETURN_THROWS();
	}

	if (php_nghttp2_flush_send(obj, "Failed to emit initial frames") != SUCCESS) {
		RETURN_THROWS();
	}
}

PHP_METHOD(Session, receive)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	zend_string *bytes;
	ssize_t rv;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(bytes)
	ZEND_PARSE_PARAMETERS_END();

	if (php_nghttp2_require_open_session(obj) != SUCCESS) {
		RETURN_THROWS();
	}

	rv = nghttp2_session_mem_recv(obj->session, (const uint8_t *) ZSTR_VAL(bytes), ZSTR_LEN(bytes));
	if (rv < 0) {
		php_nghttp2_throw_error(ce_protocol_exception, "Failed to process received bytes", (int) rv);
		RETURN_THROWS();
	}

	if (php_nghttp2_flush_send(obj, "Failed to flush pending frames after receive") != SUCCESS) {
		RETURN_THROWS();
	}

	RETURN_LONG((zend_long) rv);
}

PHP_METHOD(Session, drainOutput)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	php_nghttp2_output_node *node;
	php_nghttp2_output_node *next;

	ZEND_PARSE_PARAMETERS_NONE();

	array_init(return_value);

	node = obj->output_head;
	while (node) {
		next = node->next;
		add_next_index_str(return_value, node->chunk);
		efree(node);
		node = next;
	}

	obj->output_head = NULL;
	obj->output_tail = NULL;
}

PHP_METHOD(Session, nextEvent)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	php_nghttp2_event_node *node;

	ZEND_PARSE_PARAMETERS_NONE();

	node = obj->event_head;
	if (!node) {
		RETURN_NULL();
	}

	obj->event_head = node->next;
	if (!obj->event_head) {
		obj->event_tail = NULL;
	}

	ZVAL_COPY(return_value, &node->event);
	zval_ptr_dtor(&node->event);
	efree(node);
}

PHP_METHOD(Session, hasPendingEvents)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);

	ZEND_PARSE_PARAMETERS_NONE();

	if (!obj->session) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "Session is not initialized");
		RETURN_THROWS();
	}

	RETURN_BOOL(obj->event_head != NULL);
}

PHP_METHOD(Session, hasPendingOutput)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);

	ZEND_PARSE_PARAMETERS_NONE();

	if (!obj->session) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "Session is not initialized");
		RETURN_THROWS();
	}

	RETURN_BOOL(obj->output_head != NULL);
}

PHP_METHOD(Session, getOpenStreamCount)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	nghttp2_stream *root;

	ZEND_PARSE_PARAMETERS_NONE();

	if (!obj->session) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "Session is not initialized");
		RETURN_THROWS();
	}

	root = nghttp2_session_get_root_stream(obj->session);
	if (!root) {
		RETURN_LONG(0);
	}

	RETURN_LONG(php_nghttp2_count_active_streams_recursive(root));
}

PHP_METHOD(Session, getStreamState)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	zend_long stream_id;
	nghttp2_stream *stream;
	const char *state_name;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(stream_id)
	ZEND_PARSE_PARAMETERS_END();

	if (!obj->session) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "Session is not initialized");
		RETURN_THROWS();
	}

	if (stream_id < 0 || stream_id > INT32_MAX) {
		zend_argument_value_error(1, "must be between 0 and 2147483647");
		RETURN_THROWS();
	}

	stream = nghttp2_session_find_stream(obj->session, (int32_t) stream_id);
	if (!stream) {
		RETURN_NULL();
	}

	state_name = php_nghttp2_stream_state_to_string(nghttp2_stream_get_state(stream));
	if (!state_name) {
		RETURN_NULL();
	}

	RETURN_STRING(state_name);
}

PHP_METHOD(Session, submitRequest)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	zval *request_head;
	nghttp2_nv *nva;
	size_t nvlen;
	zend_bool end_stream;
	int32_t stream_id;
	int rv;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OF_CLASS(request_head, ce_request_head)
	ZEND_PARSE_PARAMETERS_END();

	if (php_nghttp2_require_open_session(obj) != SUCCESS) {
		RETURN_THROWS();
	}

	if (php_nghttp2_build_request_nva(request_head, &nva, &nvlen, &end_stream) != SUCCESS) {
		RETURN_THROWS();
	}

	stream_id = nghttp2_submit_headers(obj->session,
		end_stream ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE,
		-1,
		NULL,
		nva,
		nvlen,
		NULL);
	php_nghttp2_free_nv(nva, nvlen);

	if (stream_id < 0) {
		php_nghttp2_throw_error(ce_runtime_exception, "Failed to submit request headers", stream_id);
		RETURN_THROWS();
	}

	rv = php_nghttp2_flush_send(obj, "Failed to flush request headers");
	if (rv != SUCCESS) {
		RETURN_THROWS();
	}

	RETURN_LONG((zend_long) stream_id);
}

PHP_METHOD(Session, submitResponse)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	zend_long stream_id;
	zval *response_head;
	nghttp2_nv *nva;
	size_t nvlen;
	zend_bool end_stream;
	int rv;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_LONG(stream_id)
		Z_PARAM_OBJECT_OF_CLASS(response_head, ce_response_head)
	ZEND_PARSE_PARAMETERS_END();

	if (php_nghttp2_require_open_session(obj) != SUCCESS) {
		RETURN_THROWS();
	}

	if (php_nghttp2_build_response_nva(response_head, &nva, &nvlen, &end_stream) != SUCCESS) {
		RETURN_THROWS();
	}

	rv = nghttp2_submit_headers(obj->session,
		end_stream ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE,
		(int32_t) stream_id,
		NULL,
		nva,
		nvlen,
		NULL);
	php_nghttp2_free_nv(nva, nvlen);

	if (rv != 0) {
		php_nghttp2_throw_error(ce_runtime_exception, "Failed to submit response headers", rv);
		RETURN_THROWS();
	}

	if (php_nghttp2_flush_send(obj, "Failed to flush response headers") != SUCCESS) {
		RETURN_THROWS();
	}
}

PHP_METHOD(Session, writeData)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	zend_long stream_id;
	zend_string *data;
	zend_bool end_stream = 0;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_LONG(stream_id)
		Z_PARAM_STR(data)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(end_stream)
	ZEND_PARSE_PARAMETERS_END();

	if (php_nghttp2_require_open_session(obj) != SUCCESS) {
		RETURN_THROWS();
	}

	if (php_nghttp2_submit_data_frame(obj, (int32_t) stream_id, data, end_stream) != SUCCESS) {
		RETURN_THROWS();
	}
}

PHP_METHOD(Session, endStream)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	zend_long stream_id;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(stream_id)
	ZEND_PARSE_PARAMETERS_END();

	if (php_nghttp2_require_open_session(obj) != SUCCESS) {
		RETURN_THROWS();
	}

	if (php_nghttp2_submit_data_frame(obj, (int32_t) stream_id, ZSTR_EMPTY_ALLOC(), 1) != SUCCESS) {
		RETURN_THROWS();
	}
}

PHP_METHOD(Session, resetStream)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	zend_long stream_id;
	zend_long error_code = 0;
	int rv;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_LONG(stream_id)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(error_code)
	ZEND_PARSE_PARAMETERS_END();

	if (php_nghttp2_require_open_session(obj) != SUCCESS) {
		RETURN_THROWS();
	}

	rv = nghttp2_submit_rst_stream(obj->session, NGHTTP2_FLAG_NONE, (int32_t) stream_id, (uint32_t) error_code);
	if (rv != 0) {
		php_nghttp2_throw_error(ce_runtime_exception, "Failed to submit RST_STREAM", rv);
		RETURN_THROWS();
	}

	if (php_nghttp2_flush_send(obj, "Failed to flush RST_STREAM") != SUCCESS) {
		RETURN_THROWS();
	}
}

PHP_METHOD(Session, shutdown)
{
	php_nghttp2_session_obj *obj = Z_NGHTTP2_SESSION_P(ZEND_THIS);
	zend_long error_code = 0;
	int rv;
	int32_t last_stream_id;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(error_code)
	ZEND_PARSE_PARAMETERS_END();

	if (!obj->session) {
		zend_throw_exception_ex(ce_runtime_exception, 0, "Session is not initialized");
		RETURN_THROWS();
	}

	if (obj->is_shutdown) {
		return;
	}

	last_stream_id = nghttp2_session_get_last_proc_stream_id(obj->session);
	rv = nghttp2_submit_goaway(obj->session, NGHTTP2_FLAG_NONE, last_stream_id, (uint32_t) error_code, NULL, 0);
	if (rv != 0) {
		php_nghttp2_throw_error(ce_runtime_exception, "Failed to submit GOAWAY", rv);
		RETURN_THROWS();
	}

	if (php_nghttp2_flush_send(obj, "Failed to flush GOAWAY") != SUCCESS) {
		RETURN_THROWS();
	}

	obj->is_shutdown = 1;
}

/* Value object methods */
PHP_METHOD(SessionOptions, __construct)
{
	zval *initial_window_size = NULL;
	zval *max_concurrent_streams = NULL;
	zval *header_table_size = NULL;
	zval *enable_push = NULL;
	zval *strict_validation = NULL;

	ZEND_PARSE_PARAMETERS_START(0, 5)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(initial_window_size)
		Z_PARAM_ZVAL(max_concurrent_streams)
		Z_PARAM_ZVAL(header_table_size)
		Z_PARAM_ZVAL(enable_push)
		Z_PARAM_ZVAL(strict_validation)
	ZEND_PARSE_PARAMETERS_END();

	if (initial_window_size && Z_TYPE_P(initial_window_size) != IS_NULL) {
		if (Z_TYPE_P(initial_window_size) != IS_LONG || Z_LVAL_P(initial_window_size) <= 0) {
			zend_argument_value_error(1, "must be a positive integer or null");
			RETURN_THROWS();
		}
		zend_update_property(ce_session_options, Z_OBJ_P(ZEND_THIS), ZEND_STRL("initialWindowSize"), initial_window_size);
	}

	if (max_concurrent_streams && Z_TYPE_P(max_concurrent_streams) != IS_NULL) {
		if (Z_TYPE_P(max_concurrent_streams) != IS_LONG || Z_LVAL_P(max_concurrent_streams) <= 0) {
			zend_argument_value_error(2, "must be a positive integer or null");
			RETURN_THROWS();
		}
		zend_update_property(ce_session_options, Z_OBJ_P(ZEND_THIS), ZEND_STRL("maxConcurrentStreams"), max_concurrent_streams);
	}

	if (header_table_size && Z_TYPE_P(header_table_size) != IS_NULL) {
		if (Z_TYPE_P(header_table_size) != IS_LONG || Z_LVAL_P(header_table_size) < 0) {
			zend_argument_value_error(3, "must be a non-negative integer or null");
			RETURN_THROWS();
		}
		zend_update_property(ce_session_options, Z_OBJ_P(ZEND_THIS), ZEND_STRL("headerTableSize"), header_table_size);
	}

	if (enable_push && Z_TYPE_P(enable_push) != IS_NULL) {
		zend_update_property_bool(ce_session_options, Z_OBJ_P(ZEND_THIS), ZEND_STRL("enablePush"), zend_is_true(enable_push));
	}

	if (strict_validation && Z_TYPE_P(strict_validation) != IS_NULL) {
		zend_update_property_bool(ce_session_options, Z_OBJ_P(ZEND_THIS), ZEND_STRL("strictValidation"), zend_is_true(strict_validation));
	}
}

PHP_METHOD(RequestHead, __construct)
{
	zend_string *method;
	zend_string *scheme;
	zend_string *authority;
	zend_string *path;
	zval *headers = NULL;
	zend_bool end_stream = 0;
	zval empty_headers;

	ZEND_PARSE_PARAMETERS_START(4, 6)
		Z_PARAM_STR(method)
		Z_PARAM_STR(scheme)
		Z_PARAM_STR(authority)
		Z_PARAM_STR(path)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY(headers)
		Z_PARAM_BOOL(end_stream)
	ZEND_PARSE_PARAMETERS_END();

	zend_update_property_str(ce_request_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("method"), method);
	zend_update_property_str(ce_request_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("scheme"), scheme);
	zend_update_property_str(ce_request_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("authority"), authority);
	zend_update_property_str(ce_request_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("path"), path);

	if (headers) {
		zend_update_property(ce_request_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("headers"), headers);
	} else {
		array_init(&empty_headers);
		zend_update_property(ce_request_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("headers"), &empty_headers);
		zval_ptr_dtor(&empty_headers);
	}

	zend_update_property_bool(ce_request_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("endStream"), end_stream);
}

PHP_METHOD(ResponseHead, __construct)
{
	zend_long status;
	zval *headers = NULL;
	zend_bool end_stream = 0;
	zval empty_headers;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_LONG(status)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY(headers)
		Z_PARAM_BOOL(end_stream)
	ZEND_PARSE_PARAMETERS_END();

	if (status < 100 || status > 999) {
		zend_argument_value_error(1, "must be between 100 and 999");
		RETURN_THROWS();
	}

	zend_update_property_long(ce_response_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("status"), status);
	if (headers) {
		zend_update_property(ce_response_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("headers"), headers);
	} else {
		array_init(&empty_headers);
		zend_update_property(ce_response_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("headers"), &empty_headers);
		zval_ptr_dtor(&empty_headers);
	}
	zend_update_property_bool(ce_response_head, Z_OBJ_P(ZEND_THIS), ZEND_STRL("endStream"), end_stream);
}

/* Object handlers */
static zend_object *php_nghttp2_session_create_object(zend_class_entry *class_type)
{
	php_nghttp2_session_obj *obj = zend_object_alloc(sizeof(*obj), class_type);

	zend_object_std_init(&obj->std, class_type);
	object_properties_init(&obj->std, class_type);
	obj->std.handlers = &php_nghttp2_session_handlers;

	obj->session = NULL;
	obj->callbacks = NULL;
	obj->role = 0;
	obj->is_shutdown = 0;
	obj->event_head = NULL;
	obj->event_tail = NULL;
	obj->output_head = NULL;
	obj->output_tail = NULL;
	obj->header_buckets = NULL;
	obj->data_sources = NULL;

	return &obj->std;
}

static void php_nghttp2_session_free_object(zend_object *object)
{
	php_nghttp2_session_obj *obj = php_nghttp2_session_from_obj(object);
	php_nghttp2_event_node *event_node;
	php_nghttp2_event_node *event_next;
	php_nghttp2_output_node *output_node;
	php_nghttp2_output_node *output_next;
	php_nghttp2_header_bucket *bucket;
	php_nghttp2_header_bucket *bucket_next;

	if (obj->session) {
		nghttp2_session_del(obj->session);
		obj->session = NULL;
	}

	if (obj->callbacks) {
		nghttp2_session_callbacks_del(obj->callbacks);
		obj->callbacks = NULL;
	}

	event_node = obj->event_head;
	while (event_node) {
		event_next = event_node->next;
		zval_ptr_dtor(&event_node->event);
		efree(event_node);
		event_node = event_next;
	}

	output_node = obj->output_head;
	while (output_node) {
		output_next = output_node->next;
		zend_string_release(output_node->chunk);
		efree(output_node);
		output_node = output_next;
	}

	bucket = obj->header_buckets;
	while (bucket) {
		bucket_next = bucket->next;
		zval_ptr_dtor(&bucket->headers);
		efree(bucket);
		bucket = bucket_next;
	}

	php_nghttp2_data_source_clear_all(obj);

	zend_object_std_dtor(&obj->std);
}

/* Arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo_session_construct, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, role, IS_LONG, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, options, Varion\\Nghttp2\\SessionOptions, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_receive, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, bytes, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_none, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_submit_request, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, head, Varion\\Nghttp2\\RequestHead, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_submit_response, 0, 0, 2)
	ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
	ZEND_ARG_OBJ_INFO(0, head, Varion\\Nghttp2\\ResponseHead, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_write_data, 0, 0, 2)
	ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, endStream, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_end_stream, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_reset_stream, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, errorCode, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_shutdown, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, errorCode, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_get_stream_state, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_session_options_construct, 0, 0, 0)
	ZEND_ARG_INFO_WITH_DEFAULT_VALUE(0, initialWindowSize, "null")
	ZEND_ARG_INFO_WITH_DEFAULT_VALUE(0, maxConcurrentStreams, "null")
	ZEND_ARG_INFO_WITH_DEFAULT_VALUE(0, headerTableSize, "null")
	ZEND_ARG_INFO_WITH_DEFAULT_VALUE(0, enablePush, "null")
	ZEND_ARG_INFO_WITH_DEFAULT_VALUE(0, strictValidation, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_request_head_construct, 0, 0, 4)
	ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, scheme, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, authority, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, endStream, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_response_head_construct, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, endStream, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

/* Method tables */
static const zend_function_entry session_methods[] = {
	PHP_ME(Session, __construct, arginfo_session_construct, ZEND_ACC_PUBLIC)
	PHP_ME(Session, receive, arginfo_session_receive, ZEND_ACC_PUBLIC)
	PHP_ME(Session, drainOutput, arginfo_session_none, ZEND_ACC_PUBLIC)
	PHP_ME(Session, nextEvent, arginfo_session_none, ZEND_ACC_PUBLIC)
	PHP_ME(Session, hasPendingEvents, arginfo_session_none, ZEND_ACC_PUBLIC)
	PHP_ME(Session, hasPendingOutput, arginfo_session_none, ZEND_ACC_PUBLIC)
	PHP_ME(Session, getOpenStreamCount, arginfo_session_none, ZEND_ACC_PUBLIC)
	PHP_ME(Session, getStreamState, arginfo_session_get_stream_state, ZEND_ACC_PUBLIC)
	PHP_ME(Session, submitRequest, arginfo_session_submit_request, ZEND_ACC_PUBLIC)
	PHP_ME(Session, submitResponse, arginfo_session_submit_response, ZEND_ACC_PUBLIC)
	PHP_ME(Session, writeData, arginfo_session_write_data, ZEND_ACC_PUBLIC)
	PHP_ME(Session, endStream, arginfo_session_end_stream, ZEND_ACC_PUBLIC)
	PHP_ME(Session, resetStream, arginfo_session_reset_stream, ZEND_ACC_PUBLIC)
	PHP_ME(Session, shutdown, arginfo_session_shutdown, ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry session_options_methods[] = {
	PHP_ME(SessionOptions, __construct, arginfo_session_options_construct, ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry request_head_methods[] = {
	PHP_ME(RequestHead, __construct, arginfo_request_head_construct, ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static const zend_function_entry response_head_methods[] = {
	PHP_ME(ResponseHead, __construct, arginfo_response_head_construct, ZEND_ACC_PUBLIC)
	PHP_FE_END
};

/* Module lifecycle */
PHP_MINIT_FUNCTION(nghttp2)
{
	zend_class_entry ce;

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "Exception", NULL);
	ce_exception = zend_register_internal_class_ex(&ce, zend_ce_exception);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "RuntimeException", NULL);
	ce_runtime_exception = zend_register_internal_class_ex(&ce, ce_exception);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "ProtocolException", NULL);
	ce_protocol_exception = zend_register_internal_class_ex(&ce, ce_runtime_exception);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "Session", session_methods);
	ce_session = zend_register_internal_class(&ce);
	ce_session->create_object = php_nghttp2_session_create_object;
	memcpy(&php_nghttp2_session_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_nghttp2_session_handlers.offset = XtOffsetOf(php_nghttp2_session_obj, std);
	php_nghttp2_session_handlers.free_obj = php_nghttp2_session_free_object;
	php_nghttp2_session_handlers.clone_obj = NULL;

	zend_declare_class_constant_long(ce_session, ZEND_STRL("ROLE_CLIENT"), PHP_NGHTTP2_ROLE_CLIENT);
	zend_declare_class_constant_long(ce_session, ZEND_STRL("ROLE_SERVER"), PHP_NGHTTP2_ROLE_SERVER);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "SessionOptions", session_options_methods);
	ce_session_options = zend_register_internal_class(&ce);
	zend_declare_property_null(ce_session_options, ZEND_STRL("initialWindowSize"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_session_options, ZEND_STRL("maxConcurrentStreams"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_session_options, ZEND_STRL("headerTableSize"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_session_options, ZEND_STRL("enablePush"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_session_options, ZEND_STRL("strictValidation"), ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "RequestHead", request_head_methods);
	ce_request_head = zend_register_internal_class(&ce);
	zend_declare_property_null(ce_request_head, ZEND_STRL("method"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_request_head, ZEND_STRL("scheme"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_request_head, ZEND_STRL("authority"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_request_head, ZEND_STRL("path"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_request_head, ZEND_STRL("headers"), ZEND_ACC_PUBLIC);
	zend_declare_property_bool(ce_request_head, ZEND_STRL("endStream"), 0, ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "ResponseHead", response_head_methods);
	ce_response_head = zend_register_internal_class(&ce);
	zend_declare_property_null(ce_response_head, ZEND_STRL("status"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_response_head, ZEND_STRL("headers"), ZEND_ACC_PUBLIC);
	zend_declare_property_bool(ce_response_head, ZEND_STRL("endStream"), 0, ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "Event", NULL);
	ce_event = zend_register_internal_class(&ce);
	ce_event->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS;

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "HeadersReceived", NULL);
	ce_event_headers_received = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_headers_received, ZEND_STRL("streamId"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_event_headers_received, ZEND_STRL("headers"), ZEND_ACC_PUBLIC);
	zend_declare_property_bool(ce_event_headers_received, ZEND_STRL("endStream"), 0, ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "DataReceived", NULL);
	ce_event_data_received = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_data_received, ZEND_STRL("streamId"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_event_data_received, ZEND_STRL("data"), ZEND_ACC_PUBLIC);
	zend_declare_property_bool(ce_event_data_received, ZEND_STRL("endStream"), 0, ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "StreamClosed", NULL);
	ce_event_stream_closed = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_stream_closed, ZEND_STRL("streamId"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_event_stream_closed, ZEND_STRL("errorCode"), ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "StreamReset", NULL);
	ce_event_stream_reset = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_stream_reset, ZEND_STRL("streamId"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_event_stream_reset, ZEND_STRL("errorCode"), ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "GoawayReceived", NULL);
	ce_event_goaway_received = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_goaway_received, ZEND_STRL("lastStreamId"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_event_goaway_received, ZEND_STRL("errorCode"), ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "SettingsReceived", NULL);
	ce_event_settings_received = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_settings_received, ZEND_STRL("settings"), ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "SettingsAcked", NULL);
	ce_event_settings_acked = zend_register_internal_class_ex(&ce, ce_event);

	return SUCCESS;
}

PHP_MINFO_FUNCTION(nghttp2)
{
	const nghttp2_info *info = nghttp2_version(0);

	php_info_print_table_start();
	php_info_print_table_header(2, "nghttp2 support", "enabled");
	php_info_print_table_row(2, "extension version", PHP_NGHTTP2_VERSION);
	php_info_print_table_row(2, "linked nghttp2", info ? info->version_str : "unknown");
	php_info_print_table_end();
}

zend_module_entry nghttp2_module_entry = {
	STANDARD_MODULE_HEADER,
	"nghttp2",
	NULL,
	PHP_MINIT(nghttp2),
	NULL,
	NULL,
	NULL,
	PHP_MINFO(nghttp2),
	PHP_NGHTTP2_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_NGHTTP2
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(nghttp2)
#endif
