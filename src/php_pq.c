/*
    +--------------------------------------------------------------------+
    | PECL :: pq                                                         |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted provided that the conditions mentioned |
    | in the accompanying LICENSE file are met.                          |
    +--------------------------------------------------------------------+
    | Copyright (c) 2013, Michael Wallner <mike@php.net>                |
    +--------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include <php.h>
#include <Zend/zend_interfaces.h>
#include <ext/standard/info.h>
#include <ext/spl/spl_array.h>

#include <libpq-events.h>

#include "php_pq.h"

typedef int STATUS; /* SUCCESS/FAILURE */

/*
ZEND_DECLARE_MODULE_GLOBALS(pq)
*/

const zend_function_entry pq_functions[] = {
	{0}
};

/* {{{ pq_module_entry
 */
zend_module_entry pq_module_entry = {
	STANDARD_MODULE_HEADER,
	"pq",
	pq_functions,
	PHP_MINIT(pq),
	PHP_MSHUTDOWN(pq),
	NULL,/*PHP_RINIT(pq),*/
	NULL,/*PHP_RSHUTDOWN(pq),*/
	PHP_MINFO(pq),
	PHP_PQ_EXT_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PQ
ZEND_GET_MODULE(pq)
#endif

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("pq.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_pq_globals, pq_globals)
    STD_PHP_INI_ENTRY("pq.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_pq_globals, pq_globals)
PHP_INI_END()
*/
/* }}} */

/* {{{ php_pq_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_pq_init_globals(zend_pq_globals *pq_globals)
{
	pq_globals->global_value = 0;
	pq_globals->global_string = NULL;
}
*/
/* }}} */

static zend_class_entry *php_pqconn_class_entry;
static zend_class_entry *php_pqres_class_entry;
static zend_class_entry *php_pqstm_class_entry;

static zend_object_handlers php_pqconn_object_handlers;
static zend_object_handlers php_pqres_object_handlers;
static zend_object_handlers php_pqstm_object_handlers;

typedef struct php_pqconn_listener {
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
} php_pqconn_listener_t;

typedef struct php_pqconn_object {
	zend_object zo;
	PGconn *conn;
	int (*poller)(PGconn *);
	HashTable listeners;
	unsigned async:1;
} php_pqconn_object_t;

typedef enum php_pqres_fetch {
	PHP_PQRES_FETCH_ARRAY,
	PHP_PQRES_FETCH_ASSOC,
	PHP_PQRES_FETCH_OBJECT
} php_pqres_fetch_t;

typedef struct php_pqres_iterator {
	zend_object_iterator zi;
	zval *current_val;
	unsigned index;
	php_pqres_fetch_t fetch_type;
} php_pqres_iterator_t;

typedef struct php_pqres_object {
	zend_object zo;
	PGresult *res;
	php_pqres_iterator_t *iter;
} php_pqres_object_t;

typedef struct php_pqstm_object {
	zend_object zo;
	char *name;
	zval *conn;
} php_pqstm_object_t;

static zend_object_iterator_funcs php_pqres_iterator_funcs;

static zend_object_iterator *php_pqres_iterator_init(zend_class_entry *ce, zval *object, int by_ref TSRMLS_DC)
{
	php_pqres_iterator_t *iter;
	zval *prop, *zfetch_type;

	iter = ecalloc(1, sizeof(*iter));
	iter->zi.funcs = &php_pqres_iterator_funcs;
	iter->zi.data = object;
	Z_ADDREF_P(object);

	zfetch_type = prop = zend_read_property(ce, object, ZEND_STRL("fetchType"), 0 TSRMLS_CC);
	if (Z_TYPE_P(zfetch_type) != IS_LONG) {
		convert_to_long_ex(&zfetch_type);
	}
	iter->fetch_type = Z_LVAL_P(zfetch_type);
	if (zfetch_type != prop) {
		zval_ptr_dtor(&zfetch_type);
	}
	if (Z_REFCOUNT_P(prop)) {
		zval_ptr_dtor(&prop);
	} else {
		zval_dtor(prop);
		FREE_ZVAL(prop);
	}

	return (zend_object_iterator *) iter;
}

static void php_pqres_iterator_dtor(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	if (iter->current_val) {
		zval_ptr_dtor(&iter->current_val);
		iter->current_val = NULL;
	}
	zval_ptr_dtor((zval **) &iter->zi.data);
	efree(iter);
}

static STATUS php_pqres_iterator_valid(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;
	php_pqres_object_t *obj = zend_object_store_get_object(iter->zi.data TSRMLS_CC);

	if (PQresultStatus(obj->res) != PGRES_TUPLES_OK) {
		return FAILURE;
	}
	if (PQntuples(obj->res) <= iter->index) {
		return FAILURE;
	}

	return SUCCESS;
}

static zval *php_pqres_row_to_zval(PGresult *res, unsigned row, php_pqres_fetch_t fetch_type TSRMLS_DC)
{
	zval *data;
	int c, cols;

	MAKE_STD_ZVAL(data);
	if (PHP_PQRES_FETCH_OBJECT == fetch_type) {
		object_init(data);
	} else {
		array_init(data);
	}

	for (c = 0, cols = PQnfields(res); c < cols; ++c) {
		if (PQgetisnull(res, row, c)) {
			switch (fetch_type) {
			case PHP_PQRES_FETCH_OBJECT:
				add_property_null(data, PQfname(res, c));
				break;

			case PHP_PQRES_FETCH_ASSOC:
				add_assoc_null(data, PQfname(res, c));
				break;

			case PHP_PQRES_FETCH_ARRAY:
				add_index_null(data, c);
				break;
			}
		} else {
			char *val = PQgetvalue(res, row, c);
			int len = PQgetlength(res, row, c);

			switch (fetch_type) {
			case PHP_PQRES_FETCH_OBJECT:
				add_property_stringl(data, PQfname(res, c), val, len, 1);
				break;

			case PHP_PQRES_FETCH_ASSOC:
				add_assoc_stringl(data, PQfname(res, c), val, len, 1);
				break;

			case PHP_PQRES_FETCH_ARRAY:
				add_index_stringl(data, c, val, len ,1);
				break;
			}
		}
	}

	return data;
}

static void php_pqres_iterator_current(zend_object_iterator *i, zval ***data_ptr TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;
	php_pqres_object_t *obj = zend_object_store_get_object(iter->zi.data TSRMLS_CC);

	if (iter->current_val) {
		zval_ptr_dtor(&iter->current_val);
	}
	iter->current_val = php_pqres_row_to_zval(obj->res, iter->index, iter->fetch_type TSRMLS_CC);
	*data_ptr = &iter->current_val;
}

static int php_pqres_iterator_key(zend_object_iterator *i, char **key_str, uint *key_len, ulong *key_num TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	*key_num = (ulong) iter->index;

	return HASH_KEY_IS_LONG;
}

static void php_pqres_iterator_next(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	++iter->index;
}

static void php_pqres_iterator_rewind(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	iter->index = 0;
}

static zend_object_iterator_funcs php_pqres_iterator_funcs = {
	php_pqres_iterator_dtor,
	/* check for end of iteration (FAILURE or SUCCESS if data is valid) */
	php_pqres_iterator_valid,
	/* fetch the item data for the current element */
	php_pqres_iterator_current,
	/* fetch the key for the current element (return HASH_KEY_IS_STRING or HASH_KEY_IS_LONG) (optional, may be NULL) */
	php_pqres_iterator_key,
	/* step forwards to next element */
	php_pqres_iterator_next,
	/* rewind to start of data (optional, may be NULL) */
	php_pqres_iterator_rewind,
	/* invalidate current value/key (optional, may be NULL) */
	NULL
};

static void php_pqconn_object_free(void *o TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	if (obj->conn) {
		PQfinish(obj->conn);
		obj->conn = NULL;
	}
	zend_hash_destroy(&obj->listeners);
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static void php_pqres_object_free(void *o TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	if (obj->res) {
		PQclear(obj->res);
		obj->res = NULL;
	}
	if (obj->iter) {
		php_pqres_iterator_dtor((zend_object_iterator *) obj->iter TSRMLS_CC);
		obj->iter = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static void php_pqstm_object_free(void *o TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;

	if (obj->name) {
		efree(obj->name);
		obj->name = NULL;
	}
	if (obj->conn) {
		zval_ptr_dtor(&obj->conn);
		obj->conn = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static zend_object_value php_pqconn_create_object_ex(zend_class_entry *ce, PGconn *conn, php_pqconn_object_t **ptr TSRMLS_DC)
{
	zend_object_value ov;
	php_pqconn_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);

	if (ptr) {
		*ptr = o;
	}

	if (conn) {
		o->conn = conn;
		o->async = !PQisnonblocking(o->conn);
	}

	zend_hash_init(&o->listeners, 0, NULL, (dtor_func_t) zend_hash_destroy, 0);

	ov.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqconn_object_free, NULL TSRMLS_CC);
	ov.handlers = &php_pqconn_object_handlers;

	return ov;
}

static zend_object_value php_pqres_create_object_ex(zend_class_entry *ce, PGresult *res, php_pqres_object_t **ptr TSRMLS_DC)
{
	zend_object_value ov;
	php_pqres_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);

	if (ptr) {
		*ptr = o;
	}

	if (res) {
		o->res = res;
	}

	ov.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqres_object_free, NULL TSRMLS_CC);
	ov.handlers = &php_pqres_object_handlers;

	return ov;
}

static zend_object_value php_pqstm_create_object_ex(zend_class_entry *ce, zval *conn, const char *name, php_pqstm_object_t **ptr TSRMLS_DC)
{
	zend_object_value ov;
	php_pqstm_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);

	if (ptr) {
		*ptr = o;
	}

	if (conn) {
		Z_ADDREF_P(conn);
		o->conn = conn;
	}

	if (name) {
		o->name = estrdup(name);
	}

	ov.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqstm_object_free, NULL TSRMLS_CC);
	ov.handlers = &php_pqstm_object_handlers;

	return ov;
}

static zend_object_value php_pqconn_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqconn_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqres_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqres_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqstm_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqstm_create_object_ex(class_type, NULL, NULL, NULL TSRMLS_CC);
}

static HashTable php_pqconn_object_prophandlers;
static HashTable php_pqres_object_prophandlers;
static HashTable php_pqstm_object_prophandlers;

typedef void (*php_pq_object_prophandler_func_t)(zval *object, void *o, zval *return_value TSRMLS_DC);

typedef struct php_pq_object_prophandler {
	php_pq_object_prophandler_func_t read;
	php_pq_object_prophandler_func_t write;
} php_pq_object_prophandler_t;

static void php_pqconn_object_read_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQstatus(obj->conn));
}

static void php_pqconn_object_read_transaction_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQtransactionStatus(obj->conn));
}

static void php_pqconn_object_read_error_message(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *error = PQerrorMessage(obj->conn);

	if (error) {
		RETVAL_STRING(error, 1);
	} else {
		RETVAL_NULL();
	}
}

static int apply_notify_listener(void *p, void *arg TSRMLS_DC)
{
	php_pqconn_listener_t *listener = p;
	PGnotify *nfy = arg;
	zval *zpid, *zchannel, *zmessage;

	MAKE_STD_ZVAL(zpid);
	ZVAL_LONG(zpid, nfy->be_pid);
	MAKE_STD_ZVAL(zchannel);
	ZVAL_STRING(zchannel, nfy->relname, 1);
	MAKE_STD_ZVAL(zmessage);
	ZVAL_STRING(zmessage, nfy->extra, 1);

	zend_fcall_info_argn(&listener->fci TSRMLS_CC, 3, &zchannel, &zmessage, &zpid);
	zend_fcall_info_call(&listener->fci, &listener->fcc, NULL, NULL TSRMLS_CC);

	zval_ptr_dtor(&zchannel);
	zval_ptr_dtor(&zmessage);
	zval_ptr_dtor(&zpid);

	return ZEND_HASH_APPLY_KEEP;
}

static int apply_notify_listeners(void *p, void *arg TSRMLS_DC)
{
	HashTable *listeners = p;

	zend_hash_apply_with_argument(listeners, apply_notify_listener, arg TSRMLS_CC);

	return ZEND_HASH_APPLY_KEEP;
}

static void php_pqconn_notify_listeners(zval *this_ptr, php_pqconn_object_t *obj TSRMLS_DC)
{
	PGnotify *nfy;

	if (!obj) {
		obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	}

	while ((nfy = PQnotifies(obj->conn))) {
		zend_hash_apply_with_argument(&obj->listeners, apply_notify_listeners, nfy TSRMLS_CC);
		PQfreemem(nfy);
	}
}

/* FIXME: extend to types->nspname->typname */
#define PHP_PQ_TYPES_QUERY \
	"select t.oid, t.* " \
	"from pg_type t join pg_namespace n on t.typnamespace=n.oid " \
	"where typisdefined " \
	"and typrelid=0 " \
	"and nspname in ('public', 'pg_catalog')"
static void php_pqconn_object_read_types(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	PGresult *res = PQexec(obj->conn, PHP_PQ_TYPES_QUERY);

	php_pqconn_notify_listeners(object, obj TSRMLS_CC);

	/* FIXME: cache that */
	if (res) {
		if (PGRES_TUPLES_OK == PQresultStatus(res)) {
			int r, rows;
			zval *byoid, *byname;

			MAKE_STD_ZVAL(byoid);
			MAKE_STD_ZVAL(byname);
			object_init(byoid);
			object_init(byname);
			object_init(return_value);
			for (r = 0, rows = PQntuples(res); r < rows; ++r) {
				zval *row = php_pqres_row_to_zval(res, r, PHP_PQRES_FETCH_OBJECT TSRMLS_CC);

				add_property_zval(byoid, PQgetvalue(res, r, 0), row);
				add_property_zval(byname, PQgetvalue(res, r, 1), row);
				zval_ptr_dtor(&row);
			}

			add_property_zval(return_value, "byOid", byoid);
			add_property_zval(return_value, "byName", byname);
			zval_ptr_dtor(&byoid);
			zval_ptr_dtor(&byname);
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not fetch types: %s", PQresultErrorMessage(res));
		}
		PQclear(res);
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not fetch types: %s", PQerrorMessage(obj->conn));
	}
}

static void php_pqres_object_read_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQresultStatus(obj->res));
}

static void php_pqres_object_read_error_message(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;
	char *error = PQresultErrorMessage(obj->res);

	if (error) {
		RETVAL_STRING(error, 1);
	} else {
		RETVAL_NULL();
	}
}

static void php_pqres_object_read_num_rows(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQntuples(obj->res));
}

static void php_pqres_object_read_num_cols(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQnfields(obj->res));
}

static void php_pqres_object_read_affected_rows(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(atoi(PQcmdTuples(obj->res)));
}

static void php_pqres_object_read_fetch_type(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	if (obj->iter) {
		RETVAL_LONG(obj->iter->fetch_type);
	} else {
		RETVAL_LONG(PHP_PQRES_FETCH_ARRAY);
	}
}

static void php_pqres_object_write_fetch_type(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;
	zval *zfetch_type = value;

	if (Z_TYPE_P(zfetch_type) != IS_LONG) {
		convert_to_long_ex(&zfetch_type);
	}

	obj->iter->fetch_type = Z_LVAL_P(zfetch_type);

	if (zfetch_type != value) {
		zval_ptr_dtor(&zfetch_type);
	}
}

static void php_pqstm_object_read_name(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;

	RETVAL_STRING(obj->name, 1);
}

static void php_pqstm_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;

	RETVAL_ZVAL(obj->conn, 1, 0);
}

static zval *php_pqconn_object_read_prop(zval *object, zval *member, int type, const zend_literal *key TSRMLS_DC)
{
	php_pqconn_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;
	zval *return_value;

	if (!obj->conn) {
		zend_error(E_WARNING, "Connection not initialized");
	} else if ((SUCCESS == zend_hash_find(&php_pqconn_object_prophandlers, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) && handler->read) {
		if (type == BP_VAR_R) {
			ALLOC_ZVAL(return_value);
			Z_SET_REFCOUNT_P(return_value, 0);
			Z_UNSET_ISREF_P(return_value);

			handler->read(object, obj, return_value TSRMLS_CC);
		} else {
			zend_error(E_ERROR, "Cannot access pq\\Connection properties by reference or array key/index");
			return_value = NULL;
		}
	} else {
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	}

	return return_value;
}

static void php_pqconn_object_write_prop(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC)
{
	php_pqconn_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;

	if (SUCCESS == zend_hash_find(&php_pqconn_object_prophandlers, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (handler->write) {
			handler->write(object, obj, value TSRMLS_CC);
		}
	} else {
		zend_get_std_object_handlers()->write_property(object, member, value, key TSRMLS_CC);
	}
}

static zval *php_pqres_object_read_prop(zval *object, zval *member, int type, const zend_literal *key TSRMLS_DC)
{
	php_pqres_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;
	zval *return_value;

	if (!obj->res) {
		zend_error(E_WARNING, "Result not initialized");
	} else if (SUCCESS == zend_hash_find(&php_pqres_object_prophandlers, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (type == BP_VAR_R) {
			ALLOC_ZVAL(return_value);
			Z_SET_REFCOUNT_P(return_value, 0);
			Z_UNSET_ISREF_P(return_value);

			handler->read(object, obj, return_value TSRMLS_CC);
		} else {
			zend_error(E_ERROR, "Cannot access pq\\Result properties by reference or array key/index");
			return_value = NULL;
		}
	} else {
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	}

	return return_value;
}

static void php_pqres_object_write_prop(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC)
{
	php_pqres_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;

	if (!obj->res) {
		zend_error(E_WARNING, "Result not initialized");
	} else if (SUCCESS == zend_hash_find(&php_pqres_object_prophandlers, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (handler->write) {
			/* ensure obj->iter is initialized, for e.g. write_fetch_type */
			if (!obj->iter) {
				obj->iter = (php_pqres_iterator_t *) php_pqres_iterator_init(Z_OBJCE_P(object), object, 0 TSRMLS_CC);
				obj->iter->zi.funcs->rewind((zend_object_iterator *) obj->iter TSRMLS_CC);
			}
			handler->write(object, obj, value TSRMLS_CC);
		}
	} else {
		zend_get_std_object_handlers()->write_property(object, member, value, key TSRMLS_CC);
	}
}

static zval *php_pqstm_object_read_prop(zval *object, zval *member, int type, const zend_literal *key TSRMLS_DC)
{
	php_pqstm_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;
	zval *return_value;

	if (!obj->conn) {
		zend_error(E_WARNING, "Statement not initialized");
	} else if (SUCCESS == zend_hash_find(&php_pqstm_object_prophandlers, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (type == BP_VAR_R) {
			ALLOC_ZVAL(return_value);
			Z_SET_REFCOUNT_P(return_value, 0);
			Z_UNSET_ISREF_P(return_value);

			handler->read(object, obj, return_value TSRMLS_CC);
		} else {
			zend_error(E_ERROR, "Cannot access pq\\Statement properties by reference or array key/index");
			return_value = NULL;
		}
	} else {
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	}

	return return_value;
}

static void php_pqstm_object_write_prop(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC)
{
	php_pqstm_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;

	if (!obj->conn) {
		zend_error(E_WARNING, "Result not initialized");
	} else if (SUCCESS == zend_hash_find(&php_pqstm_object_prophandlers, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (handler->write) {
			handler->write(object, obj, value TSRMLS_CC);
		}
	} else {
		zend_get_std_object_handlers()->write_property(object, member, value, key TSRMLS_CC);
	}
}

static STATUS php_pqconn_update_socket(zval *this_ptr, php_pqconn_object_t *obj TSRMLS_DC)
{
	zval *zsocket, zmember;
	php_stream *stream;
	STATUS retval;
	int socket;
	
	if (!obj) {
		obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	}
	
	INIT_PZVAL(&zmember);
	ZVAL_STRINGL(&zmember, "socket", sizeof("socket")-1, 0);
	MAKE_STD_ZVAL(zsocket);
	
	if ((CONNECTION_BAD != PQstatus(obj->conn))
	&&	(-1 < (socket = PQsocket(obj->conn)))
	&&	(stream = php_stream_fopen_from_fd(socket, "r+b", NULL))) {
		php_stream_to_zval(stream, zsocket);
		retval = SUCCESS;
	} else {
		ZVAL_NULL(zsocket);
		retval = FAILURE;
	}
	zend_get_std_object_handlers()->write_property(getThis(), &zmember, zsocket, NULL TSRMLS_CC);
	zval_ptr_dtor(&zsocket);
	
	return retval;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_construct, 0, 0, 1)
	ZEND_ARG_INFO(0, dsn)
	ZEND_ARG_INFO(0, async)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, __construct) {
	zend_error_handling zeh;
	char *dsn_str;
	int dsn_len;
	zend_bool async = 0;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|b", &dsn_str, &dsn_len, &async)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			PQfinish(obj->conn);
		}
		if ((obj->async = async)) {
			obj->conn = PQconnectStart(dsn_str);
			obj->poller = (int (*)(PGconn*)) PQconnectPoll;
		} else {
			obj->conn = PQconnectdb(dsn_str);
		}
		
		if (SUCCESS != php_pqconn_update_socket(getThis(), obj TSRMLS_CC)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection failed: %s", PQerrorMessage(obj->conn));
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_reset, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, reset) {
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			if (obj->async) {
				if (PQresetStart(obj->conn)) {
					obj->poller = (int (*)(PGconn*)) PQresetPoll;
					RETURN_TRUE;
				}
			} else {
				PQreset(obj->conn);
				
				if (CONNECTION_OK == PQstatus(obj->conn)) {
					RETURN_TRUE;
				} else {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection reset failed: %s", PQerrorMessage(obj->conn));
				}
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
		}
		RETURN_FALSE;
	}
}

static void listener_dtor(void *l) {
	php_pqconn_listener_t *listener = l;

	zend_fcall_info_args_clear(&listener->fci, 1);

	zval_ptr_dtor(&listener->fci.function_name);
	if (listener->fci.object_ptr) {
		zval_ptr_dtor(&listener->fci.object_ptr);
	}
}

static void php_pqconn_add_listener(php_pqconn_object_t *obj, const char *channel_str, size_t channel_len, php_pqconn_listener_t *listener TSRMLS_DC)
{
	HashTable ht, *existing_listeners;

	Z_ADDREF_P(listener->fci.function_name);
	if (listener->fci.object_ptr) {
		Z_ADDREF_P(listener->fci.object_ptr);
	}
	if (SUCCESS == zend_hash_find(&obj->listeners, channel_str, channel_len + 1, (void *) &existing_listeners)) {
		zend_hash_next_index_insert(existing_listeners, (void *) listener, sizeof(*listener), NULL);
	} else {
		zend_hash_init(&ht, 1, NULL, (dtor_func_t) listener_dtor, 0);
		zend_hash_next_index_insert(&ht, (void *) listener, sizeof(*listener), NULL);
		zend_hash_add(&obj->listeners, channel_str, channel_len + 1, (void *) &ht, sizeof(HashTable), NULL);
	}
}

static STATUS php_pqres_success(PGresult *res TSRMLS_DC)
{
	switch (PQresultStatus(res)) {
	case PGRES_BAD_RESPONSE:
	case PGRES_NONFATAL_ERROR:
	case PGRES_FATAL_ERROR:
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", PQresultErrorMessage(res));
		return FAILURE;
	default:
		return SUCCESS;
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_listen, 0, 0, 0)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, listen) {
	char *channel_str = NULL;
	int channel_len = 0;
	php_pqconn_listener_t listener;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sf", &channel_str, &channel_len, &listener.fci, &listener.fcc)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		obj->poller = PQconsumeInput;

		if (obj->conn) {
			PGresult *res;
			char cmd[1024];

			slprintf(cmd, sizeof(cmd), "LISTEN %s", channel_str);
			res = PQexec(obj->conn, cmd);

			if (res) {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					php_pqconn_add_listener(obj, channel_str, channel_len, &listener TSRMLS_CC);
					RETVAL_TRUE;
				} else {
					RETVAL_FALSE;
				}
				PQclear(res);
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not install listener: %s", PQerrorMessage(obj->conn));
				RETVAL_FALSE;
			}

			php_pqconn_notify_listeners(getThis(), obj TSRMLS_CC);

		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
			RETVAL_FALSE;
		}
	}

}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_notify, 0, 0, 2)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, notify) {
	char *channel_str, *message_str;
	int channel_len, message_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &channel_str, &channel_len, &message_str, &message_len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			PGresult *res;
			char *params[2] = {channel_str, message_str};

			res = PQexecParams(obj->conn, "select pg_notify($1, $2)", 2, NULL, (const char *const*) params, NULL, NULL, 0);

			if (res) {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					RETVAL_TRUE;
				} else {
					RETVAL_FALSE;
				}
				PQclear(res);
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not notify listeners: %s", PQerrorMessage(obj->conn));
				RETVAL_FALSE;
			}

			php_pqconn_notify_listeners(getThis(), obj TSRMLS_CC);

		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
			RETVAL_FALSE;
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_poll, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, poll) {
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			if (obj->poller) {
				if (obj->poller == PQconsumeInput) {
					RETVAL_LONG(obj->poller(obj->conn) * PGRES_POLLING_OK);
					php_pqconn_notify_listeners(getThis(), obj TSRMLS_CC);
					return;
				} else {
					RETURN_LONG(obj->poller(obj->conn));
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "No asynchronous operation active");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
		}
		RETURN_FALSE;
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, exec) {
	zend_error_handling zeh;
	char *query_str;
	int query_len;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &query_str, &query_len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			PGresult *res = PQexec(obj->conn, query_str);

			php_pqconn_notify_listeners(getThis(), obj TSRMLS_CC);

			if (res) {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					return_value->type = IS_OBJECT;
					return_value->value.obj = php_pqres_create_object_ex(php_pqres_class_entry, res, NULL TSRMLS_CC);
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not execute query: %s", PQerrorMessage(obj->conn));
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static int apply_to_oid(void *p, void *arg TSRMLS_DC)
{
	Oid **types = arg;
	zval **ztype = p;

	if (Z_TYPE_PP(ztype) != IS_LONG) {
		convert_to_long_ex(ztype);
	}

	**types = Z_LVAL_PP(ztype);
	++*types;

	if (*ztype != *(zval **)p) {
		zval_ptr_dtor(ztype);
	}
	return ZEND_HASH_APPLY_KEEP;
}

static int apply_to_param(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	char ***params;
	HashTable *zdtor;
	zval **zparam = p;

	params = (char ***) va_arg(argv, char ***);
	zdtor = (HashTable *) va_arg(argv, HashTable *);

	if (Z_TYPE_PP(zparam) == IS_NULL) {
		**params = NULL;
		++*params;
	} else {
		if (Z_TYPE_PP(zparam) != IS_STRING) {
			convert_to_string_ex(zparam);
		}

		**params = Z_STRVAL_PP(zparam);
		++*params;

		if (*zparam != *(zval **)p) {
			zend_hash_next_index_insert(zdtor, zparam, sizeof(zval *), NULL);
		}
	}
	return ZEND_HASH_APPLY_KEEP;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec_params, 0, 0, 2)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, params, 0)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, execParams) {
	zend_error_handling zeh;
	char *query_str;
	int query_len;
	zval *zparams;
	zval *ztypes = NULL;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa/|a/!", &query_str, &query_len, &zparams, &ztypes)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			PGresult *res;
			int count = 0;
			Oid *types = NULL;
			char **params = NULL;
			HashTable zdtor;

			ZEND_INIT_SYMTABLE(&zdtor);

			if (ztypes && zend_hash_num_elements(Z_ARRVAL_P(ztypes))) {
				Oid *tmp;

				tmp = types = ecalloc(zend_hash_num_elements(Z_ARRVAL_P(ztypes)), sizeof(Oid));
				zend_hash_apply_with_argument(Z_ARRVAL_P(ztypes), apply_to_oid, &tmp TSRMLS_CC);
			}
			if ((count = zend_hash_num_elements(Z_ARRVAL_P(zparams)))) {
				char **tmp;

				tmp = params = ecalloc(zend_hash_num_elements(Z_ARRVAL_P(zparams)), sizeof(char *));
				zend_hash_apply_with_arguments(Z_ARRVAL_P(zparams) TSRMLS_CC, apply_to_param, 2, &tmp, &zdtor);
			}

			res = PQexecParams(obj->conn, query_str, count, types, (const char *const*) params, NULL, NULL, 0);

			zend_hash_destroy(&zdtor);
			if (types) {
				efree(types);
			}
			if (params) {
				efree(params);
			}

			php_pqconn_notify_listeners(getThis(), obj TSRMLS_CC);

			if (res) {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					return_value->type = IS_OBJECT;
					return_value->value.obj = php_pqres_create_object_ex(php_pqres_class_entry, res, NULL TSRMLS_CC);
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not execute query: %s", PQerrorMessage(obj->conn));
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static STATUS php_pqconn_prepare(PGconn *conn, const char *name, const char *query, HashTable *typest TSRMLS_DC)
{
	Oid *types = NULL;
	int count = 0;
	PGresult *res;

	if (typest && (count = zend_hash_num_elements(typest))) {
		Oid *tmp;

		tmp = types = ecalloc(count, sizeof(Oid));
		zend_hash_apply_with_argument(typest, apply_to_oid, &tmp TSRMLS_CC);
	}

	res = PQprepare(conn, name, query, count, types);

	if (types) {
		efree(types);
	}

	if (res) {
		if (PGRES_COMMAND_OK == PQresultStatus(res)) {
			return SUCCESS;
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not prepare statement: %s", PQresultErrorMessage(res));
		}
		PQclear(res);
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not prepare statement: %s", PQerrorMessage(conn));
	}
	return FAILURE;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_prepare, 0, 0, 2)
	ZEND_ARG_INFO(0, "name")
	ZEND_ARG_INFO(0, "query")
	ZEND_ARG_ARRAY_INFO(0, "types", 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, prepare) {
	zend_error_handling zeh;
	zval *ztypes = NULL;
	char *name_str, *query_str;
	int name_len, *query_len;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|a/!", &name_str, &name_len, &query_str, &query_len, &ztypes)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			if (SUCCESS == php_pqconn_prepare(obj->conn, name_str, query_str, ztypes ? Z_ARRVAL_P(ztypes) : NULL TSRMLS_CC)) {
				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqstm_create_object_ex(php_pqstm_class_entry, getThis(), name_str, NULL TSRMLS_CC);
			}
			php_pqconn_notify_listeners(getThis(), obj TSRMLS_CC);
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
		}
	}
}

static zend_function_entry php_pqconn_methods[] = {
	PHP_ME(pqconn, __construct, ai_pqconn_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqconn, reset, ai_pqconn_reset, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, poll, ai_pqconn_poll, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, exec, ai_pqconn_exec, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, execParams, ai_pqconn_exec_params, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, prepare, ai_pqconn_prepare, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, listen, ai_pqconn_listen, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, notify, ai_pqconn_notify, ZEND_ACC_PUBLIC)
	{0}
};

static zval **php_pqres_iteration(zval *this_ptr, php_pqres_object_t *obj, php_pqres_fetch_t fetch_type TSRMLS_DC)
{
	zval **row = NULL;
	php_pqres_fetch_t orig_fetch;

	if (!obj) {
	 obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	}

	if (!obj->iter) {
		obj->iter = (php_pqres_iterator_t *) php_pqres_iterator_init(Z_OBJCE_P(getThis()), getThis(), 0 TSRMLS_CC);
		obj->iter->zi.funcs->rewind((zend_object_iterator *) obj->iter TSRMLS_CC);
	}
	orig_fetch = obj->iter->fetch_type;
	obj->iter->fetch_type = fetch_type;
	if (SUCCESS == obj->iter->zi.funcs->valid((zend_object_iterator *) obj->iter TSRMLS_CC)) {
		obj->iter->zi.funcs->get_current_data((zend_object_iterator *) obj->iter, &row TSRMLS_CC);
		obj->iter->zi.funcs->move_forward((zend_object_iterator *) obj->iter TSRMLS_CC);
	}
	obj->iter->fetch_type = orig_fetch;

	return row ? row : NULL;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_row, 0, 0, 0)
	ZEND_ARG_INFO(0, fetch_type)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchRow) {
	zend_error_handling zeh;
	php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	long fetch_type = obj->iter ? obj->iter->fetch_type : PHP_PQRES_FETCH_ARRAY;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &fetch_type)) {
		zval **row = php_pqres_iteration(getThis(), obj, fetch_type TSRMLS_CC);

		if (row) {
			RETVAL_ZVAL(*row, 1, 0);
		} else {
			RETVAL_FALSE;
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static zval **column_at(zval *row, int col TSRMLS_DC)
{
	zval **data = NULL;
	HashTable *ht = HASH_OF(row);
	int count = zend_hash_num_elements(ht);

	if (col < count) {
		zend_hash_internal_pointer_reset(ht);
		while (col-- > 0) {
			zend_hash_move_forward(ht);
		}
		zend_hash_get_current_data(ht, (void *) &data);
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Column index %d does excess column count %d", col, count);
	}
	return data;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_col, 0, 0, 0)
	ZEND_ARG_INFO(0, col_num)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchCol) {
	zend_error_handling zeh;
	long fetch_col = 0;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &fetch_col)) {
		php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		zval **row = php_pqres_iteration(getThis(), obj, obj->iter ? obj->iter->fetch_type : 0 TSRMLS_CC);

		if (row) {
			zval **col = column_at(*row, fetch_col TSRMLS_CC);

			if (col) {
				RETVAL_ZVAL(*col, 1, 0);
			} else {
				RETVAL_FALSE;
			}
		} else {
			RETVAL_FALSE;
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);

}

static zend_function_entry php_pqres_methods[] = {
	PHP_ME(pqres, fetchRow, ai_pqres_fetch_row, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchCol, ai_pqres_fetch_col, ZEND_ACC_PUBLIC)
	{0}
};

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_construct, 0, 0, 3)
	ZEND_ARG_OBJ_INFO(0, "Connection", "pq\\Connection", 0)
	ZEND_ARG_INFO(0, "name")
	ZEND_ARG_INFO(0, "query")
	ZEND_ARG_ARRAY_INFO(0, "types", 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, __construct) {
	zend_error_handling zeh;
	zval *zconn, *ztypes = NULL;
	char *name_str, *query_str;
	int name_len, *query_len;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Oss|a/!", &zconn, php_pqconn_class_entry, &name_str, &name_len, &query_str, &query_len, &ztypes)) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (conn_obj->conn) {
			if (SUCCESS == php_pqconn_prepare(conn_obj->conn, name_str, query_str, ztypes ? Z_ARRVAL_P(ztypes) : NULL TSRMLS_CC)) {
				Z_ADDREF_P(zconn);
				obj->conn = zconn;
				obj->name = estrdup(name_str);
			}
			php_pqconn_notify_listeners(obj->conn, conn_obj TSRMLS_CC);
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_exec, 0, 0, 0)
	ZEND_ARG_ARRAY_INFO(0, "params", 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, exec) {
	zend_error_handling zeh;
	zval *zparams = NULL;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|a/!", &zparams)) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn && obj->name) {
			php_pqconn_object_t *conn_obj = zend_object_store_get_object(obj->conn TSRMLS_CC);

			if (conn_obj->conn) {
				int count = 0;
				char **params = NULL;
				HashTable zdtor;
				PGresult *res;

				ZEND_INIT_SYMTABLE(&zdtor);

				if (zparams && (count = zend_hash_num_elements(Z_ARRVAL_P(zparams)))) {
					char **tmp;

					tmp = params = ecalloc(count, sizeof(char *));
					zend_hash_apply_with_arguments(Z_ARRVAL_P(zparams) TSRMLS_CC, apply_to_param, 2, &tmp, &zdtor);
				}

				res = PQexecPrepared(conn_obj->conn, obj->name, count, (const char *const*) params, NULL, NULL, 0);

				if (params) {
					efree(params);
				}
				zend_hash_destroy(&zdtor);

				php_pqconn_notify_listeners(obj->conn, conn_obj TSRMLS_CC);

				if (res) {
					if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
						return_value->type = IS_OBJECT;
						return_value->value.obj = php_pqres_create_object_ex(php_pqres_class_entry, res, NULL TSRMLS_CC);
					}
				} else {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not execute statement: %s", PQerrorMessage(conn_obj->conn));
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Statement not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_desc, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, desc) {
	zend_error_handling zeh;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn && obj->name) {
			php_pqconn_object_t *conn_obj = zend_object_store_get_object(obj->conn TSRMLS_CC);

			if (conn_obj->conn) {
				PGresult *res = PQdescribePrepared(conn_obj->conn, obj->name);

				php_pqconn_notify_listeners(obj->conn, conn_obj TSRMLS_CC);

				if (res) {
					if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
						int p, params;

						array_init(return_value);
						for (p = 0, params = PQnparams(res); p < params; ++p) {
							add_next_index_long(return_value, PQparamtype(res, p));
						}
					}
				} else {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not describe statement: %s", PQerrorMessage(conn_obj->conn));
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Statement not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static zend_function_entry php_pqstm_methods[] = {
	PHP_ME(pqstm, __construct, ai_pqstm_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqstm, exec, ai_pqstm_exec, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, desc, ai_pqstm_desc, ZEND_ACC_PUBLIC)
	{0}
};

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(pq)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	zend_hash_init(&php_pqconn_object_prophandlers, 1, NULL, NULL, 1);
	INIT_NS_CLASS_ENTRY(ce, "pq", "Connection", php_pqconn_methods);
	php_pqconn_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqconn_class_entry->create_object = php_pqconn_create_object;
	memcpy(&php_pqconn_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqconn_object_handlers.read_property = php_pqconn_object_read_prop;
	php_pqconn_object_handlers.write_property = php_pqconn_object_write_prop;
	php_pqconn_object_handlers.clone_obj = NULL;
	php_pqconn_object_handlers.get_property_ptr_ptr = NULL;

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("status"), CONNECTION_BAD, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_status;
	zend_hash_add(&php_pqconn_object_prophandlers, "status", sizeof("status"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("transactionStatus"), PQTRANS_UNKNOWN, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_transaction_status;
	zend_hash_add(&php_pqconn_object_prophandlers, "transactionStatus", sizeof("transactionStatus"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("socket"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = NULL; /* forward to std prophandler */
	zend_hash_add(&php_pqconn_object_prophandlers, "socket", sizeof("socket"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("errorMessage"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_error_message;
	zend_hash_add(&php_pqconn_object_prophandlers, "errorMessage", sizeof("errorMessage"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("types"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_types;
	zend_hash_add(&php_pqconn_object_prophandlers, "types", sizeof("types"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("OK"), CONNECTION_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("BAD"), CONNECTION_BAD TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("STARTED"), CONNECTION_STARTED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("MADE"), CONNECTION_MADE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("AWAITING_RESPONSE"), CONNECTION_AWAITING_RESPONSE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("AUTH_OK"), CONNECTION_AUTH_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("SSL_STARTUP"), CONNECTION_SSL_STARTUP TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("SETENV"), CONNECTION_SETENV TSRMLS_CC);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_IDLE"), PQTRANS_IDLE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_ACTIVE"), PQTRANS_ACTIVE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_INTRANS"), PQTRANS_INTRANS TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_INERROR"), PQTRANS_INERROR TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_UNKNOWN"), PQTRANS_UNKNOWN TSRMLS_CC);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_FAILED"), PGRES_POLLING_FAILED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_READING"), PGRES_POLLING_READING TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_WRITING"), PGRES_POLLING_WRITING TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_OK"), PGRES_POLLING_OK TSRMLS_CC);

	zend_hash_init(&php_pqres_object_prophandlers, 1, NULL, NULL, 1);
	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Result", php_pqres_methods);
	php_pqres_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqres_class_entry->create_object = php_pqres_create_object;
	php_pqres_class_entry->iterator_funcs.funcs = &php_pqres_iterator_funcs;
	php_pqres_class_entry->get_iterator = php_pqres_iterator_init;

	memcpy(&php_pqres_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqres_object_handlers.read_property = php_pqres_object_read_prop;
	php_pqres_object_handlers.write_property = php_pqres_object_write_prop;

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("status"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_status;
	zend_hash_add(&php_pqres_object_prophandlers, "status", sizeof("status"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("errorMessage"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_error_message;
	zend_hash_add(&php_pqres_object_prophandlers, "errorMessage", sizeof("errorMessage"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("numRows"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_num_rows;
	zend_hash_add(&php_pqres_object_prophandlers, "numRows", sizeof("numRows"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("numCols"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_num_cols;
	zend_hash_add(&php_pqres_object_prophandlers, "numCols", sizeof("numCols"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("affectedRows"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_affected_rows;
	zend_hash_add(&php_pqres_object_prophandlers, "affectedRows", sizeof("affectedRows"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("fetchType"), PHP_PQRES_FETCH_ARRAY, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_fetch_type;
	ph.write = php_pqres_object_write_fetch_type;
	zend_hash_add(&php_pqres_object_prophandlers, "fetchType", sizeof("fetchType"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("EMPTY_QUERY"), PGRES_EMPTY_QUERY TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COMMAND_OK"), PGRES_COMMAND_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("TUPLES_OK"), PGRES_TUPLES_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_OUT"), PGRES_COPY_OUT TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_IN"), PGRES_COPY_IN TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("BAD_RESPONSE"), PGRES_BAD_RESPONSE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("NONFATAL_ERROR"), PGRES_NONFATAL_ERROR TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FATAL_ERROR"), PGRES_FATAL_ERROR TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_BOTH"), PGRES_COPY_BOTH TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("SINGLE_TUPLE"), PGRES_SINGLE_TUPLE TSRMLS_CC);

	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FETCH_ARRAY"), PHP_PQRES_FETCH_ARRAY TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FETCH_ASSOC"), PHP_PQRES_FETCH_ASSOC TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FETCH_OBJECT"), PHP_PQRES_FETCH_OBJECT TSRMLS_CC);

	zend_hash_init(&php_pqstm_object_prophandlers, 1, NULL, NULL, 1);
	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Statement", php_pqstm_methods);
	php_pqstm_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqstm_class_entry->create_object = php_pqstm_create_object;

	memcpy(&php_pqstm_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqstm_object_handlers.read_property = php_pqstm_object_read_prop;
	php_pqstm_object_handlers.write_property = php_pqstm_object_write_prop;

	zend_declare_property_null(php_pqstm_class_entry, ZEND_STRL("name"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqstm_object_read_name;
	zend_hash_add(&php_pqstm_object_prophandlers, "name", sizeof("name"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqstm_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqstm_object_read_connection;
	zend_hash_add(&php_pqstm_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	/*
	REGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(pq)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(pq)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "pq support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */



/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
