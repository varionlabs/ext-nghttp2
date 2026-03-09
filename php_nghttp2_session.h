#ifndef PHP_NGHTTP2_SESSION_H
#define PHP_NGHTTP2_SESSION_H

#include "php.h"
#include <nghttp2/nghttp2.h>
#include <stdint.h>

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

extern zend_class_entry *ce_session;
extern zend_class_entry *ce_session_options;
extern zend_class_entry *ce_request_head;
extern zend_class_entry *ce_response_head;

extern zend_class_entry *ce_exception;
extern zend_class_entry *ce_runtime_exception;
extern zend_class_entry *ce_protocol_exception;

extern zend_object_handlers php_nghttp2_session_handlers;

static inline php_nghttp2_session_obj *php_nghttp2_session_from_obj(zend_object *obj)
{
	return (php_nghttp2_session_obj *) ((char *) obj - XtOffsetOf(php_nghttp2_session_obj, std));
}

#define Z_NGHTTP2_SESSION_P(zv) php_nghttp2_session_from_obj(Z_OBJ_P((zv)))

zend_result php_nghttp2_register_session_classes(void);

void php_nghttp2_throw_error(zend_class_entry *error_ce, const char *context, int code);
void php_nghttp2_enqueue_output(php_nghttp2_session_obj *obj, const uint8_t *data, size_t len);
void php_nghttp2_enqueue_event(php_nghttp2_session_obj *obj, zval *event);
php_nghttp2_header_bucket *php_nghttp2_get_or_create_header_bucket(php_nghttp2_session_obj *obj, int32_t stream_id);
php_nghttp2_header_bucket *php_nghttp2_take_header_bucket(php_nghttp2_session_obj *obj, int32_t stream_id);
void php_nghttp2_free_header_bucket(php_nghttp2_header_bucket *bucket);
void php_nghttp2_data_source_remove(php_nghttp2_session_obj *obj, php_nghttp2_data_source_ctx *target);
void php_nghttp2_data_source_cleanup_stream(php_nghttp2_session_obj *obj, int32_t stream_id);
void php_nghttp2_data_source_clear_all(php_nghttp2_session_obj *obj);
ssize_t php_nghttp2_send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data);

#endif
