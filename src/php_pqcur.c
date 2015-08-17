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

static void cur_close(php_pqcur_object_t *obj, zend_bool async, zend_bool silent)
{
	if (obj->intern->open && obj->intern->conn->intern) {
		PGresult *res;
		smart_str cmd = {0};

		smart_str_appends(&cmd, "CLOSE ");
		smart_str_appends(&cmd, obj->intern->name);
		smart_str_0(&cmd);

		if (async) {
			if (PQsendQuery(obj->intern->conn->intern->conn, smart_str_v(&cmd))) {
				obj->intern->conn->intern->poller = PQconsumeInput;
				php_pqconn_notify_listeners(obj->intern->conn);
			} else if (!silent) {
				throw_exce(EX_IO, "Failed to close cursor (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}
		} else {
			if ((res = PQexec(obj->intern->conn->intern->conn, smart_str_v(&cmd)))) {
				PHP_PQclear(res);
			} else if (!silent) {
				throw_exce(EX_RUNTIME, "Failed to close cursor (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}
		}

		smart_str_free(&cmd);
		obj->intern->open = 0;
	}
}

static void cur_open(INTERNAL_FUNCTION_PARAMETERS, zend_bool async)
{
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;
	php_pqcur_object_t *obj;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (rv == FAILURE) {
		return;
	}

	obj = PHP_PQ_OBJ(getThis(), NULL);

	if (!obj->intern) {
		throw_exce(EX_UNINITIALIZED, "pq\\Cursor not initialized");
		return;
	} else if (obj->intern->open) {
		return;
	}

	if (async) {
		rv = php_pqconn_declare_async(NULL, obj->intern->conn, obj->intern->decl);
	} else {
		rv = php_pqconn_declare(NULL, obj->intern->conn, obj->intern->decl);
	}

	if (rv == SUCCESS) {
		obj->intern->open = 1;
	}
}

static void cur_fetch_or_move(INTERNAL_FUNCTION_PARAMETERS, const char *action, zend_bool async)
{
	char *spec_str = "1";
	size_t spec_len = 1;
	ZEND_RESULT_CODE rv;
	php_pq_callback_t resolver = {{0}};
	zend_error_handling zeh;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), async ? "|sf" : "|s", &spec_str, &spec_len, &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqcur_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Cursor not initialized");
		} else {
			smart_str cmd = {0};

			smart_str_appends(&cmd, *action == 'f' ? "FETCH " : "MOVE ");
			smart_str_appendl(&cmd, spec_str, spec_len);
			smart_str_appends(&cmd, " FROM ");
			smart_str_appends(&cmd, obj->intern->name);
			smart_str_0(&cmd);

			if (async) {
				int rc = PQsendQuery(obj->intern->conn->intern->conn, smart_str_v(&cmd));

				if (!rc) {
					throw_exce(EX_IO, "Failed to %s cursor (%s)", *action == 'f' ? "fetch from" : "move in", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
#if HAVE_PQSETSINGLEROWMODE
				} else if (obj->intern->conn->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn->intern->conn)) {
					throw_exce(EX_RUNTIME, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
#endif
				} else {
					php_pq_callback_recurse(&obj->intern->conn->intern->onevent, &resolver);
					obj->intern->conn->intern->poller = PQconsumeInput;
				}
			} else {
				PGresult *res = PQexec(obj->intern->conn->intern->conn, smart_str_v(&cmd));

				if (!res) {
					throw_exce(EX_RUNTIME, "Failed to %s cursor (%s)", *action == 'f' ? "fetch from" : "move in", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else if (SUCCESS == php_pqres_success(res)) {
					php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), return_value);

				}
			}
			smart_str_free(&cmd);
			php_pqconn_notify_listeners(obj->intern->conn);
		}
	}
}

static void php_pqcur_object_free(zend_object *o)
{
	php_pqcur_object_t *obj = PHP_PQ_OBJ(NULL, o);
#if DBG_GC
	fprintf(stderr, "FREE cur(#%d) %p (conn: %p)\n", obj->zo.handle, obj, obj->intern->conn);
#endif
	if (obj->intern) {
		cur_close(obj, 0, 1);
		php_pq_object_delref(obj->intern->conn);
		efree(obj->intern->decl);
		efree(obj->intern->name);
		efree(obj->intern);
		obj->intern = NULL;
	}
	php_pq_object_dtor(o);
}

php_pqcur_object_t *php_pqcur_create_object_ex(zend_class_entry *ce, php_pqcur_t *intern)
{
	return php_pq_object_create(ce, intern, sizeof(php_pqcur_object_t),
			&php_pqcur_object_handlers, &php_pqcur_object_prophandlers);
}

static zend_object *php_pqcur_create_object(zend_class_entry *class_type)
{
	return &php_pqcur_create_object_ex(class_type, NULL)->zo;
}

static void php_pqcur_object_read_name(zval *object, void *o, zval *return_value)
{
	php_pqcur_object_t *obj = o;

	RETVAL_STRING(obj->intern->name);
}

static void php_pqcur_object_read_connection(zval *object, void *o, zval *return_value)
{
	php_pqcur_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, return_value);
}

static void php_pqcur_object_gc_connection(zval *object, void *o, zval *return_value)
{
	php_pqcur_object_t *obj = o;
	zval zconn;

	php_pq_object_to_zval_no_addref(obj->intern->conn, &zconn);
	add_next_index_zval(return_value, &zconn);
}

static void php_pqcur_object_read_query(zval *object, void *o, zval *return_value)
{
	php_pqcur_object_t *obj = o;

	RETVAL_STRING(obj->intern->decl + obj->intern->query_offset);
}

static void php_pqcur_object_read_flags(zval *object, void *o, zval *return_value)
{
	php_pqcur_object_t *obj = o;

	RETVAL_LONG(obj->intern->flags);
}

char *php_pqcur_declare_str(const char *name_str, size_t name_len, unsigned flags, const char *query_str, size_t query_len, int *query_offset)
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

	if (query_offset) {
		/* sizeof() includes the terminating null byte, so no need for spaces in the string literals */
		*query_offset = sizeof("DECLARE")
			+ (name_len + 1)
			+ ((flags & PHP_PQ_DECLARE_BINARY) ? sizeof("BINARY") : 1)
			+ ((flags & PHP_PQ_DECLARE_INSENSITIVE) ? sizeof("INSENSITIVE") : 1)
			+ ((flags & PHP_PQ_DECLARE_NO_SCROLL) ? sizeof("NO SCROLL") :
					(flags & PHP_PQ_DECLARE_SCROLL) ? sizeof("SCROLL") : 1)
			+ sizeof("CURSOR")
			+ ((flags & PHP_PQ_DECLARE_WITH_HOLD) ? sizeof("WITH HOLD") : 1)
			+ sizeof("FOR");
	}

	return decl_str;
}

php_pqcur_t *php_pqcur_init(php_pqconn_object_t *conn, const char *name, char *decl, int query_offset, long flags)
{
	php_pqcur_t *cur = ecalloc(1, sizeof(*cur));

	php_pq_object_addref(conn);
	cur->conn = conn;
	cur->name = estrdup(name);
	cur->decl = decl;
	cur->query_offset = query_offset;
	cur->flags = flags;
	cur->open = 1;

	return cur;
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
	size_t name_len, query_len;
	zend_long flags;
	zval *zconn;
	ZEND_RESULT_CODE rv;
	zend_bool async = 0;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "Osls|b", &zconn, php_pqconn_class_entry, &name_str, &name_len, &flags, &query_str, &query_len, &async);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqcur_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);
		php_pqconn_object_t *conn_obj = PHP_PQ_OBJ(zconn, NULL);

		if (obj->intern) {
			throw_exce(EX_BAD_METHODCALL, "pq\\Cursor already initialized");
		} if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			int query_offset;
			char *decl = php_pqcur_declare_str(name_str, name_len, flags, query_str, query_len, &query_offset);

			if (async) {
				rv = php_pqconn_declare_async(zconn, conn_obj, decl);
			} else {
				rv = php_pqconn_declare(zconn, conn_obj, decl);
			}

			if (SUCCESS != rv) {
				efree(decl);
			} else {
				obj->intern = php_pqcur_init(conn_obj, name_str, decl, query_offset, flags);
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
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (rv == SUCCESS) {
		php_pqcur_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Cursor not initialized");
		} else {
			cur_close(obj, 0, 0);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcur_closeAsync, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcur, closeAsync)
{
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (rv == SUCCESS) {
		php_pqcur_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Cursor not initialized");
		} else {
			cur_close(obj, 1, 0);
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
	PHP_ME(pqcur, openAsync, ai_pqcur_openAsync, ZEND_ACC_PUBLIC)
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
	php_pqcur_class_entry = zend_register_internal_class_ex(&ce, NULL);
	php_pqcur_class_entry->create_object = php_pqcur_create_object;

	memcpy(&php_pqcur_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqcur_object_handlers.offset = XtOffsetOf(php_pqcur_object_t, zo);
	php_pqcur_object_handlers.free_obj = php_pqcur_object_free;
	php_pqcur_object_handlers.read_property = php_pq_object_read_prop;
	php_pqcur_object_handlers.write_property = php_pq_object_write_prop;
	php_pqcur_object_handlers.clone_obj = NULL;
	php_pqcur_object_handlers.get_property_ptr_ptr = NULL;
	php_pqcur_object_handlers.get_gc = php_pq_object_get_gc;
	php_pqcur_object_handlers.get_properties = php_pq_object_properties;
	php_pqcur_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqcur_object_prophandlers, 4, NULL, php_pq_object_prophandler_dtor, 1);

	zend_declare_class_constant_long(php_pqcur_class_entry, ZEND_STRL("BINARY"), PHP_PQ_DECLARE_BINARY);
	zend_declare_class_constant_long(php_pqcur_class_entry, ZEND_STRL("INSENSITIVE"), PHP_PQ_DECLARE_INSENSITIVE);
	zend_declare_class_constant_long(php_pqcur_class_entry, ZEND_STRL("WITH_HOLD"), PHP_PQ_DECLARE_WITH_HOLD);
	zend_declare_class_constant_long(php_pqcur_class_entry, ZEND_STRL("SCROLL"), PHP_PQ_DECLARE_SCROLL);
	zend_declare_class_constant_long(php_pqcur_class_entry, ZEND_STRL("NO_SCROLL"), PHP_PQ_DECLARE_NO_SCROLL);

	zend_declare_property_null(php_pqcur_class_entry, ZEND_STRL("name"), ZEND_ACC_PUBLIC);
	ph.read = php_pqcur_object_read_name;
	zend_hash_str_add_mem(&php_pqcur_object_prophandlers, "name", sizeof("name")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqcur_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC);
	ph.read = php_pqcur_object_read_connection;
	ph.gc = php_pqcur_object_gc_connection;
	zend_hash_str_add_mem(&php_pqcur_object_prophandlers, "connection", sizeof("connection")-1, (void *) &ph, sizeof(ph));
	ph.gc = NULL;

	zend_declare_property_null(php_pqcur_class_entry, ZEND_STRL("query"), ZEND_ACC_PUBLIC);
	ph.read = php_pqcur_object_read_query;
	zend_hash_str_add_mem(&php_pqcur_object_prophandlers, "query", sizeof("query")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqcur_class_entry, ZEND_STRL("flags"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcur_object_read_flags;
	zend_hash_str_add_mem(&php_pqcur_object_prophandlers, "flags", sizeof("flags")-1, (void *) &ph, sizeof(ph));

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
