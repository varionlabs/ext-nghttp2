#ifndef PHP_NGHTTP2_EVENT_H
#define PHP_NGHTTP2_EVENT_H

#include "php.h"
#include <nghttp2/nghttp2.h>

extern zend_class_entry *ce_event;
extern zend_class_entry *ce_event_stream;
extern zend_class_entry *ce_event_connection;
extern zend_class_entry *ce_event_headers_received;
extern zend_class_entry *ce_event_data_received;
extern zend_class_entry *ce_event_stream_closed;
extern zend_class_entry *ce_event_stream_reset;
extern zend_class_entry *ce_event_goaway_received;
extern zend_class_entry *ce_event_settings_received;
extern zend_class_entry *ce_event_settings_acked;

zend_result php_nghttp2_register_event_classes(void);

int php_nghttp2_on_header_callback(nghttp2_session *session, const nghttp2_frame *frame,
	const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen,
	uint8_t flags, void *user_data);
int php_nghttp2_on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id,
	const uint8_t *data, size_t len, void *user_data);
int php_nghttp2_on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code,
	void *user_data);
int php_nghttp2_on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);

#endif
