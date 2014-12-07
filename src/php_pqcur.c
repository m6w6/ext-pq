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

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pq_object.h"
#include "php_pqexc.h"
#include "php_pqconn.h"
#include "php_pqres.h"
#include "php_pqcur.h"

zend_class_entry *php_pqcur_class_entry;
static zend_object_handlers php_pqcur_object_handlers;
static HashTable php_pqcur_object_prophandlers;

static void cur_close(php_pqcur_object_t *obj, zend_bool async, zend_bool silent TSRMLS_DC)
{
	if (obj->intern->open && obj->intern->conn->intern) {
		PGresult *res;
		smart_str cmd = {0};

		smart_str_appends(&cmd, "CLOSE ");
		smart_str_appends(&cmd, obj->intern->name);
		smart_str_0(&cmd);

		if (async) {
			if (PQsendQuery(obj->intern->conn->intern->conn, cmd.c)) {
				obj->intern->conn->intern->poller = PQconsumeInput;
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			} else if (!silent) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to close cursor (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}
		} else {
			if ((res = PQexec(obj->intern->conn->intern->conn, cmd.c))) {
				PHP_PQclear(res);
			} else if (!silent) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to close cursor (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}
		}

		smart_str_free(&cmd);
		obj->intern->open = 0;
	}
}

static void cur_open(INTERNAL_FUNCTION_PARAMETERS, zend_bool async)
{
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (rv == FAILURE) {
		return;
	}

	php_pqcur_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

	if (!obj->intern) {
		throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Cursor not initialized");
		return;
	} else if (obj->intern->open) {
		return;
	}

	if (async) {
		rv = php_pqconn_declare_async(NULL, obj->intern->conn, obj->intern->decl TSRMLS_CC);
	} else {
		rv = php_pqconn_declare(NULL, obj->intern->conn, obj->intern->decl TSRMLS_CC);
	}

	if (rv == SUCCESS) {
		obj->intern->open = 1;
	}
}

static void cur_fetch_or_move(INTERNAL_FUNCTION_PARAMETERS, const char *action, zend_bool async)
{
	char *spec_str = "1";
	int spec_len = 1;
	STATUS rv;
	php_pq_callback_t resolver = {{0}};
	zend_error_handling zeh;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, async ? "|sf" : "|s", &spec_str, &spec_len, &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqcur_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Cursor not initialized");
		} else {
			smart_str cmd = {0};

			smart_str_appends(&cmd, *action == 'f' ? "FETCH " : "MOVE ");
			smart_str_appendl(&cmd, spec_str, spec_len);
			smart_str_appends(&cmd, " FROM ");
			smart_str_appends(&cmd, obj->intern->name);
			smart_str_0(&cmd);

			if (async) {
				int rc = PQsendQuery(obj->intern->conn->intern->conn, cmd.c);

				if (!rc) {
					throw_exce(EX_IO TSRMLS_CC, "Failed to %s cursor (%s)", *action == 'f' ? "fetch from" : "move in", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
#if HAVE_PQSETSINGLEROWMODE
				} else if (obj->intern->conn->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn->intern->conn)) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
#endif
				} else {
					php_pq_callback_recurse(&obj->intern->conn->intern->onevent, &resolver TSRMLS_CC);
					obj->intern->conn->intern->poller = PQconsumeInput;
				}
			} else {
				PGresult *res = PQexec(obj->intern->conn->intern->conn, cmd.c);

				if (!res) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to %s cursor (%s)", *action == 'f' ? "fetch from" : "move in", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), &return_value TSRMLS_CC);

				}
			}
			smart_str_free(&cmd);
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

static void php_pqcur_object_free(void *o TSRMLS_DC)
{
	php_pqcur_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE cur(#%d) %p (conn: %p)\n", obj->zv.handle, obj, obj->intern->conn);
#endif
	if (obj->intern) {
		cur_close(obj, 0, 1 TSRMLS_CC);
		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern->decl);
		efree(obj->intern->name);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

zend_object_value php_pqcur_create_object_ex(zend_class_entry *ce, php_pqcur_t *intern, php_pqcur_object_t **ptr TSRMLS_DC)
{
	php_pqcur_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqcur_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqcur_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqcur_object_handlers;

	return o->zv;
}

static zend_object_value php_pqcur_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqcur_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static void php_pqcur_object_read_name(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqcur_object_t *obj = o;

	RETVAL_STRING(obj->intern->name, 1);
}

static void php_pqcur_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqcur_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static void php_pqcur_object_read_flags(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqcur_object_t *obj = o;

	RETVAL_LONG(obj->intern->flags);
}

char *php_pqcur_declare_str(const char *name_str, size_t name_len, unsigned flags, const char *query_str, size_t query_len)
{
	size_t decl_len = name_len + query_len + sizeof("DECLARE  BINARY INSENSITIVE NO SCROLL CURSOR WITH HOLD FOR ");
	char *decl_str;

	decl_str = emalloc(decl_len);
	decl_len = slprintf(decl_str, decl_len, "DECLARE %s %s %s %s CURSOR %s FOR %s",
			name_str,
			(flags & PHP_PQ_DECLARE_BINARY) ? "BINARY" : "",
			(flags & PHP_PQ_DECLARE_INSENSITIVE) ? "INSENSITIVE" : "",
			(flags & PHP_PQ_DECLARE_NO_SCROLL) ? "NO SCROLL" :
					(flags & PHP_PQ_DECLARE_SCROLL) ? "SCROLL" : "",
			(flags & PHP_PQ_DECLARE_WITH_HOLD) ? "WITH HOLD" : "",
			query_str
	);
	return decl_str;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcur___construct, 0, 0, 4)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, flags)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_INFO(0, async)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcur, __construct) {
	zend_error_handling zeh;
	char *name_str, *query_str;
	int name_len, query_len;
	long flags;
	zval *zconn;
	STATUS rv;
	zend_bool async = 0;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Osls|b", &zconn, php_pqconn_class_entry, &name_str, &name_len, &flags, &query_str, &query_len, &async);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqcur_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (obj->intern) {
			throw_exce(EX_BAD_METHODCALL TSRMLS_CC, "pq\\Cursor already initialized");
		} if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *decl = php_pqcur_declare_str(name_str, name_len, flags, query_str, query_len);

			if (async) {
				rv = php_pqconn_declare_async(zconn, conn_obj, decl TSRMLS_CC);
			} else {
				rv = php_pqconn_declare(zconn, conn_obj, decl TSRMLS_CC);
			}

			if (SUCCESS != rv) {
				efree(decl);
			} else {
				php_pqcur_t *cur = ecalloc(1, sizeof(*cur));

				php_pq_object_addref(conn_obj TSRMLS_CC);
				cur->conn = conn_obj;
				cur->open = 1;
				cur->name = estrdup(name_str);
				cur->decl = decl;
				cur->flags = flags;
				obj->intern = cur;
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcur_open, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcur, open)
{
	cur_open(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcur_openAsync, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcur, openAsync)
{
	cur_open(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcur_close, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcur, close)
{
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (rv == SUCCESS) {
		php_pqcur_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Cursor not initialized");
		} else {
			cur_close(obj, 0, 0 TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcur_closeAsync, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcur, closeAsync)
{
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (rv == SUCCESS) {
		php_pqcur_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Cursor not initialized");
		} else {
			cur_close(obj, 1, 0 TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcur_fetch, 0, 0, 1)
	ZEND_ARG_INFO(0, spec)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcur, fetch)
{
	cur_fetch_or_move(INTERNAL_FUNCTION_PARAM_PASSTHRU, "fetch", 0);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcur_move, 0, 0, 0)
	ZEND_ARG_INFO(0, spec)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcur, move)
{
	cur_fetch_or_move(INTERNAL_FUNCTION_PARAM_PASSTHRU, "move", 0);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcur_fetchAsync, 0, 0, 0)
	ZEND_ARG_INFO(0, spec)
	ZEND_ARG_INFO(0, callback)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcur, fetchAsync)
{
	cur_fetch_or_move(INTERNAL_FUNCTION_PARAM_PASSTHRU, "fetch", 1);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcur_moveAsync, 0, 0, 0)
	ZEND_ARG_INFO(0, spec)
	ZEND_ARG_INFO(0, callback)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcur, moveAsync)
{
	cur_fetch_or_move(INTERNAL_FUNCTION_PARAM_PASSTHRU, "move", 1);
}

static zend_function_entry php_pqcur_methods[] = {
	PHP_ME(pqcur, __construct, ai_pqcur___construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqcur, open, ai_pqcur_open, ZEND_ACC_PUBLIC)
	PHP_ME(pqcur, openAsync, ai_pqcur_open, ZEND_ACC_PUBLIC)
	PHP_ME(pqcur, close, ai_pqcur_close, ZEND_ACC_PUBLIC)
	PHP_ME(pqcur, closeAsync, ai_pqcur_closeAsync, ZEND_ACC_PUBLIC)
	PHP_ME(pqcur, fetch, ai_pqcur_fetch, ZEND_ACC_PUBLIC)
	PHP_ME(pqcur, move, ai_pqcur_move, ZEND_ACC_PUBLIC)
	PHP_ME(pqcur, fetchAsync, ai_pqcur_fetchAsync, ZEND_ACC_PUBLIC)
	PHP_ME(pqcur, moveAsync, ai_pqcur_moveAsync, ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}
};

PHP_MSHUTDOWN_FUNCTION(pqcur)
{
	zend_hash_destroy(&php_pqcur_object_prophandlers);
	return SUCCESS;
}

PHP_MINIT_FUNCTION(pqcur)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Cursor", php_pqcur_methods);
	php_pqcur_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqcur_class_entry->create_object = php_pqcur_create_object;

	memcpy(&php_pqcur_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqcur_object_handlers.read_property = php_pq_object_read_prop;
	php_pqcur_object_handlers.write_property = php_pq_object_write_prop;
	php_pqcur_object_handlers.clone_obj = NULL;
	php_pqcur_object_handlers.get_property_ptr_ptr = NULL;
	php_pqcur_object_handlers.get_gc = NULL;
	php_pqcur_object_handlers.get_properties = php_pq_object_properties;
	php_pqcur_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqcur_object_prophandlers, 2, NULL, NULL, 1);

	zend_declare_class_constant_long(php_pqcur_class_entry, ZEND_STRL("BINARY"), PHP_PQ_DECLARE_BINARY TSRMLS_CC);
	zend_declare_class_constant_long(php_pqcur_class_entry, ZEND_STRL("INSENSITIVE"), PHP_PQ_DECLARE_INSENSITIVE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqcur_class_entry, ZEND_STRL("WITH_HOLD"), PHP_PQ_DECLARE_WITH_HOLD TSRMLS_CC);
	zend_declare_class_constant_long(php_pqcur_class_entry, ZEND_STRL("SCROLL"), PHP_PQ_DECLARE_SCROLL TSRMLS_CC);
	zend_declare_class_constant_long(php_pqcur_class_entry, ZEND_STRL("NO_SCROLL"), PHP_PQ_DECLARE_NO_SCROLL TSRMLS_CC);

	zend_declare_property_null(php_pqcur_class_entry, ZEND_STRL("name"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcur_object_read_name;
	zend_hash_add(&php_pqcur_object_prophandlers, "name", sizeof("name"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqcur_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcur_object_read_connection;
	zend_hash_add(&php_pqcur_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqcur_class_entry, ZEND_STRL("flags"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcur_object_read_flags;
	zend_hash_add(&php_pqcur_object_prophandlers, "flags", sizeof("flags"), (void *) &ph, sizeof(ph), NULL);

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
