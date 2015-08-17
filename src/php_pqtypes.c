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
#include "php_pqres.h"
#include "php_pqtypes.h"

zend_class_entry *php_pqtypes_class_entry;
static zend_object_handlers php_pqtypes_object_handlers;
static HashTable php_pqtypes_object_prophandlers;

static void php_pqtypes_object_free(zend_object *o)
{
	php_pqtypes_object_t *obj = PHP_PQ_OBJ(NULL, o);
#if DBG_GC
	fprintf(stderr, "FREE types(#%d) %p (conn(#%d): %p)\n", obj->zo.handle, obj, obj->intern->conn->zo.handle, obj->intern->conn);
#endif
	if (obj->intern) {
		zend_hash_destroy(&obj->intern->types);
		php_pq_object_delref(obj->intern->conn);
		efree(obj->intern);
		obj->intern = NULL;
	}
	php_pq_object_dtor(o);
}

php_pqtypes_object_t *php_pqtypes_create_object_ex(zend_class_entry *ce, php_pqtypes_t *intern)
{
	return php_pq_object_create(ce, intern, sizeof(php_pqtypes_object_t),
			&php_pqtypes_object_handlers, &php_pqtypes_object_prophandlers);
}

static zend_object *php_pqtypes_create_object(zend_class_entry *class_type)
{
	return &php_pqtypes_create_object_ex(class_type, NULL)->zo;
}

static void php_pqtypes_object_read_connection(zval *object, void *o, zval *return_value)
{
	php_pqtypes_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, return_value);
}
static void php_pqtypes_object_gc_connection(zval *object, void *o, zval *return_value)
{
	php_pqtypes_object_t *obj = o;
	zval zconn;

	php_pq_object_to_zval_no_addref(obj->intern->conn, &zconn);
	add_next_index_zval(return_value, &zconn);
}

static int has_dimension(HashTable *ht, zval *member, zend_string **key, zend_long *index)
{
	if (Z_TYPE_P(member) == IS_LONG) {
		*index = Z_LVAL_P(member);

		check_index:
		return zend_hash_index_exists(ht, *index);
	} else {
		zend_string *str = zval_get_string(member);

		if (is_numeric_str_function(str, index, NULL)) {
			zend_string_release(str);
			goto check_index;
		}

		if (zend_hash_exists(ht, str)) {
			*key = str;
			return 1;
		}

		zend_string_release(str);
		return 0;
	}
}

static int php_pqtypes_object_has_dimension(zval *object, zval *member, int check_empty)
{
	php_pqtypes_object_t *obj = PHP_PQ_OBJ(object, NULL);
	zend_string *key = NULL;
	zend_long index = 0;

	if (has_dimension(&obj->intern->types, member, &key, &index)) {
		if (check_empty) {
			zval *data;

			if (key) {
				if ((data = zend_hash_find(&obj->intern->types, key))) {
					zend_string_release(key);
					return Z_TYPE_P(data) != IS_NULL;
				}
				zend_string_release(key);
			} else if ((data = zend_hash_index_find(&obj->intern->types, index))) {
				return Z_TYPE_P(data) != IS_NULL;
			}
		} else {
			return 1;
		}
	}

	return 0;
}

static zval *php_pqtypes_object_read_dimension(zval *object, zval *member, int type, zval *rv)
{
	php_pqtypes_object_t *obj = PHP_PQ_OBJ(object, NULL);
	zend_string *key = NULL;
	zend_long index = 0;
	zval *data = NULL;

	if (has_dimension(&obj->intern->types, member, &key, &index)) {
		if (key) {
			data = zend_hash_find(&obj->intern->types, key);
			zend_string_release(key);
		} else {
			data = zend_hash_index_find(&obj->intern->types, index);
		}
	}

	return data;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtypes_construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
	ZEND_ARG_ARRAY_INFO(0, namespaces, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtypes, __construct) {
	zend_error_handling zeh;
	zval *zconn, *znsp = NULL;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "O|a!", &zconn, php_pqconn_class_entry, &znsp);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *conn_obj = PHP_PQ_OBJ(zconn, NULL);

		if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			php_pqtypes_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

			obj->intern = ecalloc(1, sizeof(*obj->intern));
			obj->intern->conn = conn_obj;
			php_pq_object_addref(conn_obj);
			zend_hash_init(&obj->intern->types, 512, NULL, ZVAL_PTR_DTOR, 0);

			if (znsp) {
				zend_call_method_with_1_params(getThis(), Z_OBJCE_P(getThis()), NULL, "refresh", NULL, znsp);
			} else {
				zend_call_method_with_0_params(getThis(), Z_OBJCE_P(getThis()), NULL, "refresh", NULL);
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

static int apply_nsp(zval *zp, int argc, va_list argv, zend_hash_key *key)
{
	unsigned pcount, tcount;
	php_pq_params_t *params = va_arg(argv, php_pq_params_t *);
	smart_str *str = va_arg(argv, smart_str *);

	tcount = php_pq_params_add_type_oid(params, PHP_PQ_OID_TEXT);
	pcount = php_pq_params_add_param(params, zp);

	if (tcount != pcount) {
		php_error_docref(NULL, E_WARNING, "Param/Type count mismatch");
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
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|H/!", &nsp);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqtypes_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Types not initialized");
		} else {
			PGresult *res;

			if (!nsp || !zend_hash_num_elements(nsp)) {
				res = PQexec(obj->intern->conn->intern->conn, PHP_PQ_TYPES_QUERY " and nspname in ('public', 'pg_catalog')");
			} else {
				smart_str str = {0};
				php_pq_params_t *params = php_pq_params_init(&obj->intern->conn->intern->converters, NULL, NULL);

				smart_str_appends(&str, PHP_PQ_TYPES_QUERY " and nspname in(");
				zend_hash_apply_with_arguments(nsp TSRMLS_CC, apply_nsp, 2, params, &str);
				smart_str_appendc(&str, ')');
				smart_str_0(&str);

				res = PQexecParams(obj->intern->conn->intern->conn, smart_str_v(&str), params->param.count, params->type.oids, (const char *const*) params->param.strings, NULL, NULL, 0);

				smart_str_free(&str);
				php_pq_params_free(&params);
			}

			if (!res) {
				throw_exce(EX_RUNTIME, "Failed to fetch types (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res)) {
					int r, rows;

					for (r = 0, rows = PQntuples(res); r < rows; ++r) {
						zval tmp, *row;

						ZVAL_NULL(&tmp);
						row = php_pqres_row_to_zval(res, r, PHP_PQRES_FETCH_OBJECT, &tmp);
						Z_ADDREF_P(row);

						zend_hash_index_update(&obj->intern->types, atol(PQgetvalue(res, r, 0 )), row);
						zend_hash_str_update(&obj->intern->types, PQgetvalue(res, r, 1), PQgetlength(res, r, 1), row);
					}
				}

				PHP_PQclear(res);
				php_pqconn_notify_listeners(obj->intern->conn);
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
	php_pqtypes_class_entry = zend_register_internal_class_ex(&ce, NULL);
	php_pqtypes_class_entry->create_object = php_pqtypes_create_object;

	/*
	zend_class_implements(php_pqtypes_class_entry TSRMLS_CC, 1, zend_ce_arrayaccess);
	*/

	memcpy(&php_pqtypes_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqtypes_object_handlers.offset = XtOffsetOf(php_pqtypes_object_t, zo);
	php_pqtypes_object_handlers.free_obj = php_pqtypes_object_free;
	php_pqtypes_object_handlers.read_property = php_pq_object_read_prop;
	php_pqtypes_object_handlers.write_property = php_pq_object_write_prop;
	php_pqtypes_object_handlers.clone_obj = NULL;
	php_pqtypes_object_handlers.get_property_ptr_ptr = NULL;
	php_pqtypes_object_handlers.get_gc = php_pq_object_get_gc;
	php_pqtypes_object_handlers.get_properties = php_pq_object_properties;
	php_pqtypes_object_handlers.get_debug_info = php_pq_object_debug_info;
	php_pqtypes_object_handlers.has_dimension = php_pqtypes_object_has_dimension;
	php_pqtypes_object_handlers.read_dimension = php_pqtypes_object_read_dimension;
	php_pqtypes_object_handlers.unset_dimension = NULL;
	php_pqtypes_object_handlers.write_dimension = NULL;

	zend_hash_init(&php_pqtypes_object_prophandlers, 1, NULL, php_pq_object_prophandler_dtor, 1);

	zend_declare_property_null(php_pqtypes_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC);
	ph.read = php_pqtypes_object_read_connection;
	ph.gc = php_pqtypes_object_gc_connection;
	zend_hash_str_add_mem(&php_pqtypes_object_prophandlers, "connection", sizeof("connection")-1, (void *) &ph, sizeof(ph));
	ph.gc = NULL;

#	undef PHP_PQ_TYPE
#	define PHP_PQ_TYPE(name, oid) zend_declare_class_constant_long(php_pqtypes_class_entry, ZEND_STRL(name), oid);
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
