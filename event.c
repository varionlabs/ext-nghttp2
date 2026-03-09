#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"

#include "php_nghttp2.h"
#include "php_nghttp2_session.h"
#include "php_nghttp2_event.h"

zend_class_entry *ce_event;
zend_class_entry *ce_event_headers_received;
zend_class_entry *ce_event_data_received;
zend_class_entry *ce_event_stream_closed;
zend_class_entry *ce_event_stream_reset;
zend_class_entry *ce_event_goaway_received;
zend_class_entry *ce_event_settings_received;
zend_class_entry *ce_event_settings_acked;
static void php_nghttp2_event_add_common_stream_id(zval *event, zend_class_entry *ce, int32_t stream_id)
{
	zend_update_property_long(ce, Z_OBJ_P(event), ZEND_STRL("streamId"), (zend_long) stream_id);
}

int php_nghttp2_on_header_callback(nghttp2_session *session, const nghttp2_frame *frame,
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

int php_nghttp2_on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id,
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

int php_nghttp2_on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code,
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

int php_nghttp2_on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
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

zend_result php_nghttp2_register_event_classes(void)
{
	zend_class_entry ce;

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "Event", NULL);
	ce_event = zend_register_internal_class(&ce);
	ce_event->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS;

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2\\Events", "HeadersReceived", NULL);
	ce_event_headers_received = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_headers_received, ZEND_STRL("streamId"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_event_headers_received, ZEND_STRL("headers"), ZEND_ACC_PUBLIC);
	zend_declare_property_bool(ce_event_headers_received, ZEND_STRL("endStream"), 0, ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2\\Events", "DataReceived", NULL);
	ce_event_data_received = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_data_received, ZEND_STRL("streamId"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_event_data_received, ZEND_STRL("data"), ZEND_ACC_PUBLIC);
	zend_declare_property_bool(ce_event_data_received, ZEND_STRL("endStream"), 0, ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2\\Events", "StreamClosed", NULL);
	ce_event_stream_closed = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_stream_closed, ZEND_STRL("streamId"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_event_stream_closed, ZEND_STRL("errorCode"), ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2\\Events", "StreamReset", NULL);
	ce_event_stream_reset = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_stream_reset, ZEND_STRL("streamId"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_event_stream_reset, ZEND_STRL("errorCode"), ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2\\Events", "GoawayReceived", NULL);
	ce_event_goaway_received = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_goaway_received, ZEND_STRL("lastStreamId"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(ce_event_goaway_received, ZEND_STRL("errorCode"), ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2\\Events", "SettingsReceived", NULL);
	ce_event_settings_received = zend_register_internal_class_ex(&ce, ce_event);
	zend_declare_property_null(ce_event_settings_received, ZEND_STRL("settings"), ZEND_ACC_PUBLIC);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2\\Events", "SettingsAcked", NULL);
	ce_event_settings_acked = zend_register_internal_class_ex(&ce, ce_event);

	return SUCCESS;
}
