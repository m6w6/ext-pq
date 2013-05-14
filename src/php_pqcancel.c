/*
    +--------------------------------------------------------------------+
    | PECL :: pq                                                         |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted provided that the conditions mentioned |
    | in the accompanying LICENSE file are met.                          |
    +--------------------------------------------------------------------+
    | Copyright (c) 2013, Michael Wallner <mike@php.net>                 |
    +--------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include <php.h>

#include <libpq-fe.h>

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pq_object.h"
#include "php_pqexc.h"
#include "php_pqcancel.h"

zend_class_entry *php_pqcancel_class_entry;
static zend_object_handlers php_pqcancel_object_handlers;
static HashTable php_pqcancel_object_prophandlers;

static void php_pqcancel_object_free(void *o TSRMLS_DC)
{
	php_pqcancel_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE cancel(#%d) %p (conn(#%d): %p)\n", obj->zv.handle, obj, obj->intern->conn->zv.handle, obj->intern->conn);
#endif
	if (obj->intern) {
		PQfreeCancel(obj->intern->cancel);
		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

zend_object_value php_pqcancel_create_object_ex(zend_class_entry *ce, php_pqcancel_t *intern, php_pqcancel_object_t **ptr TSRMLS_DC)
{
	php_pqcancel_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqcancel_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqcancel_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqcancel_object_handlers;

	return o->zv;
}

static zend_object_value php_pqcancel_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqcancel_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static void php_pqcancel_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqcancel_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcancel_construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcancel, __construct) {
	zend_error_handling zeh;
	zval *zconn;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O", &zconn, php_pqconn_class_entry);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PGcancel *cancel = PQgetCancel(conn_obj->intern->conn);

			if (!cancel) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to acquire cancel (%s)", PHP_PQerrorMessage(conn_obj->intern->conn));
			} else {
				php_pqcancel_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

				obj->intern = ecalloc(1, sizeof(*obj->intern));
				obj->intern->cancel = cancel;
				php_pq_object_addref(conn_obj TSRMLS_CC);
				obj->intern->conn = conn_obj;
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcancel_cancel, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcancel, cancel) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqcancel_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Cancel not initialized");
		} else {
			char err[256] = {0};

			if (!PQcancel(obj->intern->cancel, err, sizeof(err))) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to request cancellation (%s)", err);
			}
		}
	}
}

static zend_function_entry php_pqcancel_methods[] = {
	PHP_ME(pqcancel, __construct, ai_pqcancel_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqcancel, cancel, ai_pqcancel_cancel, ZEND_ACC_PUBLIC)
	{0}
};

PHP_MSHUTDOWN_FUNCTION(pqcancel)
{
	zend_hash_destroy(&php_pqcancel_object_prophandlers);
	return SUCCESS;
}

PHP_MINIT_FUNCTION(pqcancel)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Cancel", php_pqcancel_methods);
	php_pqcancel_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqcancel_class_entry->create_object = php_pqcancel_create_object;

	memcpy(&php_pqcancel_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqcancel_object_handlers.read_property = php_pq_object_read_prop;
	php_pqcancel_object_handlers.write_property = php_pq_object_write_prop;
	php_pqcancel_object_handlers.clone_obj = NULL;
	php_pqcancel_object_handlers.get_property_ptr_ptr = NULL;
	php_pqcancel_object_handlers.get_gc = NULL;
	php_pqcancel_object_handlers.get_properties = php_pq_object_properties;
	php_pqcancel_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqcancel_object_prophandlers, 1, NULL, NULL, 1);

	zend_declare_property_null(php_pqcancel_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcancel_object_read_connection;
	zend_hash_add(&php_pqcancel_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	return SUCCESS;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
