#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "Zend/zend_exceptions.h"

#include "php_nghttp2.h"
#include "php_nghttp2_session.h"
#include "php_nghttp2_event.h"

#include <nghttp2/nghttp2.h>

zend_class_entry *ce_exception;
zend_class_entry *ce_runtime_exception;
zend_class_entry *ce_protocol_exception;

PHP_MINIT_FUNCTION(nghttp2)
{
	zend_class_entry ce;

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "Exception", NULL);
	ce_exception = zend_register_internal_class_ex(&ce, zend_ce_exception);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "RuntimeException", NULL);
	ce_runtime_exception = zend_register_internal_class_ex(&ce, ce_exception);

	INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp2", "ProtocolException", NULL);
	ce_protocol_exception = zend_register_internal_class_ex(&ce, ce_runtime_exception);

	if (php_nghttp2_register_session_classes() != SUCCESS) {
		return FAILURE;
	}

	if (php_nghttp2_register_event_classes() != SUCCESS) {
		return FAILURE;
	}

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
