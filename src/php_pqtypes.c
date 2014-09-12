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
#include "php_pqres.h"
#include "php_pqtypes.h"

zend_class_entry *php_pqtypes_class_entry;
static zend_object_handlers php_pqtypes_object_handlers;
static HashTable php_pqtypes_object_prophandlers;

static void php_pqtypes_object_free(void *o TSRMLS_DC)
{
	php_pqtypes_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE types(#%d) %p (conn(#%d): %p)\n", obj->zv.handle, obj, obj->intern->conn->zv.handle, obj->intern->conn);
#endif
	if (obj->intern) {
		zend_hash_destroy(&obj->intern->types);
		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

zend_object_value php_pqtypes_create_object_ex(zend_class_entry *ce, php_pqtypes_t *intern, php_pqtypes_object_t **ptr TSRMLS_DC)
{
	php_pqtypes_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqtypes_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqtypes_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqtypes_object_handlers;

	return o->zv;
}

static zend_object_value php_pqtypes_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqtypes_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static void php_pqtypes_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtypes_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static int has_dimension(HashTable *ht, zval *member, char **key_str, int *key_len, ulong *index TSRMLS_DC)
{
	long lval = 0;
	zval *tmp = member;

	switch (Z_TYPE_P(member)) {
	default:
		convert_to_string_ex(&tmp);
		/* no break */
	case IS_STRING:
		if (!is_numeric_string(Z_STRVAL_P(tmp), Z_STRLEN_P(tmp), &lval, NULL, 0)) {
			int exists = zend_hash_exists(ht, Z_STRVAL_P(tmp), Z_STRLEN_P(tmp) + 1);

			if (key_str) {
				*key_str = estrndup(Z_STRVAL_P(tmp), Z_STRLEN_P(tmp));
				if (key_len) {
					*key_len = Z_STRLEN_P(tmp) + 1;
				}
			}
			if (member != tmp) {
				zval_ptr_dtor(&tmp);
			}

			return exists;
		}
		break;
	case IS_LONG:
		lval = Z_LVAL_P(member);
		break;
	}

	if (member != tmp) {
		zval_ptr_dtor(&tmp);
	}
	if (index) {
		*index = lval;
	}
	return zend_hash_index_exists(ht, lval);
}

static int php_pqtypes_object_has_dimension(zval *object, zval *member, int check_empty TSRMLS_DC)
{
	php_pqtypes_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	char *key_str = NULL;
	int key_len = 0;
	ulong index = 0;

	if (check_empty) {
		if (has_dimension(&obj->intern->types, member, &key_str, &key_len, &index TSRMLS_CC)) {
			zval **data;

			if (key_str && key_len) {
				if (SUCCESS == zend_hash_find(&obj->intern->types, key_str, key_len, (void *) &data)) {
					efree(key_str);
					return Z_TYPE_PP(data) != IS_NULL;
				}
				efree(key_str);
				key_str = NULL;
			} else {
				if (SUCCESS == zend_hash_index_find(&obj->intern->types, index, (void *) &data)) {
					return Z_TYPE_PP(data) != IS_NULL;
				}
			}
		}
		if (key_str) {
			efree(key_str);
		}
	} else {
		return has_dimension(&obj->intern->types, member, NULL, NULL, NULL TSRMLS_CC);
	}

	return 0;
}

static zval *php_pqtypes_object_read_dimension(zval *object, zval *member, int type TSRMLS_DC)
{
	ulong index = 0;
	char *key_str = NULL;
	int key_len = 0;
	php_pqtypes_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);

	if (has_dimension(&obj->intern->types, member, &key_str, &key_len, &index TSRMLS_CC)) {
		zval **data;

		if (key_str && key_len) {
			if (SUCCESS == zend_hash_find(&obj->intern->types, key_str, key_len, (void *) &data)) {
				efree(key_str);
				return *data;
			}
		} else {
			if (SUCCESS == zend_hash_index_find(&obj->intern->types, index, (void *) &data)) {
				return *data;
			}
		}
	}

	if (key_str) {
		efree(key_str);
	}

	return NULL;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtypes_construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
	ZEND_ARG_ARRAY_INFO(0, namespaces, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtypes, __construct) {
	zend_error_handling zeh;
	zval *zconn, *znsp = NULL;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|a!", &zconn, php_pqconn_class_entry, &znsp);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			php_pqtypes_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
			zval *retval = NULL;

			obj->intern = ecalloc(1, sizeof(*obj->intern));
			obj->intern->conn = conn_obj;
			php_pq_object_addref(conn_obj TSRMLS_CC);
			zend_hash_init(&obj->intern->types, 300, NULL, ZVAL_PTR_DTOR, 0);

			if (znsp) {
				zend_call_method_with_1_params(&getThis(), Z_OBJCE_P(getThis()), NULL, "refresh", &retval, znsp);
			} else {
				zend_call_method_with_0_params(&getThis(), Z_OBJCE_P(getThis()), NULL, "refresh", &retval);
			}

			if (retval) {
				zval_ptr_dtor(&retval);
			}
		}
	}
}

#define PHP_PQ_TYPES_QUERY \
	"select t.oid, t.* " \
	"from pg_type t join pg_namespace n on t.typnamespace=n.oid " \
	"where typisdefined " \
	"and typrelid=0"
#ifndef PHP_PQ_OID_TEXT
# define PHP_PQ_OID_TEXT 25
#endif

static int apply_nsp(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	zval **zp = p;
	unsigned pcount, tcount;
	php_pq_params_t *params = va_arg(argv, php_pq_params_t *);
	smart_str *str = va_arg(argv, smart_str *);

	tcount = php_pq_params_add_type_oid(params, PHP_PQ_OID_TEXT);
	pcount = php_pq_params_add_param(params, *zp);

	if (tcount != pcount) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Param/Type count mismatch");
		return ZEND_HASH_APPLY_STOP;
	}
	if (pcount > 1) {
		smart_str_appendc(str, ',');
	}
	smart_str_appendc(str, '$');
	smart_str_append_unsigned(str, pcount);

	return ZEND_HASH_APPLY_KEEP;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtypes_refresh, 0, 0, 0)
	ZEND_ARG_ARRAY_INFO(0, namespaces, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtypes, refresh) {
	HashTable *nsp = NULL;
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|H/!", &nsp);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtypes_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Types not initialized");
		} else {
			PGresult *res;

			if (!nsp || !zend_hash_num_elements(nsp)) {
				res = PQexec(obj->intern->conn->intern->conn, PHP_PQ_TYPES_QUERY " and nspname in ('public', 'pg_catalog')");
			} else {
				smart_str str = {0};
				php_pq_params_t *params = php_pq_params_init(&obj->intern->conn->intern->converters, NULL, NULL TSRMLS_CC);

				smart_str_appends(&str, PHP_PQ_TYPES_QUERY " and nspname in(");
				zend_hash_apply_with_arguments(nsp TSRMLS_CC, apply_nsp, 2, params, &str);
				smart_str_appendc(&str, ')');
				smart_str_0(&str);

				res = PQexecParams(obj->intern->conn->intern->conn, str.c, params->param.count, params->type.oids, (const char *const*) params->param.strings, NULL, NULL, 0);

				smart_str_free(&str);
				php_pq_params_free(&params);
			}

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to fetch types (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					int r, rows;

					for (r = 0, rows = PQntuples(res); r < rows; ++r) {
						zval *row = php_pqres_row_to_zval(res, r, PHP_PQRES_FETCH_OBJECT, NULL TSRMLS_CC);
						long oid = atol(PQgetvalue(res, r, 0 ));
						char *name = PQgetvalue(res, r, 1);

						Z_ADDREF_P(row);

						zend_hash_index_update(&obj->intern->types, oid, (void *) &row, sizeof(zval *), NULL);
						zend_hash_update(&obj->intern->types, name, strlen(name) + 1, (void *) &row, sizeof(zval *), NULL);
					}
				}

				PHP_PQclear(res);
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			}
		}
	}
}

static zend_function_entry php_pqtypes_methods[] = {
	PHP_ME(pqtypes, __construct, ai_pqtypes_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqtypes, refresh, ai_pqtypes_refresh, ZEND_ACC_PUBLIC)
	{0}
};

PHP_MSHUTDOWN_FUNCTION(pqtypes)
{
	zend_hash_destroy(&php_pqtypes_object_prophandlers);
	return SUCCESS;
}

PHP_MINIT_FUNCTION(pqtypes)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Types", php_pqtypes_methods);
	php_pqtypes_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqtypes_class_entry->create_object = php_pqtypes_create_object;

	memcpy(&php_pqtypes_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqtypes_object_handlers.read_property = php_pq_object_read_prop;
	php_pqtypes_object_handlers.write_property = php_pq_object_write_prop;
	php_pqtypes_object_handlers.clone_obj = NULL;
	php_pqtypes_object_handlers.get_property_ptr_ptr = NULL;
	php_pqtypes_object_handlers.get_gc = NULL;
	php_pqtypes_object_handlers.get_properties = php_pq_object_properties;
	php_pqtypes_object_handlers.get_debug_info = php_pq_object_debug_info;
	php_pqtypes_object_handlers.has_dimension = php_pqtypes_object_has_dimension;
	php_pqtypes_object_handlers.read_dimension = php_pqtypes_object_read_dimension;
	php_pqtypes_object_handlers.unset_dimension = NULL;
	php_pqtypes_object_handlers.write_dimension = NULL;

	zend_hash_init(&php_pqtypes_object_prophandlers, 1, NULL, NULL, 1);

	zend_declare_property_null(php_pqtypes_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtypes_object_read_connection;
	zend_hash_add(&php_pqtypes_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

#	undef PHP_PQ_TYPE
#	define PHP_PQ_TYPE(name, oid) zend_declare_class_constant_long(php_pqtypes_class_entry, ZEND_STRL(name), oid TSRMLS_CC);
#	include "php_pq_type.h"

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
