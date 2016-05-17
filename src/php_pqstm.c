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
#include "php_pqstm.h"

zend_class_entry *php_pqstm_class_entry;
static zend_object_handlers php_pqstm_object_handlers;
static HashTable php_pqstm_object_prophandlers;

static void php_pqstm_deallocate(php_pqstm_object_t *obj, zend_bool async, zend_bool silent TSRMLS_DC)
{
	if (obj->intern->allocated) {
		char *quoted_name = PQescapeIdentifier(obj->intern->conn->intern->conn, obj->intern->name, strlen(obj->intern->name));

		if (quoted_name) {
			smart_str cmd = {0};

			smart_str_appends(&cmd, "DEALLOCATE ");
			smart_str_appends(&cmd, quoted_name);
			smart_str_0(&cmd);

			if (async) {
				if (PQsendQuery(obj->intern->conn->intern->conn, cmd.c)) {
					obj->intern->conn->intern->poller = PQconsumeInput;
					php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
				} else if (!silent) {
					throw_exce(EX_IO TSRMLS_CC, "Failed to deallocate statement (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				}
			} else {
				PGresult *res;

				if ((res = php_pq_exec(obj->intern->conn->intern->conn, cmd.c))) {
					php_pqres_clear(res);
				} else if (!silent) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to deallocate statement (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				}
			}

			PQfreemem(quoted_name);
			smart_str_free(&cmd);
		}

		obj->intern->allocated = 0;
		zend_hash_del(&obj->intern->conn->intern->statements, obj->intern->name, strlen(obj->intern->name)+1);
	}
}

static void php_pqstm_object_free(void *o TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE stm(#%d) %p (conn(#%d): %p)\n", obj->zv.handle, obj, obj->intern->conn->zv.handle, obj->intern->conn);
#endif
	if (obj->intern) {
		if (obj->intern->conn->intern) {
			php_pq_callback_dtor(&obj->intern->conn->intern->onevent);
			php_pqstm_deallocate(obj, 0, 1 TSRMLS_CC);
			php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		}
		efree(obj->intern->name);
		efree(obj->intern->query);
		zend_hash_destroy(&obj->intern->bound);
		if (obj->intern->params) {
			php_pq_params_free(&obj->intern->params);
		}
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

zend_object_value php_pqstm_create_object_ex(zend_class_entry *ce, php_pqstm_t *intern, php_pqstm_object_t **ptr TSRMLS_DC)
{
	php_pqstm_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqstm_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqstm_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqstm_object_handlers;

	return o->zv;
}

static zend_object_value php_pqstm_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqstm_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static void php_pqstm_object_read_name(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;

	RETVAL_STRING(obj->intern->name, 1);
}

static void php_pqstm_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static void php_pqstm_object_read_query(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;

	RETVAL_STRING(obj->intern->query, 1);
}

static void php_pqstm_object_read_types(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	int i;
	php_pqstm_object_t *obj = o;

	array_init_size(return_value, obj->intern->params->type.count);
	for (i = 0; i < obj->intern->params->type.count; i++) {
		add_next_index_long(return_value, (long)obj->intern->params->type.oids[i]);
	}
}

php_pqstm_t *php_pqstm_init(php_pqconn_object_t *conn, const char *name, const char *query, php_pq_params_t *params TSRMLS_DC)
{
	php_pqstm_t *stm = ecalloc(1, sizeof(*stm));

	php_pq_object_addref(conn TSRMLS_CC);
	stm->conn = conn;
	stm->name = estrdup(name);
	stm->params = params;
	stm->query = estrdup(query);
	stm->allocated = 1;

	ZEND_INIT_SYMTABLE(&stm->bound);

	zend_hash_add(&conn->intern->statements, name, strlen(name)+1, &stm, sizeof(stm), NULL);

	return stm;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_construct, 0, 0, 3)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
	ZEND_ARG_INFO(0, async)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, __construct) {
	zend_error_handling zeh;
	zval *zconn, *ztypes = NULL;
	char *name_str, *query_str;
	int name_len, *query_len;
	zend_bool async = 0;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Oss|a/!b", &zconn, php_pqconn_class_entry, &name_str, &name_len, &query_str, &query_len, &ztypes, &async);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (obj->intern) {
			throw_exce(EX_BAD_METHODCALL TSRMLS_CC, "pq\\Statement already initialized");
		} else if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			php_pq_params_t *params = php_pq_params_init(&conn_obj->intern->converters, ztypes ? Z_ARRVAL_P(ztypes) : NULL, NULL TSRMLS_CC);

			if (async) {
				rv = php_pqconn_prepare_async(zconn, conn_obj, name_str, query_str, params TSRMLS_CC);
			} else {
				rv = php_pqconn_prepare(zconn, conn_obj, name_str, query_str, params TSRMLS_CC);
			}

			if (SUCCESS == rv) {
				obj->intern = php_pqstm_init(conn_obj, name_str, query_str, params TSRMLS_CC);
			}
		}
	}
}
ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_bind, 0, 0, 2)
	ZEND_ARG_INFO(0, param_no)
	ZEND_ARG_INFO(1, param_ref)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, bind) {
	long param_no;
	zval **param_ref;
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "lZ", &param_no, &param_ref);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else if (!obj->intern->allocated) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement has been deallocated");
		} else {
			SEPARATE_ZVAL_TO_MAKE_IS_REF(param_ref);
			Z_ADDREF_PP(param_ref);
			zend_hash_index_update(&obj->intern->bound, param_no, (void *) param_ref, sizeof(zval *), NULL);
			zend_hash_sort(&obj->intern->bound, zend_qsort, php_pq_compare_index, 0 TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_exec, 0, 0, 0)
	ZEND_ARG_ARRAY_INFO(0, params, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, exec) {
	zend_error_handling zeh;
	zval *zparams = NULL;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|a/!", &zparams);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else if (!obj->intern->allocated) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement has been deallocated");
		} else {
			PGresult *res;

			php_pq_params_set_params(obj->intern->params, zparams ? Z_ARRVAL_P(zparams) : &obj->intern->bound);
			res = php_pq_exec_prepared(obj->intern->conn->intern->conn, obj->intern->name, obj->intern->params->param.count, (const char *const*) obj->intern->params->param.strings, NULL, NULL, 0);
			php_pq_params_set_params(obj->intern->params, NULL);

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to execute statement (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
				php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), &return_value TSRMLS_CC);
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_exec_async, 0, 0, 0)
	ZEND_ARG_ARRAY_INFO(0, params, 1)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, execAsync) {
	zend_error_handling zeh;
	zval *zparams = NULL;
	php_pq_callback_t resolver = {{0}};
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|a/!f", &zparams, &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else if (!obj->intern->allocated) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement has been deallocated");
		} else {
			int rc;

			php_pq_params_set_params(obj->intern->params, zparams ? Z_ARRVAL_P(zparams) : &obj->intern->bound);
			rc = PQsendQueryPrepared(obj->intern->conn->intern->conn, obj->intern->name, obj->intern->params->param.count, (const char *const*) obj->intern->params->param.strings, NULL, NULL, 0);
			php_pq_params_set_params(obj->intern->params, NULL);

			if (!rc) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to execute statement (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
#if HAVE_PQSETSINGLEROWMODE
			} else if (obj->intern->conn->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn->intern->conn)) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
#endif
			} else {
				php_pq_callback_recurse(&obj->intern->conn->intern->onevent, &resolver TSRMLS_CC);
				obj->intern->conn->intern->poller = PQconsumeInput;
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_desc, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, desc) {
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else if (!obj->intern->allocated) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement has been deallocated");
		} else {
			PGresult *res = PQdescribePrepared(obj->intern->conn->intern->conn, obj->intern->name);

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to describe statement (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					int p, params;

					array_init(return_value);
					for (p = 0, params = PQnparams(res); p < params; ++p) {
						add_next_index_long(return_value, PQparamtype(res, p));
					}
				}
				php_pqres_clear(res);
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_desc_async, 0, 0, 1)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, descAsync) {
	zend_error_handling zeh;
	php_pq_callback_t resolver = {{0}};
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "f", &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else if (!obj->intern->allocated) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement has been deallocated");
		} else if (!PQsendDescribePrepared(obj->intern->conn->intern->conn, obj->intern->name)) {
			throw_exce(EX_IO TSRMLS_CC, "Failed to describe statement: %s", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
		} else {
			php_pq_callback_recurse(&obj->intern->conn->intern->onevent, &resolver TSRMLS_CC);
			obj->intern->conn->intern->poller = PQconsumeInput;
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

static zend_always_inline void php_pqstm_deallocate_handler(INTERNAL_FUNCTION_PARAMETERS, zend_bool async)
{
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (rv == SUCCESS) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else {
			php_pqstm_deallocate(obj, async, 0 TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_deallocate, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, deallocate)
{
	php_pqstm_deallocate_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_deallocate_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, deallocateAsync)
{
	php_pqstm_deallocate_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}

static inline void php_pqstm_prepare_handler(INTERNAL_FUNCTION_PARAMETERS, zend_bool async)
{
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (rv == SUCCESS) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else if (!obj->intern->allocated) {
			if (async) {
				rv = php_pqconn_prepare_async(NULL, obj->intern->conn, obj->intern->name, obj->intern->query, obj->intern->params TSRMLS_CC);
			} else {
				rv = php_pqconn_prepare(NULL, obj->intern->conn, obj->intern->name, obj->intern->query, obj->intern->params TSRMLS_CC);
			}

			if (SUCCESS == rv) {
				obj->intern->allocated = 1;

				zend_hash_add(&obj->intern->conn->intern->statements,
						obj->intern->name, strlen(obj->intern->name)+1, &obj->intern, sizeof(obj->intern), NULL);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_prepare, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, prepare)
{
	php_pqstm_prepare_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_prepare_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, prepareAsync)
{
	php_pqstm_prepare_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}

static zend_function_entry php_pqstm_methods[] = {
	PHP_ME(pqstm, __construct, ai_pqstm_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqstm, bind, ai_pqstm_bind, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, deallocate, ai_pqstm_deallocate, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, deallocateAsync, ai_pqstm_deallocate_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, desc, ai_pqstm_desc, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, descAsync, ai_pqstm_desc_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, exec, ai_pqstm_exec, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, execAsync, ai_pqstm_exec_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, prepare, ai_pqstm_prepare, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, prepareAsync, ai_pqstm_prepare_async, ZEND_ACC_PUBLIC)
	{0}
};

PHP_MSHUTDOWN_FUNCTION(pqstm)
{
	zend_hash_destroy(&php_pqstm_object_prophandlers);
	return SUCCESS;
}

PHP_MINIT_FUNCTION(pqstm)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Statement", php_pqstm_methods);
	php_pqstm_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqstm_class_entry->create_object = php_pqstm_create_object;

	memcpy(&php_pqstm_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqstm_object_handlers.read_property = php_pq_object_read_prop;
	php_pqstm_object_handlers.write_property = php_pq_object_write_prop;
	php_pqstm_object_handlers.clone_obj = NULL;
	php_pqstm_object_handlers.get_property_ptr_ptr = NULL;
	php_pqstm_object_handlers.get_gc = NULL;
	php_pqstm_object_handlers.get_properties = php_pq_object_properties;
	php_pqstm_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqstm_object_prophandlers, 2, NULL, NULL, 1);

	zend_declare_property_null(php_pqstm_class_entry, ZEND_STRL("name"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqstm_object_read_name;
	zend_hash_add(&php_pqstm_object_prophandlers, "name", sizeof("name"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqstm_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqstm_object_read_connection;
	zend_hash_add(&php_pqstm_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqstm_class_entry, ZEND_STRL("query"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqstm_object_read_query;
	zend_hash_add(&php_pqstm_object_prophandlers, "query", sizeof("query"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqstm_class_entry, ZEND_STRL("types"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqstm_object_read_types;
	zend_hash_add(&php_pqstm_object_prophandlers, "types", sizeof("types"), (void *) &ph, sizeof(ph), NULL);

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
