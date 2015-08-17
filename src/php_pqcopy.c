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
#include <Zend/zend_smart_str.h>

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

static void php_pqcopy_object_free(zend_object *o)
{
	php_pqcopy_object_t *obj = PHP_PQ_OBJ(NULL, o);
#if DBG_GC
	fprintf(stderr, "FREE copy(#%d) %p (conn(#%d): %p)\n", obj->zo.handle, obj, obj->intern->conn->zo.handle, obj->intern->conn);
#endif
	if (obj->intern) {
		efree(obj->intern->expression);
		efree(obj->intern->options);
		php_pq_object_delref(obj->intern->conn);
		efree(obj->intern);
		obj->intern = NULL;
	}
	php_pq_object_dtor(o);
}

php_pqcopy_object_t *php_pqcopy_create_object_ex(zend_class_entry *ce, php_pqcopy_t *intern)
{
	return php_pq_object_create(ce, intern, sizeof(php_pqcopy_object_t),
			&php_pqcopy_object_handlers, &php_pqcopy_object_prophandlers);
}

static zend_object *php_pqcopy_create_object(zend_class_entry *class_type)
{
	return &php_pqcopy_create_object_ex(class_type, NULL)->zo;
}

static void php_pqcopy_object_read_connection(zval *object, void *o, zval *return_value)
{
	php_pqcopy_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, return_value);
}

static void php_pqcopy_object_gc_connection(zval *object, void *o, zval *return_value)
{
	php_pqcopy_object_t *obj = o;
	zval zconn;

	php_pq_object_to_zval_no_addref(obj->intern->conn, &zconn);
	add_next_index_zval(return_value, &zconn);
}

static void php_pqcopy_object_read_direction(zval *object, void *o, zval *return_value)
{
	php_pqcopy_object_t *obj = o;

	RETVAL_LONG(obj->intern->direction);
}

static void php_pqcopy_object_read_expression(zval *object, void *o, zval *return_value)
{
	php_pqcopy_object_t *obj = o;

	RETURN_STRING(obj->intern->expression);
}

static void php_pqcopy_object_read_options(zval *object, void *o, zval *return_value)
{
	php_pqcopy_object_t *obj = o;

	RETURN_STRING(obj->intern->options);
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
	size_t expr_len, opt_len = 0;
	zend_long direction;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "Osl|s", &zconn, php_pqconn_class_entry, &expr_str, &expr_len, &direction, &opt_str, &opt_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *conn_obj = PHP_PQ_OBJ(zconn, NULL);

		if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			php_pqcopy_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);
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
				throw_exce(EX_RUNTIME, "Invalid COPY direction, expected one of FROM_STDIN (%d) TO_STDOUT (%d), got %ld", PHP_PQCOPY_FROM_STDIN, PHP_PQCOPY_TO_STDOUT, direction);
				smart_str_free(&cmd);
				return;
			}
			smart_str_appendl(&cmd, opt_str, opt_len);
			smart_str_0(&cmd);

			res = PQexec(conn_obj->intern->conn, smart_str_v(&cmd));

			if (!res) {
				throw_exce(EX_RUNTIME, "Failed to start %s (%s)", smart_str_v(&cmd), PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res)) {
					obj->intern = ecalloc(1, sizeof(*obj->intern));
					obj->intern->direction = direction;
					obj->intern->expression = estrdup(expr_str);
					obj->intern->options = estrdup(opt_str);
					obj->intern->conn = conn_obj;
					php_pq_object_addref(conn_obj);
				}

				PHP_PQclear(res);
			}

			smart_str_free(&cmd);
			php_pqconn_notify_listeners(obj->intern->conn);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcopy_put, 0, 0, 1)
	ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcopy, put) {
	zend_error_handling zeh;
	char *data_str;
	size_t data_len;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "s", &data_str, &data_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqcopy_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\COPY not initialized");
		} else if (obj->intern->direction != PHP_PQCOPY_FROM_STDIN) {
			throw_exce(EX_BAD_METHODCALL, "pq\\COPY was not initialized with FROM_STDIN");
		} else {
			if (1 != PQputCopyData(obj->intern->conn->intern->conn, data_str, data_len)) {
				throw_exce(EX_RUNTIME, "Failed to put COPY data (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}
			php_pqconn_notify_listeners(obj->intern->conn);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcopy_end, 0, 0, 0)
	ZEND_ARG_INFO(0, error)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcopy, end) {
	zend_error_handling zeh;
	char *error_str = NULL;
	size_t error_len = 0;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|s!", &error_str, &error_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqcopy_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\COPY not intitialized");
		} else if (obj->intern->direction != PHP_PQCOPY_FROM_STDIN) {
			throw_exce(EX_BAD_METHODCALL, "pq\\COPY was not intitialized with FROM_STDIN");
		} else {
			if (1 != PQputCopyEnd(obj->intern->conn->intern->conn, error_str)) {
				throw_exce(EX_RUNTIME, "Failed to end COPY (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				PGresult *res = PQgetResult(obj->intern->conn->intern->conn);

				if (!res) {
					throw_exce(EX_RUNTIME, "Failed to fetch COPY result (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else {
					php_pqres_success(res);
					PHP_PQclear(res);
				}
			}

			php_pqconn_notify_listeners(obj->intern->conn);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcopy_get, 0, 0, 1)
	ZEND_ARG_INFO(1, data)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcopy, get) {
	zend_error_handling zeh;
	zval *zdata;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &zdata);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqcopy_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\COPY not initialized");
		} else if (obj->intern->direction != PHP_PQCOPY_TO_STDOUT) {
			throw_exce(EX_RUNTIME, "pq\\COPY was not intialized with TO_STDOUT");
		} else {
			PGresult *res;
			char *buffer = NULL;
			int bytes = PQgetCopyData(obj->intern->conn->intern->conn, &buffer, 0);

			switch (bytes) {
			case -2:
				throw_exce(EX_RUNTIME, "Failed to fetch COPY data (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				break;

			case -1:
				res = PQgetResult(obj->intern->conn->intern->conn);

				if (!res) {
					throw_exce(EX_RUNTIME, "Failed to fetch COPY result (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else {
					php_pqres_success(res);
					PHP_PQclear(res);
					RETVAL_FALSE;
				}
				break;

			default:
				ZVAL_DEREF(zdata);
				zval_dtor(zdata);
				if (buffer) {
					ZVAL_STRINGL(zdata, buffer, bytes);
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
	php_pqcopy_class_entry = zend_register_internal_class_ex(&ce, NULL);
	php_pqcopy_class_entry->create_object = php_pqcopy_create_object;

	memcpy(&php_pqcopy_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqcopy_object_handlers.offset = XtOffsetOf(php_pqcopy_object_t, zo);
	php_pqcopy_object_handlers.free_obj = php_pqcopy_object_free;
	php_pqcopy_object_handlers.read_property = php_pq_object_read_prop;
	php_pqcopy_object_handlers.write_property = php_pq_object_write_prop;
	php_pqcopy_object_handlers.clone_obj = NULL;
	php_pqcopy_object_handlers.get_property_ptr_ptr = NULL;
	php_pqcopy_object_handlers.get_gc = php_pq_object_get_gc;
	php_pqcopy_object_handlers.get_properties = php_pq_object_properties;
	php_pqcopy_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqcopy_object_prophandlers, 4, NULL, php_pq_object_prophandler_dtor, 1);

	zend_declare_property_null(php_pqcopy_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC);
	ph.read = php_pqcopy_object_read_connection;
	ph.gc = php_pqcopy_object_gc_connection;
	zend_hash_str_add_mem(&php_pqcopy_object_prophandlers, "connection", sizeof("connection")-1, (void *) &ph, sizeof(ph));
	ph.gc = NULL;

	zend_declare_property_null(php_pqcopy_class_entry, ZEND_STRL("expression"), ZEND_ACC_PUBLIC);
	ph.read = php_pqcopy_object_read_expression;
	zend_hash_str_add_mem(&php_pqcopy_object_prophandlers, "expression", sizeof("expression")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqcopy_class_entry, ZEND_STRL("direction"), ZEND_ACC_PUBLIC);
	ph.read = php_pqcopy_object_read_direction;
	zend_hash_str_add_mem(&php_pqcopy_object_prophandlers, "direction", sizeof("direction")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqcopy_class_entry, ZEND_STRL("options"), ZEND_ACC_PUBLIC);
	ph.read = php_pqcopy_object_read_options;
	zend_hash_str_add_mem(&php_pqcopy_object_prophandlers, "options", sizeof("options")-1, (void *) &ph, sizeof(ph));

	zend_declare_class_constant_long(php_pqcopy_class_entry, ZEND_STRL("FROM_STDIN"), PHP_PQCOPY_FROM_STDIN);
	zend_declare_class_constant_long(php_pqcopy_class_entry, ZEND_STRL("TO_STDOUT"), PHP_PQCOPY_TO_STDOUT);

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
