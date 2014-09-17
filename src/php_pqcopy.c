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
#include <ext/standard/php_smart_str.h>

#include <libpq-events.h>

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pq_object.h"
#include "php_pqexc.h"
#include "php_pqres.h"
#include "php_pqconn.h"
#include "php_pqcopy.h"

zend_class_entry *php_pqcopy_class_entry;
static zend_object_handlers php_pqcopy_object_handlers;
static HashTable php_pqcopy_object_prophandlers;

static void php_pqcopy_object_free(void *o TSRMLS_DC)
{
	php_pqcopy_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE copy(#%d) %p (conn(#%d): %p)\n", obj->zv.handle, obj, obj->intern->conn->zv.handle, obj->intern->conn);
#endif
	if (obj->intern) {
		efree(obj->intern->expression);
		efree(obj->intern->options);
		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

zend_object_value php_pqcopy_create_object_ex(zend_class_entry *ce, php_pqcopy_t *intern, php_pqcopy_object_t **ptr TSRMLS_DC)
{
	php_pqcopy_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqcopy_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqcopy_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqcopy_object_handlers;

	return o->zv;
}

static zend_object_value php_pqcopy_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqcopy_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static void php_pqcopy_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqcopy_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static void php_pqcopy_object_read_direction(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqcopy_object_t *obj = o;

	RETVAL_LONG(obj->intern->direction);
}

static void php_pqcopy_object_read_expression(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqcopy_object_t *obj = o;

	RETURN_STRING(obj->intern->expression, 1);
}

static void php_pqcopy_object_read_options(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqcopy_object_t *obj = o;

	RETURN_STRING(obj->intern->options, 1);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcopy_construct, 0, 0, 3)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
	ZEND_ARG_INFO(0, expression)
	ZEND_ARG_INFO(0, direction)
	ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcopy, __construct) {
	zend_error_handling zeh;
	zval *zconn;
	char *expr_str, *opt_str = "";
	int expr_len, opt_len = 0;
	long direction;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Osl|s", &zconn, php_pqconn_class_entry, &expr_str, &expr_len, &direction, &opt_str, &opt_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			php_pqcopy_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
			smart_str cmd = {0};
			PGresult *res;

			smart_str_appends(&cmd, "COPY ");
			smart_str_appendl(&cmd, expr_str, expr_len);

			switch (direction) {
			case PHP_PQCOPY_FROM_STDIN:
				smart_str_appends(&cmd, " FROM STDIN ");
				break;
			case PHP_PQCOPY_TO_STDOUT:
				smart_str_appends(&cmd, " TO STDOUT ");
				break;
			default:
				throw_exce(EX_RUNTIME TSRMLS_CC, "Invalid COPY direction, expected one of FROM_STDIN (%d) TO_STDOUT (%d), got %ld", PHP_PQCOPY_FROM_STDIN, PHP_PQCOPY_TO_STDOUT, direction);
				smart_str_free(&cmd);
				return;
			}
			smart_str_appendl(&cmd, opt_str, opt_len);
			smart_str_0(&cmd);

			res = PQexec(conn_obj->intern->conn, cmd.c);

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to start %s (%s)", cmd.c, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					obj->intern = ecalloc(1, sizeof(*obj->intern));
					obj->intern->direction = direction;
					obj->intern->expression = estrdup(expr_str);
					obj->intern->options = estrdup(opt_str);
					obj->intern->conn = conn_obj;
					php_pq_object_addref(conn_obj TSRMLS_CC);
				}

				PHP_PQclear(res);
			}

			smart_str_free(&cmd);
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcopy_put, 0, 0, 1)
	ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcopy, put) {
	zend_error_handling zeh;
	char *data_str;
	int data_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &data_str, &data_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqcopy_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\COPY not initialized");
		} else if (obj->intern->direction != PHP_PQCOPY_FROM_STDIN) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\COPY was not initialized with FROM_STDIN");
		} else {
			if (1 != PQputCopyData(obj->intern->conn->intern->conn, data_str, data_len)) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to put COPY data (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcopy_end, 0, 0, 0)
	ZEND_ARG_INFO(0, error)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcopy, end) {
	zend_error_handling zeh;
	char *error_str = NULL;
	int error_len = 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!", &error_str, &error_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqcopy_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\COPY not intitialized");
		} else if (obj->intern->direction != PHP_PQCOPY_FROM_STDIN) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\COPY was not intitialized with FROM_STDIN");
		} else {
			if (1 != PQputCopyEnd(obj->intern->conn->intern->conn, error_str)) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to end COPY (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				PGresult *res = PQgetResult(obj->intern->conn->intern->conn);

				if (!res) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to fetch COPY result (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else {
					php_pqres_success(res TSRMLS_CC);
					PHP_PQclear(res);
				}
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcopy_get, 0, 0, 1)
	ZEND_ARG_INFO(1, data)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcopy, get) {
	zend_error_handling zeh;
	zval *zdata;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &zdata);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqcopy_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\COPY not initialized");
		} else if (obj->intern->direction != PHP_PQCOPY_TO_STDOUT) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\COPY was not intialized with TO_STDOUT");
		} else {
			PGresult *res;
			char *buffer = NULL;
			int bytes = PQgetCopyData(obj->intern->conn->intern->conn, &buffer, 0);

			switch (bytes) {
			case -2:
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to fetch COPY data (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				break;

			case -1:
				res = PQgetResult(obj->intern->conn->intern->conn);

				if (!res) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to fetch COPY result (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else {
					php_pqres_success(res TSRMLS_CC);
					PHP_PQclear(res);
					RETVAL_FALSE;
				}
				break;

			default:
				zval_dtor(zdata);
				if (buffer) {
					ZVAL_STRINGL(zdata, buffer, bytes, 1);
				} else {
					ZVAL_EMPTY_STRING(zdata);
				}
				RETVAL_TRUE;
				break;
			}

			if (buffer) {
				PQfreemem(buffer);
			}
		}
	}
}

static zend_function_entry php_pqcopy_methods[] = {
	PHP_ME(pqcopy, __construct, ai_pqcopy_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqcopy, put, ai_pqcopy_put, ZEND_ACC_PUBLIC)
	PHP_ME(pqcopy, end, ai_pqcopy_end, ZEND_ACC_PUBLIC)
	PHP_ME(pqcopy, get, ai_pqcopy_get, ZEND_ACC_PUBLIC)
	{0}
};

PHP_MSHUTDOWN_FUNCTION(pqcopy)
{
	zend_hash_destroy(&php_pqcopy_object_prophandlers);
	return SUCCESS;
}

PHP_MINIT_FUNCTION(pqcopy)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "COPY", php_pqcopy_methods);
	php_pqcopy_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqcopy_class_entry->create_object = php_pqcopy_create_object;

	memcpy(&php_pqcopy_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqcopy_object_handlers.read_property = php_pq_object_read_prop;
	php_pqcopy_object_handlers.write_property = php_pq_object_write_prop;
	php_pqcopy_object_handlers.clone_obj = NULL;
	php_pqcopy_object_handlers.get_property_ptr_ptr = NULL;
	php_pqcopy_object_handlers.get_gc = NULL;
	php_pqcopy_object_handlers.get_properties = php_pq_object_properties;
	php_pqcopy_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqcopy_object_prophandlers, 4, NULL, NULL, 1);

	zend_declare_property_null(php_pqcopy_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcopy_object_read_connection;
	zend_hash_add(&php_pqcopy_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqcopy_class_entry, ZEND_STRL("expression"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcopy_object_read_expression;
	zend_hash_add(&php_pqcopy_object_prophandlers, "expression", sizeof("expression"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqcopy_class_entry, ZEND_STRL("direction"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcopy_object_read_direction;
	zend_hash_add(&php_pqcopy_object_prophandlers, "direction", sizeof("direction"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqcopy_class_entry, ZEND_STRL("options"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcopy_object_read_options;
	zend_hash_add(&php_pqcopy_object_prophandlers, "options", sizeof("options"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_class_constant_long(php_pqcopy_class_entry, ZEND_STRL("FROM_STDIN"), PHP_PQCOPY_FROM_STDIN TSRMLS_CC);
	zend_declare_class_constant_long(php_pqcopy_class_entry, ZEND_STRL("TO_STDOUT"), PHP_PQCOPY_TO_STDOUT TSRMLS_CC);

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
