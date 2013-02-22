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

#define SMART_STR_PREALLOC 256

#include <php.h>
#include <Zend/zend_interfaces.h>
#include <Zend/zend_exceptions.h>
#include <ext/standard/info.h>
#include <ext/standard/php_smart_str.h>
#include <ext/spl/spl_array.h>
#include <ext/spl/spl_exceptions.h>
#include <ext/raphf/php_raphf.h>

#include <libpq-events.h>
#include <libpq/libpq-fs.h>
#include <fnmatch.h>

#include "php_pq.h"

typedef int STATUS; /* SUCCESS/FAILURE */

static char *rtrim(char *e) {
	size_t l = strlen(e);

	while (l-- > 0 && e[l] == '\n') {
		e[l] = '\0';
	}
	return e;
}

#define PHP_PQerrorMessage(c) rtrim(PQerrorMessage((c)))
#define PHP_PQresultErrorMessage(r) rtrim(PQresultErrorMessage((r)))

static int php_pqconn_event(PGEventId id, void *e, void *data);

#define PHP_PQclear(_r) \
	do { \
		php_pqres_object_t *_o = PQresultInstanceData((_r), php_pqconn_event); \
		if (_o) { \
			php_pq_object_delref(_o TSRMLS_CC); \
		} else { \
			PQclear(_r); \
		} \
	} while (0)

/*
ZEND_DECLARE_MODULE_GLOBALS(pq)
*/

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
static zend_class_entry *php_pqtypes_class_entry;
static zend_class_entry *php_pqres_class_entry;
static zend_class_entry *php_pqstm_class_entry;
static zend_class_entry *php_pqtxn_class_entry;
static zend_class_entry *php_pqcancel_class_entry;
static zend_class_entry *php_pqlob_class_entry;
static zend_class_entry *php_pqcopy_class_entry;

typedef enum php_pqexc_type {
	EX_INVALID_ARGUMENT,
	EX_RUNTIME,
	EX_CONNECTION_FAILED,
	EX_IO,
	EX_ESCAPE,
	EX_BAD_METHODCALL,
	EX_UNINITIALIZED,
	EX_DOMAIN,
	EX_SQL
} php_pqexc_type_t;

static zend_class_entry *php_pqexc_interface_class_entry;
static zend_class_entry *php_pqexc_invalid_argument_class_entry;
static zend_class_entry *php_pqexc_runtime_class_entry;
static zend_class_entry *php_pqexc_bad_methodcall_class_entry;
static zend_class_entry *php_pqexc_domain_class_entry;

static zend_class_entry *exce(php_pqexc_type_t type)
{
	switch (type) {
	default:
	case EX_INVALID_ARGUMENT:
		return php_pqexc_invalid_argument_class_entry;
	case EX_RUNTIME:
	case EX_CONNECTION_FAILED:
	case EX_IO:
	case EX_ESCAPE:
		return php_pqexc_runtime_class_entry;
	case EX_UNINITIALIZED:
	case EX_BAD_METHODCALL:
		return php_pqexc_bad_methodcall_class_entry;
	case EX_DOMAIN:
	case EX_SQL:
		return php_pqexc_domain_class_entry;
	}
}

static zval *throw_exce(php_pqexc_type_t type TSRMLS_DC, const char *fmt, ...)
{
	char *msg;
	zval *zexc;
	va_list argv;

	va_start(argv, fmt);
	vspprintf(&msg, 0, fmt, argv);
	va_end(argv);

	zexc = zend_throw_exception(exce(type), msg, type TSRMLS_CC);
	efree(msg);

	return zexc;
}

static zend_object_handlers php_pqconn_object_handlers;
static zend_object_handlers php_pqtypes_object_handlers;
static zend_object_handlers php_pqres_object_handlers;
static zend_object_handlers php_pqstm_object_handlers;
static zend_object_handlers php_pqtxn_object_handlers;
static zend_object_handlers php_pqcancel_object_handlers;
static zend_object_handlers php_pqlob_object_handlers;
static zend_object_handlers php_pqcopy_object_handlers;

typedef struct php_pq_callback {
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	void *data;
} php_pq_callback_t;

typedef struct php_pq_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	void *intern;
} php_pq_object_t;

#define PHP_PQCONN_ASYNC 0x01
#define PHP_PQCONN_PERSISTENT 0x02

typedef struct php_pqconn {
	PGconn *conn;
	int (*poller)(PGconn *);
	php_resource_factory_t factory;
	HashTable listeners;
	HashTable eventhandlers;
	php_pq_callback_t onevent;
	unsigned unbuffered:1;
} php_pqconn_t;

typedef struct php_pqconn_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqconn_t *intern;
} php_pqconn_object_t;

typedef struct php_pqtypes {
	HashTable types;
	php_pqconn_object_t *conn;
} php_pqtypes_t;

typedef struct php_pqtypes_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqtypes_t *intern;
} php_pqtypes_object_t;

typedef struct php_pqconn_event_data {
	php_pqconn_object_t *obj;
#ifdef ZTS
	void ***ts;
#endif
} php_pqconn_event_data_t;

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

typedef struct php_pqres {
	PGresult *res;
	php_pqres_iterator_t *iter;
	HashTable bound;
} php_pqres_t;

typedef struct php_pqres_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqres_t *intern;
} php_pqres_object_t;

typedef struct php_pqstm {
	php_pqconn_object_t *conn;
	char *name;
	HashTable bound;
} php_pqstm_t;

typedef struct php_pqstm_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqstm_t *intern;
} php_pqstm_object_t;

typedef enum php_pqtxn_isolation {
	PHP_PQTXN_READ_COMMITTED,
	PHP_PQTXN_REPEATABLE_READ,
	PHP_PQTXN_SERIALIZABLE,
} php_pqtxn_isolation_t;

typedef struct php_pqtxn {
	php_pqconn_object_t *conn;
	php_pqtxn_isolation_t isolation;
	unsigned savepoint;
	unsigned open:1;
	unsigned readonly:1;
	unsigned deferrable:1;
} php_pqtxn_t;

typedef struct php_pqtxn_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqtxn_t *intern;
} php_pqtxn_object_t;

typedef struct php_pqcancel {
	PGcancel *cancel;
	php_pqconn_object_t *conn;
} php_pqcancel_t;

typedef struct php_pqcancel_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqcancel_t *intern;
} php_pqcancel_object_t;

typedef struct php_pqevent {
	php_pq_callback_t cb;
	char *type;
	ulong h;
} php_pqevent_t;

typedef struct php_pqevent_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqevent_t *intern;
} php_pqevent_object_t;

typedef struct php_pqlob {
	int lofd;
	Oid loid;
	int stream;
	php_pqtxn_object_t *txn;
} php_pqlob_t;

typedef struct php_pqlob_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqlob_t *intern;
} php_pqlob_object_t;

typedef enum php_pqcopy_direction {
	PHP_PQCOPY_FROM_STDIN,
	PHP_PQCOPY_TO_STDOUT
} php_pqcopy_direction_t;

typedef enum php_pqcopy_status {
	PHP_PQCOPY_FAIL,
	PHP_PQCOPY_CONT,
	PHP_PQCOPY_DONE
} php_pqcopy_status_t;

typedef struct php_pqcopy {
	php_pqcopy_direction_t direction;
	char *expression;
	char *options;
	php_pqconn_object_t *conn;
} php_pqcopy_t;

typedef struct php_pqcopy_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqcopy_t *intern;
} php_pqcopy_object_t;

static HashTable php_pqconn_object_prophandlers;
static HashTable php_pqtypes_object_prophandlers;
static HashTable php_pqres_object_prophandlers;
static HashTable php_pqstm_object_prophandlers;
static HashTable php_pqtxn_object_prophandlers;
static HashTable php_pqcancel_object_prophandlers;
static HashTable php_pqlob_object_prophandlers;
static HashTable php_pqcopy_object_prophandlers;

typedef void (*php_pq_object_prophandler_func_t)(zval *object, void *o, zval *return_value TSRMLS_DC);

typedef struct php_pq_object_prophandler {
	php_pq_object_prophandler_func_t read;
	php_pq_object_prophandler_func_t write;
} php_pq_object_prophandler_t;

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

	switch (PQresultStatus(obj->intern->res)) {
	case PGRES_TUPLES_OK:
	case PGRES_SINGLE_TUPLE:
		if (PQntuples(obj->intern->res) <= iter->index) {
			return FAILURE;
		}
		break;
	default:
		return FAILURE;
	}

	return SUCCESS;
}

static zval *php_pqres_row_to_zval(PGresult *res, unsigned row, php_pqres_fetch_t fetch_type, zval **data_ptr TSRMLS_DC)
{
	zval *data = NULL;
	int c, cols;

	if (data_ptr) {
		data = *data_ptr;
	}
	if (!data) {
		MAKE_STD_ZVAL(data);
		if (PHP_PQRES_FETCH_OBJECT == fetch_type) {
			object_init(data);
		} else {
			array_init(data);
		}
		if (data_ptr) {
			*data_ptr = data;
		}
	}

	if (PQntuples(res) > row) {
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
	}

	return data;
}

static void php_pqres_iterator_current(zend_object_iterator *i, zval ***data_ptr TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;
	php_pqres_object_t *obj = zend_object_store_get_object(iter->zi.data TSRMLS_CC);

	if (!iter->current_val) {
		iter->current_val = php_pqres_row_to_zval(obj->intern->res, iter->index, iter->fetch_type, NULL TSRMLS_CC);
	}
	*data_ptr = &iter->current_val;
}

static int php_pqres_iterator_key(zend_object_iterator *i, char **key_str, uint *key_len, ulong *key_num TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	*key_num = (ulong) iter->index;

	return HASH_KEY_IS_LONG;
}

static void php_pqres_iterator_invalidate(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	if (iter->current_val) {
		zval_ptr_dtor(&iter->current_val);
		iter->current_val = NULL;
	}
}

static void php_pqres_iterator_next(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	php_pqres_iterator_invalidate(i TSRMLS_CC);
	++iter->index;
}

static void php_pqres_iterator_rewind(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	php_pqres_iterator_invalidate(i TSRMLS_CC);
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
	php_pqres_iterator_invalidate
};

static int php_pqres_count_elements(zval *object, long *count TSRMLS_DC)
{
	php_pqres_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);

	if (!obj->intern) {
		return FAILURE;
	} else {
		*count = (long) PQntuples(obj->intern->res);
		return SUCCESS;
	}
}

static STATUS php_pqres_success(PGresult *res TSRMLS_DC)
{
	zval *zexc;

	switch (PQresultStatus(res)) {
	case PGRES_BAD_RESPONSE:
	case PGRES_NONFATAL_ERROR:
	case PGRES_FATAL_ERROR:
		zexc = throw_exce(EX_SQL TSRMLS_CC, "%s", PHP_PQresultErrorMessage(res));
		zend_update_property_string(php_pqexc_domain_class_entry, zexc, ZEND_STRL("sqlstate"), PQresultErrorField(res, PG_DIAG_SQLSTATE) TSRMLS_CC);
		return FAILURE;
	default:
		return SUCCESS;
	}
}

static void php_pq_callback_dtor(php_pq_callback_t *cb) {
	if (cb->fci.size > 0) {
		zend_fcall_info_args_clear(&cb->fci, 1);
		zval_ptr_dtor(&cb->fci.function_name);
		if (cb->fci.object_ptr) {
			zval_ptr_dtor(&cb->fci.object_ptr);
		}
	}
	cb->fci.size = 0;
}

static void php_pq_callback_addref(php_pq_callback_t *cb)
{
	Z_ADDREF_P(cb->fci.function_name);
	if (cb->fci.object_ptr) {
		Z_ADDREF_P(cb->fci.object_ptr);
	}
}

/*
static void php_pqconn_del_eventhandler(php_pqconn_object_t *obj, const char *type_str, size_t type_len, ulong id TSRMLS_DC)
{
	zval **evhs;

	if (SUCCESS == zend_hash_find(&obj->intern->eventhandlers, type_str, type_len + 1, (void *) &evhs)) {
		zend_hash_index_del(Z_ARRVAL_PP(evhs), id);
	}
}
*/

static ulong php_pqconn_add_eventhandler(php_pqconn_object_t *obj, const char *type_str, size_t type_len, php_pq_callback_t *cb TSRMLS_DC)
{
	ulong h;
	HashTable *evhs;

	if (SUCCESS != zend_hash_find(&obj->intern->eventhandlers, type_str, type_len + 1, (void *) &evhs)) {
		HashTable evh;

		zend_hash_init(&evh, 1, NULL, (dtor_func_t) php_pq_callback_dtor, 0);
		zend_hash_add(&obj->intern->eventhandlers, type_str, type_len + 1, (void *) &evh, sizeof(evh), (void *) &evhs);
	}

	php_pq_callback_addref(cb);
	h = zend_hash_next_free_element(evhs);
	zend_hash_index_update(evhs, h, (void *) cb, sizeof(*cb), NULL);

	return h;
}

static void php_pq_object_to_zval(void *o, zval **zv TSRMLS_DC)
{
	php_pq_object_t *obj = o;

	if (!*zv) {
		MAKE_STD_ZVAL(*zv);
	}

	zend_objects_store_add_ref_by_handle(obj->zv.handle TSRMLS_CC);

	(*zv)->type = IS_OBJECT;
	(*zv)->value.obj = obj->zv;
}

static void php_pq_object_to_zval_no_addref(void *o, zval **zv TSRMLS_DC)
{
	php_pq_object_t *obj = o;

	if (!*zv) {
		MAKE_STD_ZVAL(*zv);
	}

	/* no add ref */

	(*zv)->type = IS_OBJECT;
	(*zv)->value.obj = obj->zv;
}

static void php_pq_object_addref(void *o TSRMLS_DC)
{
	php_pq_object_t *obj = o;
	zend_objects_store_add_ref_by_handle(obj->zv.handle TSRMLS_CC);
}

static void php_pq_object_delref(void *o TSRMLS_DC)
{
	php_pq_object_t *obj = o;
	zend_objects_store_del_ref_by_handle_ex(obj->zv.handle, obj->zv.handlers TSRMLS_CC);
}

static void php_pqconn_object_free(void *o TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE conn(#%d) %p\n", obj->zv.handle, obj);
#endif
	if (obj->intern) {
		php_resource_factory_handle_dtor(&obj->intern->factory, obj->intern->conn TSRMLS_CC);
		php_resource_factory_dtor(&obj->intern->factory);
		php_pq_callback_dtor(&obj->intern->onevent);
		zend_hash_destroy(&obj->intern->listeners);
		zend_hash_destroy(&obj->intern->eventhandlers);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

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

static void php_pqres_object_free(void *o TSRMLS_DC)
{
	php_pqres_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE res(#%d) %p\n", obj->zv.handle, obj);
#endif
	if (obj->intern) {
		if (obj->intern->res) {
			PQresultSetInstanceData(obj->intern->res, php_pqconn_event, NULL);
			PQclear(obj->intern->res);
			obj->intern->res = NULL;
		}

		if (obj->intern->iter) {
			php_pqres_iterator_dtor((zend_object_iterator *) obj->intern->iter TSRMLS_CC);
			obj->intern->iter = NULL;
		}

		zend_hash_destroy(&obj->intern->bound);

		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static void php_pqstm_object_free(void *o TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE stm(#%d) %p (conn(#%d): %p)\n", obj->zv.handle, obj, obj->intern->conn->zv.handle, obj->intern->conn);
#endif
	if (obj->intern) {
		char *quoted_name = PQescapeIdentifier(obj->intern->conn->intern->conn, obj->intern->name, strlen(obj->intern->name));

		php_pq_callback_dtor(&obj->intern->conn->intern->onevent);

		if (quoted_name) {
			PGresult *res;
			smart_str cmd = {0};

			smart_str_appends(&cmd, "DEALLOCATE ");
			smart_str_appends(&cmd, quoted_name);
			smart_str_0(&cmd);
			PQfreemem(quoted_name);

			if ((res = PQexec(obj->intern->conn->intern->conn, cmd.c))) {
				PHP_PQclear(res);
			}
			smart_str_free(&cmd);
		}

		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern->name);
		zend_hash_destroy(&obj->intern->bound);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static void php_pqtxn_object_free(void *o TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE txn(#%d) %p (conn(#%d): %p)\n", obj->zv.handle, obj, obj->intern->conn->zv.handle, obj->intern->conn);
#endif
	if (obj->intern) {
		if (obj->intern->open) {
			PGresult *res = PQexec(obj->intern->conn->intern->conn, "ROLLBACK");

			if (res) {
				PHP_PQclear(res);
			}
		}
		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

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

static void php_pqlob_object_free(void *o TSRMLS_DC)
{
	php_pqlob_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE lob(#%d) %p (txn(#%d): %p)\n", obj->zv.handle, obj, obj->intern->txn->zv.handle, obj->intern->txn);
#endif
	if (obj->intern) {
		if (obj->intern->lofd) {
			lo_close(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd);
		}
		/* invalidate the stream */
		if (obj->intern->stream) {
			zend_list_delete(obj->intern->stream);
			obj->intern->stream = 0;
		}
		php_pq_object_delref(obj->intern->txn TSRMLS_CC);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

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

static zend_object_value php_pqconn_create_object_ex(zend_class_entry *ce, php_pqconn_t *intern, php_pqconn_object_t **ptr TSRMLS_DC)
{
	php_pqconn_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqconn_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqconn_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqconn_object_handlers;

	return o->zv;
}

static zend_object_value php_pqtypes_create_object_ex(zend_class_entry *ce, php_pqtypes_t *intern, php_pqtypes_object_t **ptr TSRMLS_DC)
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

static zend_object_value php_pqres_create_object_ex(zend_class_entry *ce, php_pqres_t *intern, php_pqres_object_t **ptr TSRMLS_DC)
{
	php_pqres_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqres_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqres_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqres_object_handlers;

	return o->zv;
}

static zend_object_value php_pqstm_create_object_ex(zend_class_entry *ce, php_pqstm_t *intern, php_pqstm_object_t **ptr TSRMLS_DC)
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

static zend_object_value php_pqtxn_create_object_ex(zend_class_entry *ce, php_pqtxn_t *intern, php_pqtxn_object_t **ptr TSRMLS_DC)
{
	php_pqtxn_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqtxn_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqtxn_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqtxn_object_handlers;

	return o->zv;
}

static zend_object_value php_pqcancel_create_object_ex(zend_class_entry *ce, php_pqcancel_t *intern, php_pqcancel_object_t **ptr TSRMLS_DC)
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

static zend_object_value php_pqlob_create_object_ex(zend_class_entry *ce, php_pqlob_t *intern, php_pqlob_object_t **ptr TSRMLS_DC)
{
	php_pqlob_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqlob_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqlob_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqlob_object_handlers;

	return o->zv;
}

static zend_object_value php_pqcopy_create_object_ex(zend_class_entry *ce, php_pqcopy_t *intern, php_pqcopy_object_t **ptr TSRMLS_DC)
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

static zend_object_value php_pqconn_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqconn_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqtypes_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqtypes_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqres_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqres_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqstm_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqstm_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqtxn_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqtxn_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqcancel_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqcancel_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqlob_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqlob_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqcopy_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqcopy_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static int apply_pi_to_ht(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	zend_property_info *pi = p;
	HashTable *ht = va_arg(argv, HashTable *);
	zval *object = va_arg(argv, zval *);
	php_pq_object_t *obj = va_arg(argv, php_pq_object_t *);
	int addref = va_arg(argv, int);
	zval *property = zend_read_property(obj->zo.ce, object, pi->name, pi->name_length, 0 TSRMLS_CC);

	if (addref) {
		Z_ADDREF_P(property);
	}
	zend_hash_add(ht, pi->name, pi->name_length + 1, (void *) &property, sizeof(zval *), NULL);

	return ZEND_HASH_APPLY_KEEP;
}

static HashTable *php_pq_object_debug_info(zval *object, int *temp TSRMLS_DC)
{
	HashTable *ht;
	php_pq_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);

	*temp = 1;
	ALLOC_HASHTABLE(ht);
	ZEND_INIT_SYMTABLE(ht);

	zend_hash_apply_with_arguments(&obj->zo.ce->properties_info TSRMLS_CC, apply_pi_to_ht, 4, ht, object, obj, 1);

	return ht;
}

static void php_pqconn_object_read_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQstatus(obj->intern->conn));
}

static void php_pqconn_object_read_transaction_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQtransactionStatus(obj->intern->conn));
}

static void php_pqconn_object_read_error_message(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *error = PHP_PQerrorMessage(obj->intern->conn);

	if (error) {
		RETVAL_STRING(error, 1);
	} else {
		RETVAL_NULL();
	}
}

static int apply_notify_listener(void *p, void *arg TSRMLS_DC)
{
	php_pq_callback_t *listener = p;
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

static int apply_notify_listeners(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	HashTable *listeners = p;
	PGnotify *nfy = va_arg(argv, PGnotify *);

	if (0 == fnmatch(key->arKey, nfy->relname, 0)) {
		zend_hash_apply_with_argument(listeners, apply_notify_listener, nfy TSRMLS_CC);
	}

	return ZEND_HASH_APPLY_KEEP;
}

static void php_pqconn_notify_listeners(php_pqconn_object_t *obj TSRMLS_DC)
{
	PGnotify *nfy;

	while ((nfy = PQnotifies(obj->intern->conn))) {
		zend_hash_apply_with_arguments(&obj->intern->listeners TSRMLS_CC, apply_notify_listeners, 1, nfy);
		PQfreemem(nfy);
	}
}

static void php_pqconn_object_read_busy(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(PQisBusy(obj->intern->conn));
}

static void php_pqconn_object_read_encoding(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_STRING(pg_encoding_to_char(PQclientEncoding(obj->intern->conn)), 1);
}

static void php_pqconn_object_write_encoding(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	zval *zenc = value;

	if (Z_TYPE_P(value) != IS_STRING) {
		if (Z_REFCOUNT_P(value) > 1) {
			zval *tmp;
			MAKE_STD_ZVAL(tmp);
			ZVAL_ZVAL(tmp, zenc, 1, 0);
			convert_to_string(tmp);
			zenc = tmp;
		} else {
			convert_to_string_ex(&zenc);
		}
	}

	if (0 > PQsetClientEncoding(obj->intern->conn, Z_STRVAL_P(zenc))) {
		zend_error(E_NOTICE, "Unrecognized encoding '%s'", Z_STRVAL_P(zenc));
	}

	if (zenc != value) {
		zval_ptr_dtor(&zenc);
	}
}

static void php_pqconn_object_read_unbuffered(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->unbuffered);
}

static void php_pqconn_object_write_unbuffered(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	obj->intern->unbuffered = zend_is_true(value);
}

static void php_pqconn_object_read_db(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *db = PQdb(obj->intern->conn);

	if (db) {
		RETVAL_STRING(db, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_user(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *user = PQuser(obj->intern->conn);

	if (user) {
		RETVAL_STRING(user, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_pass(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *pass = PQpass(obj->intern->conn);

	if (pass) {
		RETVAL_STRING(pass, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_host(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *host = PQhost(obj->intern->conn);

	if (host) {
		RETVAL_STRING(host, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_port(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *port = PQport(obj->intern->conn);

	if (port) {
		RETVAL_STRING(port, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_options(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *options = PQoptions(obj->intern->conn);

	if (options) {
		RETVAL_STRING(options, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_event_handlers(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	array_init(return_value);
	zend_hash_copy(Z_ARRVAL_P(return_value), &obj->intern->eventhandlers, (copy_ctor_func_t) zval_add_ref, NULL, sizeof(zval *));
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
		if (key_str) {
			efree(key_str);
		}
	}

	return NULL;
}

static void php_pqres_object_read_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQresultStatus(obj->intern->res));
}

static void php_pqres_object_read_status_message(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_STRING(PQresStatus(PQresultStatus(obj->intern->res))+sizeof("PGRES"), 1);
}

static void php_pqres_object_read_error_message(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;
	char *error = PHP_PQresultErrorMessage(obj->intern->res);

	if (error) {
		RETVAL_STRING(error, 1);
	} else {
		RETVAL_NULL();
	}
}

static void php_pqres_object_read_num_rows(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQntuples(obj->intern->res));
}

static void php_pqres_object_read_num_cols(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQnfields(obj->intern->res));
}

static void php_pqres_object_read_affected_rows(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(atoi(PQcmdTuples(obj->intern->res)));
}

static void php_pqres_object_read_fetch_type(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	if (obj->intern->iter) {
		RETVAL_LONG(obj->intern->iter->fetch_type);
	} else {
		RETVAL_LONG(PHP_PQRES_FETCH_ARRAY);
	}
}

static void php_pqres_object_write_fetch_type(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;
	zval *zfetch_type = value;

	if (Z_TYPE_P(value) != IS_LONG) {
		if (Z_REFCOUNT_P(value) > 1) {
			zval *tmp;
			MAKE_STD_ZVAL(tmp);
			ZVAL_ZVAL(tmp, zfetch_type, 1, 0);
			convert_to_long(tmp);
			zfetch_type = tmp;
		} else {
			convert_to_long_ex(&zfetch_type);
		}
	}

	if (!obj->intern->iter) {
		obj->intern->iter = (php_pqres_iterator_t *) php_pqres_iterator_init(Z_OBJCE_P(object), object, 0 TSRMLS_CC);
		obj->intern->iter->zi.funcs->rewind((zend_object_iterator *) obj->intern->iter TSRMLS_CC);
	}
	obj->intern->iter->fetch_type = Z_LVAL_P(zfetch_type);

	if (zfetch_type != value) {
		zval_ptr_dtor(&zfetch_type);
	}
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

static void php_pqtxn_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static void php_pqtxn_object_read_isolation(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	RETVAL_LONG(obj->intern->isolation);
}

static void php_pqtxn_object_read_readonly(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->readonly);
}

static void php_pqtxn_object_read_deferrable(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->deferrable);
}

static void php_pqtxn_object_write_isolation(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
	php_pqtxn_isolation_t orig = obj->intern->isolation;
	zval *zisolation = value;
	PGresult *res;

	if (Z_TYPE_P(zisolation) != IS_LONG) {
		if (Z_REFCOUNT_P(value) > 1) {
			zval *tmp;
			MAKE_STD_ZVAL(tmp);
			ZVAL_ZVAL(tmp, zisolation, 1, 0);
			convert_to_long(tmp);
			zisolation = tmp;
		} else {
			convert_to_long_ex(&zisolation);
		}
	}

	switch ((obj->intern->isolation = Z_LVAL_P(zisolation))) {
	case PHP_PQTXN_READ_COMMITTED:
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION ISOLATION LEVEL READ COMMITED");
		break;
	case PHP_PQTXN_REPEATABLE_READ:
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
		break;
	case PHP_PQTXN_SERIALIZABLE:
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");
		break;
	default:
		obj->intern->isolation = orig;
		res = NULL;
		break;
	}

	if (zisolation != value) {
		zval_ptr_dtor(&zisolation);
	}

	if (res) {
		php_pqres_success(res TSRMLS_CC);
		PHP_PQclear(res);
	}
}

static void php_pqtxn_object_write_readonly(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
	PGresult *res;

	if ((obj->intern->readonly = zend_is_true(value))) {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION READ ONLY");
	} else {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION READ WRITE");
	}

	if (res) {
		php_pqres_success(res TSRMLS_CC);
		PHP_PQclear(res);
	}
}

static void php_pqtxn_object_write_deferrable(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
	PGresult *res;

	if ((obj->intern->deferrable = zend_is_true(value))) {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION DEFERRABLE");
	} else {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION NOT DEFERRABLE");
	}

	if (res) {
		php_pqres_success(res TSRMLS_CC);
		PHP_PQclear(res);
	}
}

static void php_pqcancel_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqcancel_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static void php_pqlob_object_read_transaction(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqlob_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->txn, &return_value TSRMLS_CC);
}

static void php_pqlob_object_read_oid(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqlob_object_t *obj = o;

	RETVAL_LONG(obj->intern->loid);
}

static void php_pqlob_object_update_stream(zval *this_ptr, php_pqlob_object_t *obj, zval **zstream TSRMLS_DC);

static void php_pqlob_object_read_stream(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqlob_object_t *obj = o;

	if (!obj->intern->stream) {
		zval *zstream;

		php_pqlob_object_update_stream(object, obj, &zstream TSRMLS_CC);
		RETVAL_ZVAL(zstream, 1, 1);
	} else {
		RETVAL_RESOURCE(obj->intern->stream);
		zend_list_addref(obj->intern->stream);
	}
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

static zend_class_entry *ancestor(zend_class_entry *ce) {
	while (ce->parent) {
		ce = ce->parent;
	}
	return ce;
}

static zval *php_pq_object_read_prop(zval *object, zval *member, int type, const zend_literal *key TSRMLS_DC)
{
	php_pq_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;
	zval *return_value;

	if (!obj->intern) {
		zend_error(E_WARNING, "%s not initialized", ancestor(obj->zo.ce)->name);
	} else if ((SUCCESS == zend_hash_find(obj->prophandler, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) && handler->read) {
		if (type == BP_VAR_R) {
			ALLOC_ZVAL(return_value);
			Z_SET_REFCOUNT_P(return_value, 0);
			Z_UNSET_ISREF_P(return_value);

			handler->read(object, obj, return_value TSRMLS_CC);
		} else {
			zend_error(E_ERROR, "Cannot access %s properties by reference or array key/index", ancestor(obj->zo.ce)->name);
			return_value = NULL;
		}
	} else {
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	}

	return return_value;
}

static void php_pq_object_write_prop(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC)
{
	php_pq_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;

	if (SUCCESS == zend_hash_find(obj->prophandler, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
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
	
	if ((CONNECTION_BAD != PQstatus(obj->intern->conn))
	&&	(-1 < (socket = PQsocket(obj->intern->conn)))
	&&	(stream = php_stream_fopen_from_fd(socket, "r+b", NULL))) {
		stream->flags |= PHP_STREAM_FLAG_NO_CLOSE;
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

#ifdef ZTS
#	define TSRMLS_DF(d) TSRMLS_D = (d)->ts
#	define TSRMLS_CF(d) (d)->ts = TSRMLS_C
#else
#	define TSRMLS_DF(d)
#	define TSRMLS_CF(d)
#endif

static int apply_event(void *p, void *a TSRMLS_DC)
{
	php_pq_callback_t *cb = p;
	zval *args = a;
	zval *retval = NULL;

	zend_fcall_info_args(&cb->fci, args TSRMLS_CC);
	zend_fcall_info_call(&cb->fci, &cb->fcc, &retval, NULL TSRMLS_CC);
	if (retval) {
		zval_ptr_dtor(&retval);
	}

	return ZEND_HASH_APPLY_KEEP;
}

static void php_pqconn_event_connreset(PGEventConnReset *event)
{
	php_pqconn_event_data_t *data = PQinstanceData(event->conn, php_pqconn_event);

	if (data) {
		HashTable *evhs;
		TSRMLS_DF(data);

		if (SUCCESS == zend_hash_find(&data->obj->intern->eventhandlers, ZEND_STRS("reset"), (void *) &evhs)) {
			zval *args, *connection = NULL;

			MAKE_STD_ZVAL(args);
			array_init(args);
			php_pq_object_to_zval(data->obj, &connection TSRMLS_CC);
			add_next_index_zval(args, connection);
			zend_hash_apply_with_argument(evhs, apply_event, args TSRMLS_CC);
			zval_ptr_dtor(&args);
		}
	}
}

static void php_pqres_init_instance_data(PGresult *res, php_pqres_object_t **ptr TSRMLS_DC)
{
	php_pqres_object_t *obj;
	php_pqres_t *r = ecalloc(1, sizeof(*r));

	r->res = res;
	ZEND_INIT_SYMTABLE(&r->bound);
	php_pqres_create_object_ex(php_pqres_class_entry, r, &obj TSRMLS_CC);

	PQresultSetInstanceData(res, php_pqconn_event, obj);

	if (ptr) {
		*ptr = obj;
	}
}

static void php_pqconn_event_resultcreate(PGEventResultCreate *event)
{
	php_pqconn_event_data_t *data = PQinstanceData(event->conn, php_pqconn_event);

	if (data) {
		php_pqres_object_t *obj;
		HashTable *evhs;
		TSRMLS_DF(data);

		php_pqres_init_instance_data(event->result, &obj TSRMLS_CC);

		/* event listener */
		if (SUCCESS == zend_hash_find(&data->obj->intern->eventhandlers, ZEND_STRS("result"), (void *) &evhs)) {
			zval *args, *connection = NULL, *res = NULL;

			MAKE_STD_ZVAL(args);
			array_init(args);
			php_pq_object_to_zval(data->obj, &connection TSRMLS_CC);
			add_next_index_zval(args, connection);
			php_pq_object_to_zval(obj, &res TSRMLS_CC);
			add_next_index_zval(args, res);
			zend_hash_apply_with_argument(evhs, apply_event, args TSRMLS_CC);
			zval_ptr_dtor(&args);
		}

		/* async callback */
		if (data->obj->intern->onevent.fci.size > 0) {
			zval *res = NULL;

			php_pq_object_to_zval(obj, &res TSRMLS_CC);
			zend_fcall_info_argn(&data->obj->intern->onevent.fci TSRMLS_CC, 1, &res);
			zend_fcall_info_call(&data->obj->intern->onevent.fci, &data->obj->intern->onevent.fcc, NULL, NULL TSRMLS_CC);
			zval_ptr_dtor(&res);
		}

	}
}

static void php_pqconn_event_resultdestroy(PGEventResultDestroy *event)
{
	php_pqres_object_t *obj = PQresultInstanceData(event->result, php_pqconn_event);

	if (obj) {
		obj->intern->res = NULL;
	}
}

static int php_pqconn_event(PGEventId id, void *e, void *data)
{
	switch (id) {
	case PGEVT_CONNRESET:
		php_pqconn_event_connreset(e);
		break;
	case PGEVT_RESULTCREATE:
		php_pqconn_event_resultcreate(e);
		break;
	case PGEVT_RESULTDESTROY:
		php_pqconn_event_resultdestroy(e);
		break;
	default:
		break;
	}

	return 1;
}

static php_pqconn_event_data_t *php_pqconn_event_data_init(php_pqconn_object_t *obj TSRMLS_DC)
{
	php_pqconn_event_data_t *data = emalloc(sizeof(*data));

	data->obj = obj;
	TSRMLS_CF(data);

	return data;
}

static void php_pqconn_notice_recv(void *p, const PGresult *res)
{
	php_pqconn_event_data_t *data = p;

	if (data) {
		HashTable *evhs;
		TSRMLS_DF(data);

		if (SUCCESS == zend_hash_find(&data->obj->intern->eventhandlers, ZEND_STRS("notice"), (void *) &evhs)) {
			zval *args, *connection = NULL;

			MAKE_STD_ZVAL(args);
			array_init(args);
			php_pq_object_to_zval(data->obj, &connection TSRMLS_CC);
			add_next_index_zval(args, connection);
			add_next_index_string(args, PHP_PQresultErrorMessage(res), 1);
			zend_hash_apply_with_argument(evhs, apply_event, args TSRMLS_CC);
			zval_ptr_dtor(&args);
	}
	}
}

typedef struct php_pqconn_resource_factory_data {
	char *dsn;
	long flags;
} php_pqconn_resource_factory_data_t;

static void *php_pqconn_resource_factory_ctor(void *data, void *init_arg TSRMLS_DC)
{
	php_pqconn_resource_factory_data_t *o = init_arg;
	PGconn *conn = NULL;;

	if (o->flags & PHP_PQCONN_ASYNC) {
		conn = PQconnectStart(o->dsn);
	} else {
		conn = PQconnectdb(o->dsn);
	}

	if (conn) {
		PQregisterEventProc(conn, php_pqconn_event, "ext-pq", NULL);
	}

	return conn;
}

static void php_pqconn_resource_factory_dtor(void *opaque, void *handle TSRMLS_DC)
{
	php_pqconn_event_data_t *evdata = PQinstanceData(handle, php_pqconn_event);

	/* we don't care for anything, except free'ing evdata */
	if (evdata) {
		PQsetInstanceData(handle, php_pqconn_event, NULL);
		memset(evdata, 0, sizeof(*evdata));
		efree(evdata);
	}

	PQfinish(handle);
}

static php_resource_factory_ops_t php_pqconn_resource_factory_ops = {
	php_pqconn_resource_factory_ctor,
	NULL,
	php_pqconn_resource_factory_dtor
};

static void php_pqconn_wakeup(php_persistent_handle_factory_t *f, void **handle TSRMLS_DC)
{
	// FIXME: ping server
}

static int apply_unlisten(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	php_pqconn_object_t *obj = va_arg(argv, php_pqconn_object_t *);
	char *quoted_channel = PQescapeIdentifier(obj->intern->conn, key->arKey, key->nKeyLength - 1);

	if (quoted_channel) {
		PGresult *res;
		char *cmd;

		spprintf(&cmd, 0, "UNLISTEN %s", quoted_channel);
		if ((res = PQexec(obj->intern->conn, cmd))) {
			PHP_PQclear(res);
		}

		efree(cmd);
		PQfreemem(quoted_channel);
	}

	return ZEND_HASH_APPLY_REMOVE;
}

static void php_pqconn_notice_ignore(void *p, const PGresult *res)
{
}

static void php_pqconn_retire(php_persistent_handle_factory_t *f, void **handle TSRMLS_DC)
{
	php_pqconn_event_data_t *evdata = PQinstanceData(*handle, php_pqconn_event);
	PGcancel *cancel;
	PGresult *res;

	/* go away */
	PQsetInstanceData(*handle, php_pqconn_event, NULL);

	/* ignore notices */
	PQsetNoticeReceiver(*handle, php_pqconn_notice_ignore, NULL);

	/* cancel async queries */
	if (PQisBusy(*handle) && (cancel = PQgetCancel(*handle))) {
		char err[256] = {0};

		PQcancel(cancel, err, sizeof(err));
		PQfreeCancel(cancel);
	}
	/* clean up async results */
	while ((res = PQgetResult(*handle))) {
		PHP_PQclear(res);
	}

	/* clean up transaction & session */
	switch (PQtransactionStatus(*handle)) {
	case PQTRANS_IDLE:
		res = PQexec(*handle, "RESET ALL");
		break;
	default:
		res = PQexec(*handle, "ROLLBACK; RESET ALL");
		break;
	}

	if (res) {
		PHP_PQclear(res);
	}

	if (evdata) {
		/* clean up notify listeners */
		zend_hash_apply_with_arguments(&evdata->obj->intern->listeners TSRMLS_CC, apply_unlisten, 1, evdata->obj);

		/* release instance data */
		memset(evdata, 0, sizeof(*evdata));
		efree(evdata);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_construct, 0, 0, 1)
	ZEND_ARG_INFO(0, dsn)
	ZEND_ARG_INFO(0, async)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, __construct) {
	zend_error_handling zeh;
	char *dsn_str = "";
	int dsn_len = 0;
	long flags = 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sl", &dsn_str, &dsn_len, &flags);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			throw_exce(EX_BAD_METHODCALL TSRMLS_CC, "pq\\Connection already initialized");
		} else {
			php_pqconn_event_data_t *evdata =  php_pqconn_event_data_init(obj TSRMLS_CC);
			php_pqconn_resource_factory_data_t rfdata = {dsn_str, flags};

			obj->intern = ecalloc(1, sizeof(*obj->intern));

			zend_hash_init(&obj->intern->listeners, 0, NULL, (dtor_func_t) zend_hash_destroy, 0);
			zend_hash_init(&obj->intern->eventhandlers, 0, NULL, (dtor_func_t) zend_hash_destroy, 0);

			if (flags & PHP_PQCONN_PERSISTENT) {
				php_persistent_handle_factory_t *phf = php_persistent_handle_concede(NULL, ZEND_STRL("pq\\Connection"), dsn_str, dsn_len, php_pqconn_wakeup, php_pqconn_retire TSRMLS_CC);
				php_resource_factory_init(&obj->intern->factory, php_persistent_handle_get_resource_factory_ops(), phf, (void (*)(void*)) php_persistent_handle_abandon);
			} else {
				php_resource_factory_init(&obj->intern->factory, &php_pqconn_resource_factory_ops, NULL, NULL);
			}

			if (flags & PHP_PQCONN_ASYNC) {
				obj->intern->poller = (int (*)(PGconn*)) PQconnectPoll;
			}

			obj->intern->conn = php_resource_factory_handle_ctor(&obj->intern->factory, &rfdata TSRMLS_CC);

			PQsetInstanceData(obj->intern->conn, php_pqconn_event, evdata);
			PQsetNoticeReceiver(obj->intern->conn, php_pqconn_notice_recv, evdata);

			if (SUCCESS != php_pqconn_update_socket(getThis(), obj TSRMLS_CC)) {
				throw_exce(EX_CONNECTION_FAILED TSRMLS_CC, "Connection failed (%s)", PHP_PQerrorMessage(obj->intern->conn));
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_reset, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, reset) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PQreset(obj->intern->conn);

			if (CONNECTION_OK != PQstatus(obj->intern->conn)) {
				throw_exce(EX_CONNECTION_FAILED TSRMLS_CC, "Connection reset failed: (%s)", PHP_PQerrorMessage(obj->intern->conn));
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_reset_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, resetAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			if (!PQresetStart(obj->intern->conn)) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to start connection reset (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				obj->intern->poller = (int (*)(PGconn*)) PQresetPoll;
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

static void php_pqconn_add_listener(php_pqconn_object_t *obj, const char *channel_str, size_t channel_len, php_pq_callback_t *listener TSRMLS_DC)
{
	HashTable ht, *existing_listeners;

	php_pq_callback_addref(listener);

	if (SUCCESS == zend_hash_find(&obj->intern->listeners, channel_str, channel_len + 1, (void *) &existing_listeners)) {
		zend_hash_next_index_insert(existing_listeners, (void *) listener, sizeof(*listener), NULL);
	} else {
		zend_hash_init(&ht, 1, NULL, (dtor_func_t) php_pq_callback_dtor, 0);
		zend_hash_next_index_insert(&ht, (void *) listener, sizeof(*listener), NULL);
		zend_hash_add(&obj->intern->listeners, channel_str, channel_len + 1, (void *) &ht, sizeof(HashTable), NULL);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_listen, 0, 0, 0)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, listen) {
	zend_error_handling zeh;
	char *channel_str = NULL;
	int channel_len = 0;
	php_pq_callback_t listener;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sf", &channel_str, &channel_len, &listener.fci, &listener.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *quoted_channel = PQescapeIdentifier(obj->intern->conn, channel_str, channel_len);

			if (!quoted_channel) {
				throw_exce(EX_ESCAPE TSRMLS_CC, "Failed to escape channel identifier (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				PGresult *res;
				smart_str cmd = {0};

				smart_str_appends(&cmd, "LISTEN ");
				smart_str_appends(&cmd, quoted_channel);
				smart_str_0(&cmd);

				res = PQexec(obj->intern->conn, cmd.c);

				smart_str_free(&cmd);
				PQfreemem(quoted_channel);

				if (!res) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to install listener (%s)", PHP_PQerrorMessage(obj->intern->conn));
				} else {
					if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
						obj->intern->poller = PQconsumeInput;
						php_pqconn_add_listener(obj, channel_str, channel_len, &listener TSRMLS_CC);
					}
					PHP_PQclear(res);
				}

				php_pqconn_notify_listeners(obj TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_listen_async, 0, 0, 0)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, listenAsync) {
	zend_error_handling zeh;
	char *channel_str = NULL;
	int channel_len = 0;
	php_pq_callback_t listener;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sf", &channel_str, &channel_len, &listener.fci, &listener.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *quoted_channel = PQescapeIdentifier(obj->intern->conn, channel_str, channel_len);

			if (!quoted_channel) {
				throw_exce(EX_ESCAPE TSRMLS_CC, "Failed to escape channel identifier (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				smart_str cmd = {0};

				smart_str_appends(&cmd, "LISTEN ");
				smart_str_appends(&cmd, quoted_channel);
				smart_str_0(&cmd);

				if (!PQsendQuery(obj->intern->conn, cmd.c)) {
					throw_exce(EX_IO TSRMLS_CC, "Failed to install listener (%s)", PHP_PQerrorMessage(obj->intern->conn));
				} else {
					obj->intern->poller = PQconsumeInput;
					php_pqconn_add_listener(obj, channel_str, channel_len, &listener TSRMLS_CC);
				}

				smart_str_free(&cmd);
				PQfreemem(quoted_channel);
				php_pqconn_notify_listeners(obj TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_notify, 0, 0, 2)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, notify) {
	zend_error_handling zeh;
	char *channel_str, *message_str;
	int channel_len, message_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &channel_str, &channel_len, &message_str, &message_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PGresult *res;
			char *params[2] = {channel_str, message_str};

			res = PQexecParams(obj->intern->conn, "select pg_notify($1, $2)", 2, NULL, (const char *const*) params, NULL, NULL, 0);

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to notify listeners (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				php_pqres_success(res TSRMLS_CC);
				PHP_PQclear(res);
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_notify_async, 0, 0, 2)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, notifyAsync) {
	zend_error_handling zeh;
	char *channel_str, *message_str;
	int channel_len, message_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &channel_str, &channel_len, &message_str, &message_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *params[2] = {channel_str, message_str};

			if (!PQsendQueryParams(obj->intern->conn, "select pg_notify($1, $2)", 2, NULL, (const char *const*) params, NULL, NULL, 0)) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to notify listeners (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				obj->intern->poller = PQconsumeInput;
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_poll, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, poll) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else if (!obj->intern->poller) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "No asynchronous operation active");
		} else {
			if (obj->intern->poller == PQconsumeInput) {
				RETVAL_LONG(obj->intern->poller(obj->intern->conn) * PGRES_POLLING_OK);
			} else {
				RETVAL_LONG(obj->intern->poller(obj->intern->conn));
			}
			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, exec) {
	zend_error_handling zeh;
	char *query_str;
	int query_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &query_str, &query_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PGresult *res = PQexec(obj->intern->conn, query_str);

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
				php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), &return_value TSRMLS_CC);
			} else {
				PHP_PQclear(res);
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_get_result, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, getResult) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PGresult *res = PQgetResult(obj->intern->conn);

			if (!res) {
				RETVAL_NULL();
			} else {
				php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), &return_value TSRMLS_CC);
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec_async, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, execAsync) {
	zend_error_handling zeh;
	php_pq_callback_t resolver = {{0}};
	char *query_str;
	int query_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f", &query_str, &query_len, &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else if (!PQsendQuery(obj->intern->conn, query_str)) {
			throw_exce(EX_IO TSRMLS_CC, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
		} else if (obj->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn)) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn));
		} else {
			obj->intern->poller = PQconsumeInput;
			php_pq_callback_dtor(&obj->intern->onevent);
			if (resolver.fci.size > 0) {
				obj->intern->onevent = resolver;
				php_pq_callback_addref(&obj->intern->onevent);
			}
			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
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

static int php_pq_types_to_array(HashTable *ht, Oid **types TSRMLS_DC)
{
	int count = zend_hash_num_elements(ht);
	
	*types = NULL;

	if (count) {
		Oid *tmp;

		/* +1 for when less types than params are specified */
		*types = tmp = ecalloc(count + 1, sizeof(**types));
		zend_hash_apply_with_argument(ht, apply_to_oid, &tmp TSRMLS_CC);
	}
	
	return count;
}

static int php_pq_params_to_array(HashTable *ht, char ***params, HashTable *zdtor TSRMLS_DC)
{
	int count = zend_hash_num_elements(ht);
	
	*params = NULL;

	if (count) {
		char **tmp;

		*params = tmp = ecalloc(count, sizeof(char *));
		zend_hash_apply_with_arguments(ht TSRMLS_CC, apply_to_param, 2, &tmp, zdtor);
	}
	
	return count;
}
/*
static Oid *php_pq_ntypes_to_array(zend_bool fill, int argc, ...)
{
	int i;
	Oid *oids = ecalloc(argc + 1, sizeof(*oids));
	va_list argv;

	va_start(argv, argc);
	for (i = 0; i < argc; ++i) {
		if (!fill || !i) {
			oids[i] = va_arg(argv, Oid);
		} else {
			oids[i] = oids[0];
		}
	}
	va_end(argv);

	return oids;
}
*/
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
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa/|a/!", &query_str, &query_len, &zparams, &ztypes);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PGresult *res;
			int count;
			Oid *types = NULL;
			char **params = NULL;
			HashTable zdtor;

			ZEND_INIT_SYMTABLE(&zdtor);
			count = php_pq_params_to_array(Z_ARRVAL_P(zparams), &params, &zdtor TSRMLS_CC);

			if (ztypes) {
				php_pq_types_to_array(Z_ARRVAL_P(ztypes), &types TSRMLS_CC);
			}

			res = PQexecParams(obj->intern->conn, query_str, count, types, (const char *const*) params, NULL, NULL, 0);

			zend_hash_destroy(&zdtor);
			if (types) {
				efree(types);
			}
			if (params) {
				efree(params);
			}

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), &return_value TSRMLS_CC);
				} else {
					PHP_PQclear(res);
				}

				php_pqconn_notify_listeners(obj TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec_params_async, 0, 0, 2)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, params, 0)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, execParamsAsync) {
	zend_error_handling zeh;
	php_pq_callback_t resolver = {{0}};
	char *query_str;
	int query_len;
	zval *zparams;
	zval *ztypes = NULL;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa/|a/!f", &query_str, &query_len, &zparams, &ztypes, &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			int count;
			Oid *types = NULL;
			char **params = NULL;
			HashTable zdtor;

			ZEND_INIT_SYMTABLE(&zdtor);
			count = php_pq_params_to_array(Z_ARRVAL_P(zparams), &params, &zdtor TSRMLS_CC);

			if (ztypes) {
				php_pq_types_to_array(Z_ARRVAL_P(ztypes), &types TSRMLS_CC);
			}

			if (!PQsendQueryParams(obj->intern->conn, query_str, count, types, (const char *const*) params, NULL, NULL, 0)) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else if (obj->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn)) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				obj->intern->poller = PQconsumeInput;
				php_pq_callback_dtor(&obj->intern->onevent);
				if (resolver.fci.size > 0) {
					obj->intern->onevent = resolver;
					php_pq_callback_addref(&obj->intern->onevent);
				}
				php_pqconn_notify_listeners(obj TSRMLS_CC);
			}

			zend_hash_destroy(&zdtor);
			if (types) {
				efree(types);
			}
			if (params) {
				efree(params);
			}
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static STATUS php_pqconn_prepare(zval *object, php_pqconn_object_t *obj, const char *name, const char *query, HashTable *typest TSRMLS_DC)
{
	Oid *types = NULL;
	int count = 0;
	PGresult *res;
	STATUS rv;

	if (!obj) {
		obj = zend_object_store_get_object(object TSRMLS_CC);
	}

	if (typest) {
		count = zend_hash_num_elements(typest);
		php_pq_types_to_array(typest, &types TSRMLS_CC);
	}
	
	res = PQprepare(obj->intern->conn, name, query, count, types);

	if (types) {
		efree(types);
	}

	if (!res) {
		rv = FAILURE;
		throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to prepare statement (%s)", PHP_PQerrorMessage(obj->intern->conn));
	} else {
		rv = php_pqres_success(res TSRMLS_CC);
		PHP_PQclear(res);
		php_pqconn_notify_listeners(obj TSRMLS_CC);
	}
	
	return rv;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_prepare, 0, 0, 2)
	ZEND_ARG_INFO(0, type)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, prepare) {
	zend_error_handling zeh;
	zval *ztypes = NULL;
	char *name_str, *query_str;
	int name_len, *query_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|a/!", &name_str, &name_len, &query_str, &query_len, &ztypes);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else if (SUCCESS == php_pqconn_prepare(getThis(), obj, name_str, query_str, ztypes ? Z_ARRVAL_P(ztypes) : NULL TSRMLS_CC)) {
			php_pqstm_t *stm = ecalloc(1, sizeof(*stm));

			php_pq_object_addref(obj TSRMLS_CC);
			stm->conn = obj;
			stm->name = estrdup(name_str);
			ZEND_INIT_SYMTABLE(&stm->bound);

			return_value->type = IS_OBJECT;
			return_value->value.obj = php_pqstm_create_object_ex(php_pqstm_class_entry, stm, NULL TSRMLS_CC);
		}
	}
}

static STATUS php_pqconn_prepare_async(zval *object, php_pqconn_object_t *obj, const char *name, const char *query, HashTable *typest TSRMLS_DC)
{
	STATUS rv;
	int count;
	Oid *types = NULL;
	
	if (!obj) {
		obj = zend_object_store_get_object(object TSRMLS_CC);
	}

	if (typest) {
		count = php_pq_types_to_array(typest, &types TSRMLS_CC);
	}
	
	if (!PQsendPrepare(obj->intern->conn, name, query, count, types)) {
		rv = FAILURE;
		throw_exce(EX_IO TSRMLS_CC, "Failed to prepare statement (%s)", PHP_PQerrorMessage(obj->intern->conn));
	} else if (obj->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn)) {
		rv = FAILURE;
		throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn));
	} else {
		rv = SUCCESS;
		obj->intern->poller = PQconsumeInput;
		php_pqconn_notify_listeners(obj TSRMLS_CC);
	}
	
	if (types) {
		efree(types);
	}
	
	return rv;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_prepare_async, 0, 0, 2)
ZEND_ARG_INFO(0, type)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, prepareAsync) {
	zend_error_handling zeh;
	zval *ztypes = NULL;
	char *name_str, *query_str;
	int name_len, *query_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|a/!", &name_str, &name_len, &query_str, &query_len, &ztypes);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else if (SUCCESS == php_pqconn_prepare_async(getThis(), obj, name_str, query_str, ztypes ? Z_ARRVAL_P(ztypes) : NULL TSRMLS_CC)) {
			php_pqstm_t *stm = ecalloc(1, sizeof(*stm));

			php_pq_object_addref(obj TSRMLS_CC);
			stm->conn = obj;
			stm->name = estrdup(name_str);
			ZEND_INIT_SYMTABLE(&stm->bound);

			return_value->type = IS_OBJECT;
			return_value->value.obj = php_pqstm_create_object_ex(php_pqstm_class_entry, stm, NULL TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_quote, 0, 0, 1)
	ZEND_ARG_INFO(0, string)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, quote) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *quoted = PQescapeLiteral(obj->intern->conn, str, len);

			if (!quoted) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to quote string (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRING(quoted, 1);
				PQfreemem(quoted);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_quote_name, 0, 0, 1)
	ZEND_ARG_INFO(0, type)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, quoteName) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *quoted = PQescapeIdentifier(obj->intern->conn, str, len);

			if (!quoted) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to quote name (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRING(quoted, 1);
				PQfreemem(quoted);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_escape_bytea, 0, 0, 1)
	ZEND_ARG_INFO(0, bytea)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, escapeBytea) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			size_t escaped_len;
			char *escaped_str = (char *) PQescapeByteaConn(obj->intern->conn, (unsigned char *) str, len, &escaped_len);

			if (!escaped_str) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to escape bytea (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRINGL(escaped_str, escaped_len - 1, 1);
				PQfreemem(escaped_str);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_unescape_bytea, 0, 0, 1)
	ZEND_ARG_INFO(0, bytea)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, unescapeBytea) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			size_t unescaped_len;
			char *unescaped_str = (char *) PQunescapeBytea((unsigned char *)str, &unescaped_len);

			if (!unescaped_str) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to unescape bytea (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRINGL(unescaped_str, unescaped_len, 1);
				PQfreemem(unescaped_str);
			}
		}
	}
}

static const char *isolation_level(long *isolation) {
	switch (*isolation) {
	case PHP_PQTXN_SERIALIZABLE:
		return "SERIALIZABLE";
	case PHP_PQTXN_REPEATABLE_READ:
		return "REPEATABLE READ";
	default:
		*isolation = PHP_PQTXN_READ_COMMITTED;
		/* no break */
	case PHP_PQTXN_READ_COMMITTED:
		return "READ COMMITTED";
	}
}

static STATUS php_pqconn_start_transaction(zval *zconn, php_pqconn_object_t *conn_obj, long isolation, zend_bool readonly, zend_bool deferrable TSRMLS_DC)
{
	STATUS rv = FAILURE;

	if (!conn_obj) {
		conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);
	}

	if (!conn_obj->intern) {
		throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
	} else {
		PGresult *res;
		smart_str cmd = {0};
		const char *il = isolation_level(&isolation);

		smart_str_appends(&cmd, "START TRANSACTION ISOLATION LEVEL ");
		smart_str_appends(&cmd, il);
		smart_str_appends(&cmd, ", READ ");
		smart_str_appends(&cmd, readonly ? "ONLY" : "WRITE");
		smart_str_appends(&cmd, ",");
		smart_str_appends(&cmd, deferrable ? "" : " NOT");
		smart_str_appends(&cmd, " DEFERRABLE");
		smart_str_0(&cmd);

		res = PQexec(conn_obj->intern->conn, cmd.c);

		if (!res) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to start transaction (%s)", PHP_PQerrorMessage(conn_obj->intern->conn));
		} else {
			rv = php_pqres_success(res TSRMLS_CC);
			PHP_PQclear(res);
			php_pqconn_notify_listeners(conn_obj TSRMLS_CC);
		}

		smart_str_free(&cmd);
	}

	return rv;
}

static STATUS php_pqconn_start_transaction_async(zval *zconn, php_pqconn_object_t *conn_obj, long isolation, zend_bool readonly, zend_bool deferrable TSRMLS_DC)
{
	STATUS rv = FAILURE;

	if (!conn_obj) {
		conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);
	}

	if (!conn_obj->intern) {
		throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
	} else {
		smart_str cmd = {0};
		const char *il = isolation_level(&isolation);

		smart_str_appends(&cmd, "START TRANSACTION ISOLATION LEVEL ");
		smart_str_appends(&cmd, il);
		smart_str_appends(&cmd, ", READ ");
		smart_str_appends(&cmd, readonly ? "ONLY" : "WRITE");
		smart_str_appends(&cmd, ",");
		smart_str_appends(&cmd, deferrable ? "" : "NOT ");
		smart_str_appends(&cmd, " DEFERRABLE");
		smart_str_0(&cmd);

		if (!PQsendQuery(conn_obj->intern->conn, cmd.c)) {
			throw_exce(EX_IO TSRMLS_CC, "Failed to start transaction (%s)", PHP_PQerrorMessage(conn_obj->intern->conn));
		} else {
			rv = SUCCESS;
			conn_obj->intern->poller = PQconsumeInput;
			php_pqconn_notify_listeners(conn_obj TSRMLS_CC);
		}

		smart_str_free(&cmd);
	}

	return rv;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_start_transaction, 0, 0, 0)
	ZEND_ARG_INFO(0, isolation)
	ZEND_ARG_INFO(0, readonly)
	ZEND_ARG_INFO(0, deferrable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, startTransaction) {
	zend_error_handling zeh;
	long isolation = PHP_PQTXN_READ_COMMITTED;
	zend_bool readonly = 0, deferrable = 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lbb", &isolation, &readonly, &deferrable);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		rv = php_pqconn_start_transaction(getThis(), obj, isolation, readonly, deferrable TSRMLS_CC);

		if (SUCCESS == rv) {
			php_pqtxn_t *txn = ecalloc(1, sizeof(*txn));

			php_pq_object_addref(obj TSRMLS_CC);
			txn->conn = obj;
			txn->open = 1;
			txn->isolation = isolation;
			txn->readonly = readonly;
			txn->deferrable = deferrable;

			return_value->type = IS_OBJECT;
			return_value->value.obj = php_pqtxn_create_object_ex(php_pqtxn_class_entry, txn, NULL TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_start_transaction_async, 0, 0, 0)
	ZEND_ARG_INFO(0, isolation)
	ZEND_ARG_INFO(0, readonly)
	ZEND_ARG_INFO(0, deferrable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, startTransactionAsync) {
	zend_error_handling zeh;
	long isolation = PHP_PQTXN_READ_COMMITTED;
	zend_bool readonly = 0, deferrable = 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lbb", &isolation, &readonly, &deferrable);
	zend_restore_error_handling(&zeh TSRMLS_CC);
	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		rv = php_pqconn_start_transaction_async(getThis(), obj, isolation, readonly, deferrable TSRMLS_CC);

		if (SUCCESS == rv) {
			php_pqtxn_t *txn = ecalloc(1, sizeof(*txn));

			php_pq_object_addref(obj TSRMLS_CC);
			txn->conn = obj;
			txn->isolation = isolation;
			txn->readonly = readonly;
			txn->deferrable = deferrable;

			return_value->type = IS_OBJECT;
			return_value->value.obj = php_pqtxn_create_object_ex(php_pqtxn_class_entry, txn, NULL TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_trace, 0, 0, 0)
	ZEND_ARG_INFO(0, stdio_stream)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, trace) {
	zval *zstream = NULL;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|r!", &zstream)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			if (!zstream) {
				PQuntrace(obj->intern->conn);
				RETVAL_TRUE;
			} else {
				FILE *fp;
				php_stream *stream = NULL;

				php_stream_from_zval(stream, &zstream);

				if (SUCCESS != php_stream_cast(stream, PHP_STREAM_AS_STDIO, (void *) &fp, REPORT_ERRORS)) {
					RETVAL_FALSE;
				} else {
					stream->flags |= PHP_STREAM_FLAG_NO_CLOSE;
					PQtrace(obj->intern->conn, fp);
					RETVAL_TRUE;
				}
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_on, 0, 0, 2)
	ZEND_ARG_INFO(0, type)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, on) {
	zend_error_handling zeh;
	char *type_str;
	int type_len;
	php_pq_callback_t cb;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sf", &type_str, &type_len, &cb.fci, &cb.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

			RETVAL_LONG(php_pqconn_add_eventhandler(obj, type_str, type_len, &cb TSRMLS_CC));
		}
	}
}

static zend_function_entry php_pqconn_methods[] = {
	PHP_ME(pqconn, __construct, ai_pqconn_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqconn, reset, ai_pqconn_reset, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, resetAsync, ai_pqconn_reset_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, poll, ai_pqconn_poll, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, exec, ai_pqconn_exec, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, execAsync, ai_pqconn_exec_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, execParams, ai_pqconn_exec_params, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, execParamsAsync, ai_pqconn_exec_params_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, prepare, ai_pqconn_prepare, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, prepareAsync, ai_pqconn_prepare_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, listen, ai_pqconn_listen, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, listenAsync, ai_pqconn_listen_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, notify, ai_pqconn_notify, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, notifyAsync, ai_pqconn_notify_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, getResult, ai_pqconn_get_result, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, quote, ai_pqconn_quote, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, quoteName, ai_pqconn_quote_name, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, escapeBytea, ai_pqconn_escape_bytea, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, unescapeBytea, ai_pqconn_unescape_bytea, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, startTransaction, ai_pqconn_start_transaction, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, startTransactionAsync, ai_pqconn_start_transaction_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, trace, ai_pqconn_trace, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, on, ai_pqconn_on, ZEND_ACC_PUBLIC)
	{0}
};

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
#define PHP_PQ_OID_TEXT 25

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
				int i, count;
				Oid *oids;
				char **params = NULL;
				HashTable zdtor;
				smart_str str = {0};

				smart_str_appends(&str, PHP_PQ_TYPES_QUERY " and nspname in(");
				zend_hash_init(&zdtor, 0, NULL, ZVAL_PTR_DTOR, 0);
				count = php_pq_params_to_array(nsp, &params, &zdtor TSRMLS_CC);
				oids = ecalloc(count + 1, sizeof(*oids));
				for (i = 0; i < count; ++i) {
					oids[i] = PHP_PQ_OID_TEXT;
					if (i) {
						smart_str_appendc(&str, ',');
					}
					smart_str_appendc(&str, '$');
					smart_str_append_unsigned(&str, i+1);
				}
				smart_str_appendc(&str, ')');
				smart_str_0(&str);

				res = PQexecParams(obj->intern->conn->intern->conn, str.c, count, oids, (const char *const*) params, NULL, NULL, 0);

				smart_str_free(&str);
				efree(oids);
				efree(params);
				zend_hash_destroy(&zdtor);
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
						zend_hash_add(&obj->intern->types, name, strlen(name) + 1, (void *) &row, sizeof(zval *), NULL);
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

static STATUS php_pqres_iteration(zval *this_ptr, php_pqres_object_t *obj, php_pqres_fetch_t fetch_type, zval ***row TSRMLS_DC)
{
	STATUS rv;
	php_pqres_fetch_t orig_fetch;

	if (!obj) {
	 obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	}

	if (obj->intern->iter) {
		obj->intern->iter->zi.funcs->move_forward((zend_object_iterator *) obj->intern->iter TSRMLS_CC);
	} else {
		obj->intern->iter = (php_pqres_iterator_t *) php_pqres_iterator_init(Z_OBJCE_P(getThis()), getThis(), 0 TSRMLS_CC);
		obj->intern->iter->zi.funcs->rewind((zend_object_iterator *) obj->intern->iter TSRMLS_CC);
	}
	orig_fetch = obj->intern->iter->fetch_type;
	obj->intern->iter->fetch_type = fetch_type;
	if (SUCCESS == (rv = obj->intern->iter->zi.funcs->valid((zend_object_iterator *) obj->intern->iter TSRMLS_CC))) {
		obj->intern->iter->zi.funcs->get_current_data((zend_object_iterator *) obj->intern->iter, row TSRMLS_CC);
	}
	obj->intern->iter->fetch_type = orig_fetch;

	return rv;
}

typedef struct php_pqres_col {
	char *name;
	int num;
} php_pqres_col_t;

static STATUS column_nn(php_pqres_object_t *obj, zval *zcol, php_pqres_col_t *col TSRMLS_DC)
{
	long index = -1;
	char *name = NULL;

	switch (Z_TYPE_P(zcol)) {
	default:
		convert_to_string(zcol);
		/* no break */

	case IS_STRING:
		if (!is_numeric_string(Z_STRVAL_P(zcol), Z_STRLEN_P(zcol), &index, NULL, 0)) {
			name = Z_STRVAL_P(zcol);
		}
		break;

	case IS_LONG:
		index = Z_LVAL_P(zcol);
		break;
	}

	if (name) {
		col->name = name;
		col->num = PQfnumber(obj->intern->res, name);
	} else {
		col->name = PQfname(obj->intern->res, index);
		col->num = index;
	}

	if (!col->name) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to find column at index %ld", index);
		return FAILURE;
	}
	if (col->num == -1) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to find column with name '%s'", name);
		return FAILURE;
	}
	return SUCCESS;
}

static int compare_index(const void *lptr, const void *rptr TSRMLS_DC)
{
	const Bucket *l = *(const Bucket **) lptr;
	const Bucket *r = *(const Bucket **) rptr;

	if (l->h < r->h) {
		return -1;
	}
	if (l->h > r->h) {
		return 1;
	}
	return 0;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_bind, 0, 0, 2)
	ZEND_ARG_INFO(0, col)
	ZEND_ARG_INFO(1, ref)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, bind) {
	zval *zcol, *zref;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z/z", &zcol, &zref)) {
		php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Result not initialized");
		} else {
			php_pqres_col_t col;

			if (SUCCESS != column_nn(obj, zcol, &col TSRMLS_CC)) {
				RETVAL_FALSE;
			} else {
				Z_ADDREF_P(zref);

				if (SUCCESS != zend_hash_index_update(&obj->intern->bound, col.num, (void *) &zref, sizeof(zval *), NULL)) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to bind column %s@%d", col.name, col.num);
					RETVAL_FALSE;
				} else {
					zend_hash_sort(&obj->intern->bound, zend_qsort, compare_index, 0 TSRMLS_CC);
					RETVAL_TRUE;
				}
			}
		}
	}
}

static int apply_bound(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	zval **zvalue, **zbound = p;
	zval **zrow = va_arg(argv, zval **);
	STATUS *rv = va_arg(argv, STATUS *);

	if (SUCCESS != zend_hash_index_find(Z_ARRVAL_PP(zrow), key->h, (void *) &zvalue)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to find column ad index %lu", key->h);
		*rv = FAILURE;
		return ZEND_HASH_APPLY_STOP;
	} else {
		zval_dtor(*zbound);
		ZVAL_COPY_VALUE(*zbound, *zvalue);
		ZVAL_NULL(*zvalue);
		zval_ptr_dtor(zvalue);
		Z_ADDREF_P(*zbound);
		*zvalue = *zbound;
		*rv = SUCCESS;
		return ZEND_HASH_APPLY_KEEP;
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_bound, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchBound) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Result not initialized");
		} else {
			zval **row = NULL;

			if (SUCCESS == php_pqres_iteration(getThis(), obj, PHP_PQRES_FETCH_ARRAY, &row TSRMLS_CC) && row) {
				zend_replace_error_handling(EH_THROW, exce(EX_RUNTIME), &zeh TSRMLS_CC);
				zend_hash_apply_with_arguments(&obj->intern->bound TSRMLS_CC, apply_bound, 2, row, &rv);
				zend_restore_error_handling(&zeh TSRMLS_CC);

				if (SUCCESS != rv) {
					zval_ptr_dtor(row);
				} else {
					RETVAL_ZVAL(*row, 1, 0);
				}
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_row, 0, 0, 0)
	ZEND_ARG_INFO(0, fetch_type)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchRow) {
	zend_error_handling zeh;
	php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	long fetch_type = -1;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &fetch_type);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Result not initialized");
		} else {
			zval **row = NULL;

			if (fetch_type == -1) {
				 fetch_type = obj->intern->iter ? obj->intern->iter->fetch_type : PHP_PQRES_FETCH_ARRAY;
			}

			zend_replace_error_handling(EH_THROW, exce(EX_RUNTIME), &zeh TSRMLS_CC);
			php_pqres_iteration(getThis(), obj, fetch_type, &row TSRMLS_CC);
			zend_restore_error_handling(&zeh TSRMLS_CC);

			if (row) {
				RETVAL_ZVAL(*row, 1, 0);
			}
		}
	}
}

static zval **column_at(zval *row, int col TSRMLS_DC)
{
	zval **data = NULL;
	HashTable *ht = HASH_OF(row);
	int count = zend_hash_num_elements(ht);

	if (col >= count) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Column index %d exceeds column count %d", col, count);
	} else {
		zend_hash_internal_pointer_reset(ht);
		while (col-- > 0) {
			zend_hash_move_forward(ht);
		}
		zend_hash_get_current_data(ht, (void *) &data);
	}
	return data;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_col, 0, 0, 0)
	ZEND_ARG_INFO(0, col_num)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchCol) {
	zend_error_handling zeh;
	long fetch_col = 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &fetch_col);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Result not initialized");
		} else {
			zval **row = NULL;

			zend_replace_error_handling(EH_THROW, exce(EX_RUNTIME), &zeh TSRMLS_CC);
			php_pqres_iteration(getThis(), obj, obj->intern->iter ? obj->intern->iter->fetch_type : 0, &row TSRMLS_CC);
			if (row) {
				zval **col = column_at(*row, fetch_col TSRMLS_CC);

				if (col) {
					RETVAL_ZVAL(*col, 1, 0);
				}
			}
			zend_restore_error_handling(&zeh TSRMLS_CC);
		}
	}
}

static int apply_to_col(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	zval **c = p;
	php_pqres_object_t *obj = va_arg(argv, php_pqres_object_t *);
	php_pqres_col_t *col, **cols = va_arg(argv, php_pqres_col_t **);
	STATUS *rv = va_arg(argv, STATUS *);

	col = *cols;

	if (SUCCESS != column_nn(obj, *c, col TSRMLS_CC)) {
		*rv = FAILURE;
		return ZEND_HASH_APPLY_STOP;
	} else {
		*rv = SUCCESS;
		++*cols;
		return ZEND_HASH_APPLY_KEEP;
	}
}

static php_pqres_col_t *php_pqres_convert_to_cols(php_pqres_object_t *obj, HashTable *ht TSRMLS_DC)
{
	php_pqres_col_t *tmp, *cols = ecalloc(zend_hash_num_elements(ht), sizeof(*cols));
	STATUS rv = SUCCESS;

	tmp = cols;
	zend_hash_apply_with_arguments(ht TSRMLS_CC, apply_to_col, 2, obj, &tmp, &rv);

	if (SUCCESS == rv) {
		return cols;
	} else {
		efree(cols);
		return NULL;
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_map, 0, 0, 0)
	ZEND_ARG_INFO(0, keys)
	ZEND_ARG_INFO(0, vals)
	ZEND_ARG_INFO(0, fetch_type)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, map) {
	zend_error_handling zeh;
	zval *zkeys = 0, *zvals = 0;
	long fetch_type = -1;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|z/!z/!l", &zkeys, &zvals, &fetch_type);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Result not initialized");
		} else {
			int ks = 0, vs = 0;
			php_pqres_col_t def = {PQfname(obj->intern->res, 0), 0}, *keys = NULL, *vals = NULL;

			if (zkeys) {
				convert_to_array(zkeys);

				if ((ks = zend_hash_num_elements(Z_ARRVAL_P(zkeys)))) {
					keys = php_pqres_convert_to_cols(obj, Z_ARRVAL_P(zkeys) TSRMLS_CC);
				} else {
					ks = 1;
					keys = &def;
				}
			} else {
				ks = 1;
				keys = &def;
			}
			if (zvals) {
				convert_to_array(zvals);

				if ((vs = zend_hash_num_elements(Z_ARRVAL_P(zvals)))) {
					vals = php_pqres_convert_to_cols(obj, Z_ARRVAL_P(zvals) TSRMLS_CC);
				}
			}

			if (fetch_type == -1) {
				fetch_type = obj->intern->iter ? obj->intern->iter->fetch_type : PHP_PQRES_FETCH_ARRAY;
			}

			if (keys) {
				int rows, r;
				zval **cur;

				switch (fetch_type) {
				case PHP_PQRES_FETCH_ARRAY:
				case PHP_PQRES_FETCH_ASSOC:
					array_init(return_value);
					break;
				case PHP_PQRES_FETCH_OBJECT:
					object_init(return_value);
					break;
				}
				for (r = 0, rows = PQntuples(obj->intern->res); r < rows; ++r) {
					int k, v;

					cur = &return_value;
					for (k = 0; k < ks; ++k) {
						char *key = PQgetvalue(obj->intern->res, r, keys[k].num);
						int len = PQgetlength(obj->intern->res, r, keys[k].num);

						if (SUCCESS != zend_symtable_find(HASH_OF(*cur), key, len + 1, (void *) &cur)) {
							zval *tmp;

							MAKE_STD_ZVAL(tmp);
							switch (fetch_type) {
							case PHP_PQRES_FETCH_ARRAY:
							case PHP_PQRES_FETCH_ASSOC:
								array_init(tmp);
								break;
							case PHP_PQRES_FETCH_OBJECT:
								object_init(tmp);
								break;
							}
							if (SUCCESS != zend_symtable_update(HASH_OF(*cur), key, len + 1, (void *) &tmp, sizeof(zval *), (void *) &cur)) {
								throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to create map");
								goto err;
							}
						}
					}
					if (vals && vs) {
						for (v = 0; v < vs; ++v) {
							char *val = PQgetvalue(obj->intern->res, r, vals[v].num);
							int len = PQgetlength(obj->intern->res, r, vals[v].num);

							switch (fetch_type) {
							case PHP_PQRES_FETCH_ARRAY:
								add_index_stringl(*cur, vals[v].num, val, len, 1);
								break;
							case PHP_PQRES_FETCH_ASSOC:
								add_assoc_stringl(*cur, vals[v].name, val, len, 1);
								break;
							case PHP_PQRES_FETCH_OBJECT:
								add_property_stringl(*cur, vals[v].name, val, len, 1);
								break;
							}
						}
					} else {
						php_pqres_row_to_zval(obj->intern->res, r, fetch_type, cur TSRMLS_CC);
					}
				}
			}

			err:
			if (keys && keys != &def) {
				efree(keys);
			}
			if (vals) {
				efree(vals);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_all, 0, 0, 0)
	ZEND_ARG_INFO(0, fetch_type)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchAll) {
	zend_error_handling zeh;
	long fetch_type = -1;
	STATUS rv;
	
	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &fetch_type);
	zend_restore_error_handling(&zeh TSRMLS_CC);
	
	if (SUCCESS == rv) {
		php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Result not initialized");
		} else {
			int r, rows = PQntuples(obj->intern->res);

			if (fetch_type == -1) {
				 fetch_type = obj->intern->iter ? obj->intern->iter->fetch_type : PHP_PQRES_FETCH_ARRAY;
			}

			array_init(return_value);
			for (r = 0; r < rows; ++r) {
				add_next_index_zval(return_value, php_pqres_row_to_zval(obj->intern->res, r, fetch_type, NULL TSRMLS_CC));
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_count, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, count) {
	if (SUCCESS == zend_parse_parameters_none()) {
		long count;

		if (SUCCESS != php_pqres_count_elements(getThis(), &count TSRMLS_CC)) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Result not initialized");
		} else {
			RETVAL_LONG(count);
		}
	}
}

static zend_function_entry php_pqres_methods[] = {
	PHP_ME(pqres, bind, ai_pqres_bind, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchBound, ai_pqres_fetch_bound, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchRow, ai_pqres_fetch_row, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchCol, ai_pqres_fetch_col, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchAll, ai_pqres_fetch_all, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, count, ai_pqres_count, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, map, ai_pqres_map, ZEND_ACC_PUBLIC)
	{0}
};

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_construct, 0, 0, 3)
	ZEND_ARG_OBJ_INFO(0, Connection, pq\\Connection, 0)
	ZEND_ARG_INFO(0, type)
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
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Oss|a/!b", &zconn, php_pqconn_class_entry, &name_str, &name_len, &query_str, &query_len, &ztypes, &async);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			if (async) {
				rv = php_pqconn_prepare_async(zconn, conn_obj, name_str, query_str, ztypes ? Z_ARRVAL_P(ztypes) : NULL TSRMLS_CC);
			} else {
				rv = php_pqconn_prepare(zconn, conn_obj, name_str, query_str, ztypes ? Z_ARRVAL_P(ztypes) : NULL TSRMLS_CC);
			}

			if (SUCCESS == rv) {
				php_pqstm_t *stm = ecalloc(1, sizeof(*stm));

				php_pq_object_addref(conn_obj TSRMLS_CC);
				stm->conn = conn_obj;
				stm->name = estrdup(name_str);
				ZEND_INIT_SYMTABLE(&stm->bound);
				obj->intern = stm;
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
	zval *param_ref;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "lz", &param_no, &param_ref)) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else {
			Z_ADDREF_P(param_ref);
			zend_hash_index_update(&obj->intern->bound, param_no, (void *) &param_ref, sizeof(zval *), NULL);
			zend_hash_sort(&obj->intern->bound, zend_qsort, compare_index, 0 TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_exec, 0, 0, 0)
	ZEND_ARG_ARRAY_INFO(0, params, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, exec) {
	zend_error_handling zeh;
	zval *zparams = NULL;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|a/!", &zparams);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else {
			int count = 0;
			char **params = NULL;
			HashTable zdtor;
			PGresult *res;

			ZEND_INIT_SYMTABLE(&zdtor);

			if (zparams) {
				count = php_pq_params_to_array(Z_ARRVAL_P(zparams), &params, &zdtor TSRMLS_CC);
			} else {
				count = php_pq_params_to_array(&obj->intern->bound, &params, &zdtor TSRMLS_CC);
			}

			res = PQexecPrepared(obj->intern->conn->intern->conn, obj->intern->name, count, (const char *const*) params, NULL, NULL, 0);

			if (params) {
				efree(params);
			}
			zend_hash_destroy(&zdtor);

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
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|a/!f", &zparams, &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else {
			int count;
			char **params = NULL;
			HashTable zdtor;

			if (zparams) {
				ZEND_INIT_SYMTABLE(&zdtor);
				count = php_pq_params_to_array(Z_ARRVAL_P(zparams), &params, &zdtor TSRMLS_CC);
			}

			if (!PQsendQueryPrepared(obj->intern->conn->intern->conn, obj->intern->name, count, (const char *const*) params, NULL, NULL, 0)) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to execute statement (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else if (obj->intern->conn->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn->intern->conn)) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				php_pq_callback_dtor(&obj->intern->conn->intern->onevent);
				if (resolver.fci.size > 0) {
					obj->intern->conn->intern->onevent = resolver;
					php_pq_callback_addref(&obj->intern->conn->intern->onevent);
				}
				obj->intern->conn->intern->poller = PQconsumeInput;
			}

			if (params) {
				efree(params);
			}
			if (zparams) {
				zend_hash_destroy(&zdtor);
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_desc, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, desc) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
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
				PHP_PQclear(res);
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_desc_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, descAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Statement not initialized");
		} else if (!PQsendDescribePrepared(obj->intern->conn->intern->conn, obj->intern->name)) {
			throw_exce(EX_IO TSRMLS_CC, "Failed to describe statement: %s", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
		} else {
			obj->intern->conn->intern->poller = PQconsumeInput;
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

static zend_function_entry php_pqstm_methods[] = {
	PHP_ME(pqstm, __construct, ai_pqstm_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqstm, bind, ai_pqstm_bind, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, exec, ai_pqstm_exec, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, desc, ai_pqstm_desc, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, execAsync, ai_pqstm_exec_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, descAsync, ai_pqstm_desc_async, ZEND_ACC_PUBLIC)
	{0}
};

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
	ZEND_ARG_INFO(0, async)
	ZEND_ARG_INFO(0, isolation)
	ZEND_ARG_INFO(0, readonly)
	ZEND_ARG_INFO(0, deferrable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, __construct) {
	zend_error_handling zeh;
	zval *zconn;
	long isolation = PHP_PQTXN_READ_COMMITTED;
	zend_bool async = 0, readonly = 0, deferrable = 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|blbb", &zconn, php_pqconn_class_entry, &async, &isolation, &readonly, &deferrable);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			if (async) {
				rv = php_pqconn_start_transaction_async(zconn, conn_obj, isolation, readonly, deferrable TSRMLS_CC);
			} else {
				rv = php_pqconn_start_transaction(zconn, conn_obj, isolation, readonly, deferrable TSRMLS_CC);
			}

			if (SUCCESS == rv) {
				php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

				obj->intern = ecalloc(1, sizeof(*obj->intern));

				php_pq_object_addref(conn_obj TSRMLS_CC);
				obj->intern->conn = conn_obj;
				obj->intern->open = 1;
				obj->intern->isolation = isolation;
				obj->intern->readonly = readonly;
				obj->intern->deferrable = deferrable;
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_savepoint, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, savepoint) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction already closed");
		} else {
			PGresult *res;
			smart_str cmd = {0};

			smart_str_appends(&cmd, "SAVEPOINT \"");
			smart_str_append_unsigned(&cmd, ++obj->intern->savepoint);
			smart_str_appends(&cmd, "\"");
			smart_str_0(&cmd);

			res = PQexec(obj->intern->conn->intern->conn, cmd.c);

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to create %s (%s)", cmd.c, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				php_pqres_success(res TSRMLS_CC);
				PHP_PQclear(res);
			}

			smart_str_free(&cmd);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_savepoint_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, savepointAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction already closed");
		} else {
			smart_str cmd = {0};

			smart_str_appends(&cmd, "SAVEPOINT \"");
			smart_str_append_unsigned(&cmd, ++obj->intern->savepoint);
			smart_str_appends(&cmd, "\"");
			smart_str_0(&cmd);

			if (!PQsendQuery(obj->intern->conn->intern->conn, cmd.c)) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to create %s (%s)", cmd.c, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}

			smart_str_free(&cmd);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_commit, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, commit) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transacation not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transacation already closed");
		} else {
			PGresult *res;
			smart_str cmd = {0};

			if (!obj->intern->savepoint) {
				res = PQexec(obj->intern->conn->intern->conn, "COMMIT");
			} else {
				smart_str_appends(&cmd, "RELEASE SAVEPOINT \"");
				smart_str_append_unsigned(&cmd, obj->intern->savepoint--);
				smart_str_appends(&cmd, "\"");
				smart_str_0(&cmd);

				res = PQexec(obj->intern->conn->intern->conn, cmd.c);
			}

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to %s (%s)", cmd.c ? cmd.c : "commit transaction", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					if (!cmd.c) {
						obj->intern->open = 0;
					}
				}
				PHP_PQclear(res);
			}

			smart_str_free(&cmd);
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_commit_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, commitAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction already closed");
		} else {
			int rc;
			smart_str cmd = {0};

			if (!obj->intern->savepoint) {
				rc = PQsendQuery(obj->intern->conn->intern->conn, "COMMIT");
			} else {
				smart_str_appends(&cmd, "RELEASE SAVEPOINT \"");
				smart_str_append_unsigned(&cmd, obj->intern->savepoint--);
				smart_str_appends(&cmd, "\"");
				smart_str_0(&cmd);

				rc = PQsendQuery(obj->intern->conn->intern->conn, cmd.c);
			}

			if (!rc) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to %s (%s)", cmd.c ? cmd.c : "commmit transaction", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (!cmd.c) {
					obj->intern->open = 0;
				}
				obj->intern->conn->intern->poller = PQconsumeInput;
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			}

			smart_str_free(&cmd);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_rollback, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, rollback) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction already closed");
		} else {
			PGresult *res;
			smart_str cmd = {0};

			if (!obj->intern->savepoint) {
				res = PQexec(obj->intern->conn->intern->conn, "ROLLBACK");
			} else {
				smart_str_appends(&cmd, "ROLLBACK TO SAVEPOINT \"");
				smart_str_append_unsigned(&cmd, obj->intern->savepoint--);
				smart_str_appends(&cmd, "\"");
				smart_str_0(&cmd);

				res = PQexec(obj->intern->conn->intern->conn, cmd.c);
			}

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to %s (%s)", cmd.c ? cmd.c : "rollback transaction", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					if (!cmd.c) {
						obj->intern->open = 0;
					}
				}
				PHP_PQclear(res);
			}

			smart_str_free(&cmd);
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_rollback_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, rollbackAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction already closed");
		} else {
			int rc;
			smart_str cmd = {0};

			if (!obj->intern->savepoint) {
				rc = PQsendQuery(obj->intern->conn->intern->conn, "ROLLBACK");
			} else {
				smart_str_appends(&cmd, "ROLLBACK TO SAVEPOINT \"");
				smart_str_append_unsigned(&cmd, obj->intern->savepoint--);
				smart_str_appends(&cmd, "\"");
				smart_str_0(&cmd);

				rc = PQsendQuery(obj->intern->conn->intern->conn, cmd.c);
			}

			if (!rc) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to %s (%s)", cmd.c ? cmd.c : "rollback transaction", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (!cmd.c) {
					obj->intern->open = 0;
				}
				obj->intern->conn->intern->poller = PQconsumeInput;
			}

			smart_str_free(&cmd);
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_export_snapshot, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, exportSnapshot) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			PGresult *res = PQexec(obj->intern->conn->intern->conn, "SELECT pg_export_snapshot()");

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to export transaction snapshot (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					RETVAL_STRING(PQgetvalue(res, 0, 0), 1);
				}

				PHP_PQclear(res);
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_export_snapshot_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, exportSnapshotAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!PQsendQuery(obj->intern->conn->intern->conn, "SELECT pg_export_snapshot()")) {
			throw_exce(EX_IO TSRMLS_CC, "Failed to export transaction snapshot (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
		} else {
			obj->intern->conn->intern->poller = PQconsumeInput;
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_import_snapshot, 0, 0, 1)
	ZEND_ARG_INFO(0, snapshot_id)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, importSnapshot) {
	zend_error_handling zeh;
	char *snapshot_str;
	int snapshot_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &snapshot_str, &snapshot_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (obj->intern->isolation < PHP_PQTXN_REPEATABLE_READ) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction must have at least isolation level REPEATABLE READ to be able to import a snapshot");
		} else {
			char *sid = PQescapeLiteral(obj->intern->conn->intern->conn, snapshot_str, snapshot_len);

			if (!sid) {
				throw_exce(EX_ESCAPE TSRMLS_CC, "Failed to quote snapshot identifier (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				PGresult *res;
				smart_str cmd = {0};

				smart_str_appends(&cmd, "SET TRANSACTION SNAPSHOT ");
				smart_str_appends(&cmd, sid);
				smart_str_0(&cmd);

				res = PQexec(obj->intern->conn->intern->conn, cmd.c);

				if (!res) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to import transaction snapshot (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else {
					php_pqres_success(res TSRMLS_CC);
					PHP_PQclear(res);
				}

				smart_str_free(&cmd);
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_import_snapshot_async, 0, 0, 1)
	ZEND_ARG_INFO(0, snapshot_id)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, importSnapshotAsync) {
	zend_error_handling zeh;
	char *snapshot_str;
	int snapshot_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &snapshot_str, &snapshot_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (obj->intern->isolation < PHP_PQTXN_REPEATABLE_READ) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction must have at least isolation level REPEATABLE READ to be able to import a snapshot");
		} else {
			char *sid = PQescapeLiteral(obj->intern->conn->intern->conn, snapshot_str, snapshot_len);

			if (!sid) {
				throw_exce(EX_ESCAPE TSRMLS_CC, "Failed to quote snapshot identifier (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				smart_str cmd = {0};

				smart_str_appends(&cmd, "SET TRANSACTION SNAPSHOT ");
				smart_str_appends(&cmd, sid);
				smart_str_0(&cmd);

				if (!PQsendQuery(obj->intern->conn->intern->conn, cmd.c)) {
					throw_exce(EX_IO TSRMLS_CC, "Failed to %s (%s)", cmd.c, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else {
					obj->intern->conn->intern->poller = PQconsumeInput;
				}

				smart_str_free(&cmd);
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			}
		}
	}
}

static const char *strmode(long mode)
{
	switch (mode & (INV_READ|INV_WRITE)) {
	case INV_READ|INV_WRITE:
		return "rw";
	case INV_READ:
		return "r";
	case INV_WRITE:
		return "w";
	default:
		return "-";
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_open_lob, 0, 0, 1)
	ZEND_ARG_INFO(0, oid)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, openLOB) {
	zend_error_handling zeh;
	long mode = INV_WRITE|INV_READ, loid;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|l", &loid, &mode);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			int lofd = lo_open(obj->intern->conn->intern->conn, loid, mode);

			if (lofd < 0) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to open large object with oid=%u with mode '%s' (%s)", loid, strmode(mode), PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				php_pqlob_t *lob = ecalloc(1, sizeof(*lob));

				lob->lofd = lofd;
				lob->loid = loid;
				php_pq_object_addref(obj TSRMLS_CC);
				lob->txn = obj;

				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqlob_create_object_ex(php_pqlob_class_entry, lob, NULL TSRMLS_CC);
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_create_lob, 0, 0, 0)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, createLOB) {
	zend_error_handling zeh;
	long mode = INV_WRITE|INV_READ;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &mode);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			Oid loid = lo_creat(obj->intern->conn->intern->conn, mode);

			if (loid == InvalidOid) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to create large object with mode '%s' (%s)", strmode(mode), PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				int lofd = lo_open(obj->intern->conn->intern->conn, loid, mode);

				if (lofd < 0) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to open large object with oid=%u with mode '%s': %s", loid, strmode(mode), PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else {
					php_pqlob_t *lob = ecalloc(1, sizeof(*lob));

					lob->lofd = lofd;
					lob->loid = loid;
					php_pq_object_addref(obj TSRMLS_CC);
					lob->txn = obj;

					return_value->type = IS_OBJECT;
					return_value->value.obj = php_pqlob_create_object_ex(php_pqlob_class_entry, lob, NULL TSRMLS_CC);
				}
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_unlink_lob, 0, 0, 1)
	ZEND_ARG_INFO(0, oid)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, unlinkLOB) {
	zend_error_handling zeh;
	long loid;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &loid);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			int rc = lo_unlink(obj->intern->conn->intern->conn, loid);

			if (rc != 1) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to unlink LOB (oid=%u): %s", loid, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_import_lob, 0, 0, 1)
	ZEND_ARG_INFO(0, local_path)
	ZEND_ARG_INFO(0, oid)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, importLOB) {
	zend_error_handling zeh;
	char *path_str;
	int path_len;
	long oid = InvalidOid;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "p|l", &path_str, &path_len, &oid);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (rv == SUCCESS) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			if (oid == InvalidOid) {
				oid = lo_import(obj->intern->conn->intern->conn, path_str);
			} else {
				oid = lo_import_with_oid(obj->intern->conn->intern->conn, path_str, oid);
			}

			if (oid == InvalidOid) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to import LOB from '%s' (%s)", path_str, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(oid);
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_export_lob, 0, 0, 2)
	ZEND_ARG_INFO(0, oid)
	ZEND_ARG_INFO(0, local_path)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, exportLOB) {
	zend_error_handling zeh;
	char *path_str;
	int path_len;
	long oid;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "lp", &oid, &path_str, &path_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (rv == SUCCESS) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			int rc = lo_export(obj->intern->conn->intern->conn, oid, path_str);

			if (rc == -1) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to export LOB (oid=%u) to '%s' (%s)", oid, path_str, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

static zend_function_entry php_pqtxn_methods[] = {
	PHP_ME(pqtxn, __construct, ai_pqtxn_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqtxn, commit, ai_pqtxn_commit, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, rollback, ai_pqtxn_rollback, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, commitAsync, ai_pqtxn_commit_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, rollbackAsync, ai_pqtxn_rollback_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, savepoint, ai_pqtxn_savepoint, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, savepointAsync, ai_pqtxn_savepoint_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, exportSnapshot, ai_pqtxn_export_snapshot, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, exportSnapshotAsync, ai_pqtxn_export_snapshot_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, importSnapshot, ai_pqtxn_import_snapshot, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, importSnapshotAsync, ai_pqtxn_import_snapshot_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, openLOB, ai_pqtxn_open_lob, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, createLOB, ai_pqtxn_create_lob, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, unlinkLOB, ai_pqtxn_unlink_lob, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, importLOB, ai_pqtxn_import_lob, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, exportLOB, ai_pqtxn_export_lob, ZEND_ACC_PUBLIC)
	{0}
};

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

static size_t php_pqlob_stream_write(php_stream *stream, const char *buffer, size_t length TSRMLS_DC)
{
	php_pqlob_object_t *obj = stream->abstract;
	int written = 0;

	if (obj) {
		written = lo_write(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, buffer, length);

		if (written < 0) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to write to LOB with oid=%u (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
		}

		php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
	}

	return written;
}

static size_t php_pqlob_stream_read(php_stream *stream, char *buffer, size_t length TSRMLS_DC)
{
	php_pqlob_object_t *obj = stream->abstract;
	int read = 0;

	if (obj) {

		if (!buffer && !length) {
			if (lo_tell(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd) == lo_lseek(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, 0, SEEK_CUR)) {
				return EOF;
			}
		} else {
			read = lo_read(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, buffer, length);

			if (read < 0) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to read from LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			}
		}

		php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
	}

	return read;
}

static STATUS php_pqlob_stream_close(php_stream *stream, int close_handle TSRMLS_DC)
{
	return SUCCESS;
}

static int php_pqlob_stream_flush(php_stream *stream TSRMLS_DC)
{
	return SUCCESS;
}

static STATUS php_pqlob_stream_seek(php_stream *stream, off_t offset, int whence, off_t *newoffset TSRMLS_DC)
{
	STATUS rv = FAILURE;
	php_pqlob_object_t *obj = stream->abstract;

	if (obj) {
		int position = lo_lseek(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, offset, whence);

		if (position < 0) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to seek offset in LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			rv = FAILURE;
		} else {
			*newoffset = position;
			rv = SUCCESS;
		}

		php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
	}

	return rv;
}

static php_stream_ops php_pqlob_stream_ops = {
	/* stdio like functions - these are mandatory! */
	php_pqlob_stream_write,
	php_pqlob_stream_read,
	php_pqlob_stream_close,
	php_pqlob_stream_flush,

	"pq\\LOB stream",

	/* these are optional */
	php_pqlob_stream_seek,
	NULL, /* cast */
	NULL, /* stat */
	NULL, /* set_option */
};

static void php_pqlob_object_update_stream(zval *this_ptr, php_pqlob_object_t *obj, zval **zstream_ptr TSRMLS_DC)
{
	zval *zstream, zmember;
	php_stream *stream;

	INIT_PZVAL(&zmember);
	ZVAL_STRINGL(&zmember, "stream", sizeof("stream")-1, 0);

	MAKE_STD_ZVAL(zstream);
	if (!obj) {
		obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	}
	stream = php_stream_alloc(&php_pqlob_stream_ops, obj, NULL, "r+b");
	stream->flags |= PHP_STREAM_FLAG_NO_FCLOSE;
	zend_list_addref(obj->intern->stream = stream->rsrc_id);
	php_stream_to_zval(stream, zstream);

	zend_get_std_object_handlers()->write_property(getThis(), &zmember, zstream, NULL TSRMLS_CC);

	if (zstream_ptr) {
		*zstream_ptr = zstream;
	} else {
		zval_ptr_dtor(&zstream);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, transaction, pq\\Transaction, 0)
	ZEND_ARG_INFO(0, oid)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, __construct) {
	zend_error_handling zeh;
	zval *ztxn;
	long mode = INV_WRITE|INV_READ, loid = InvalidOid;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|ll", &ztxn, php_pqtxn_class_entry, &loid, &mode);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *txn_obj = zend_object_store_get_object(ztxn TSRMLS_CC);

		if (!txn_obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!txn_obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transation already closed");
		} else {
			if (loid == InvalidOid) {
				loid = lo_creat(txn_obj->intern->conn->intern->conn, mode);
			}

			if (loid == InvalidOid) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to create large object with mode '%s' (%s)", strmode(mode), PHP_PQerrorMessage(txn_obj->intern->conn->intern->conn));
			} else {
				int lofd = lo_open(txn_obj->intern->conn->intern->conn, loid, mode);

				if (lofd < 0) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to open large object with oid=%u with mode '%s' (%s)", loid, strmode(mode), PHP_PQerrorMessage(txn_obj->intern->conn->intern->conn));
				} else {
					php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

					obj->intern = ecalloc(1, sizeof(*obj->intern));
					obj->intern->lofd = lofd;
					obj->intern->loid = loid;
					php_pq_object_addref(txn_obj TSRMLS_CC);
					obj->intern->txn = txn_obj;
				}
			}

			php_pqconn_notify_listeners(txn_obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_write, 0, 0, 1)
	ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, write) {
	zend_error_handling zeh;
	char *data_str;
	int data_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &data_str, &data_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\LOB not initialized");
		} else {
			int written = lo_write(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, data_str, data_len);

			if (written < 0) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to write to LOB with oid=%u (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(written);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_read, 0, 0, 0)
	ZEND_ARG_INFO(0, length)
	ZEND_ARG_INFO(1, read)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, read) {
	zend_error_handling zeh;
	long length = 0x1000;
	zval *zread = NULL;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lz!", &length, &zread);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\LOB not initialized");
		} else {
			char *buffer = emalloc(length + 1);
			int read = lo_read(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, buffer, length);

			if (read < 0) {
				efree(buffer);
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to read from LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				if (zread) {
					zval_dtor(zread);
					ZVAL_LONG(zread, read);
				}
				buffer[read] = '\0';
				RETVAL_STRINGL(buffer, read, 0);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_seek, 0, 0, 1)
	ZEND_ARG_INFO(0, offset)
	ZEND_ARG_INFO(0, whence)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, seek) {
	zend_error_handling zeh;
	long offset, whence = SEEK_SET;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|l", &offset, &whence);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\LOB not initialized");
		} else {
			int position = lo_lseek(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, offset, whence);

			if (position < 0) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to seek offset in LOB with oid=%d (%s)", obj->intern->loid,	PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(position);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_tell, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, tell) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\LOB not initialized");
		} else {
			int position = lo_tell(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd);

			if (position < 0) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to tell offset in LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(position);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_truncate, 0, 0, 0)
	ZEND_ARG_INFO(0, length)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, truncate) {
	zend_error_handling zeh;
	long length = 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &length);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\LOB not initialized");
		} else {
			int rc = lo_truncate(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, length);

			if (rc != 0) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to truncate LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
		}
	}
}

static zend_function_entry php_pqlob_methods[] = {
	PHP_ME(pqlob, __construct, ai_pqlob_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqlob, write, ai_pqlob_write, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, read, ai_pqlob_read, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, seek, ai_pqlob_seek, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, tell, ai_pqlob_tell, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, truncate, ai_pqlob_truncate, ZEND_ACC_PUBLIC)
	{0}
};

ZEND_BEGIN_ARG_INFO_EX(ai_pqcopy_construct, 0, 0, 3)
	ZEND_ARG_OBJ_INFO(0, "connection", pq\\Connection, 0)
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

static zend_function_entry php_pqexc_methods[] = {
	{0}
};

/* {{{ PHP_MINIT_FUNCTION
 */
static PHP_MINIT_FUNCTION(pq)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Exception", php_pqexc_methods);
	php_pqexc_interface_class_entry = zend_register_internal_interface(&ce TSRMLS_CC);

	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("INVALID_ARGUMENT"), EX_INVALID_ARGUMENT TSRMLS_CC);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("RUNTIME"), EX_RUNTIME TSRMLS_CC);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("CONNECTION_FAILED"), EX_CONNECTION_FAILED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("IO"), EX_IO TSRMLS_CC);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("ESCAPE"), EX_ESCAPE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("BAD_METHODCALL"), EX_BAD_METHODCALL TSRMLS_CC);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("UNINITIALIZED"), EX_UNINITIALIZED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("DOMAIN"), EX_DOMAIN TSRMLS_CC);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("SQL"), EX_SQL TSRMLS_CC);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq\\Exception", "InvalidArgumentException", php_pqexc_methods);
	php_pqexc_invalid_argument_class_entry = zend_register_internal_class_ex(&ce, spl_ce_InvalidArgumentException, "InvalidArgumentException" TSRMLS_CC);
	zend_class_implements(php_pqexc_invalid_argument_class_entry TSRMLS_CC, 1, php_pqexc_interface_class_entry);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq\\Exception", "RuntimeException", php_pqexc_methods);
	php_pqexc_runtime_class_entry = zend_register_internal_class_ex(&ce, spl_ce_RuntimeException, "RuntimeException" TSRMLS_CC);
	zend_class_implements(php_pqexc_runtime_class_entry TSRMLS_CC, 1, php_pqexc_interface_class_entry);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq\\Exception", "BadMethodCallException", php_pqexc_methods);
	php_pqexc_bad_methodcall_class_entry = zend_register_internal_class_ex(&ce, spl_ce_BadMethodCallException, "BadMethodCallException" TSRMLS_CC);
	zend_class_implements(php_pqexc_bad_methodcall_class_entry TSRMLS_CC, 1, php_pqexc_interface_class_entry);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq\\Exception", "DomainException", php_pqexc_methods);
	php_pqexc_domain_class_entry = zend_register_internal_class_ex(&ce, spl_ce_DomainException, "DomainException" TSRMLS_CC);
	zend_class_implements(php_pqexc_domain_class_entry TSRMLS_CC, 1, php_pqexc_interface_class_entry);
	zend_declare_property_null(php_pqexc_domain_class_entry, ZEND_STRL("sqlstate"), ZEND_ACC_PUBLIC TSRMLS_CC);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Connection", php_pqconn_methods);
	php_pqconn_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqconn_class_entry->create_object = php_pqconn_create_object;

	memcpy(&php_pqconn_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqconn_object_handlers.read_property = php_pq_object_read_prop;
	php_pqconn_object_handlers.write_property = php_pq_object_write_prop;
	php_pqconn_object_handlers.clone_obj = NULL;
	php_pqconn_object_handlers.get_property_ptr_ptr = NULL;
	php_pqconn_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqconn_object_prophandlers, 14, NULL, NULL, 1);

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

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("busy"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_busy;
	zend_hash_add(&php_pqconn_object_prophandlers, "busy", sizeof("busy"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("encoding"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_encoding;
	ph.write = php_pqconn_object_write_encoding;
	zend_hash_add(&php_pqconn_object_prophandlers, "encoding", sizeof("encoding"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("unbuffered"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_unbuffered;
	ph.write = php_pqconn_object_write_unbuffered;
	zend_hash_add(&php_pqconn_object_prophandlers, "unbuffered", sizeof("unbuffered"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("db"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_db;
	zend_hash_add(&php_pqconn_object_prophandlers, "db", sizeof("db"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("user"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_user;
	zend_hash_add(&php_pqconn_object_prophandlers, "user", sizeof("user"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("pass"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_pass;
	zend_hash_add(&php_pqconn_object_prophandlers, "pass", sizeof("pass"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("host"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_host;
	zend_hash_add(&php_pqconn_object_prophandlers, "host", sizeof("host"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("port"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_port;
	zend_hash_add(&php_pqconn_object_prophandlers, "port", sizeof("port"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("options"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_options;
	zend_hash_add(&php_pqconn_object_prophandlers, "options", sizeof("options"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("eventHandlers"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_event_handlers;
	zend_hash_add(&php_pqconn_object_prophandlers, "eventHandlers", sizeof("eventHandlers"), (void *) &ph, sizeof(ph), NULL);

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

	zend_declare_class_constant_stringl(php_pqconn_class_entry, ZEND_STRL("EVENT_NOTICE"), ZEND_STRL("notice") TSRMLS_CC);
	zend_declare_class_constant_stringl(php_pqconn_class_entry, ZEND_STRL("EVENT_RESULT"), ZEND_STRL("result") TSRMLS_CC);
	zend_declare_class_constant_stringl(php_pqconn_class_entry, ZEND_STRL("EVENT_RESET"), ZEND_STRL("reset") TSRMLS_CC);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("ASYNC"), 0x1 TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("PERSISTENT"), 0x2 TSRMLS_CC);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Types", php_pqtypes_methods);
	php_pqtypes_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqtypes_class_entry->create_object = php_pqtypes_create_object;

	memcpy(&php_pqtypes_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqtypes_object_handlers.read_property = php_pq_object_read_prop;
	php_pqtypes_object_handlers.write_property = php_pq_object_write_prop;
	php_pqtypes_object_handlers.clone_obj = NULL;
	php_pqtypes_object_handlers.get_property_ptr_ptr = NULL;
	php_pqtypes_object_handlers.get_debug_info = php_pq_object_debug_info;
	php_pqtypes_object_handlers.has_dimension = php_pqtypes_object_has_dimension;
	php_pqtypes_object_handlers.read_dimension = php_pqtypes_object_read_dimension;
	php_pqtypes_object_handlers.unset_dimension = NULL;
	php_pqtypes_object_handlers.write_dimension = NULL;

	zend_hash_init(&php_pqtypes_object_prophandlers, 1, NULL, NULL, 1);

	zend_declare_property_null(php_pqtypes_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtypes_object_read_connection;
	zend_hash_add(&php_pqtypes_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Result", php_pqres_methods);
	php_pqres_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqres_class_entry->create_object = php_pqres_create_object;
	php_pqres_class_entry->iterator_funcs.funcs = &php_pqres_iterator_funcs;
	php_pqres_class_entry->get_iterator = php_pqres_iterator_init;
	zend_class_implements(php_pqres_class_entry TSRMLS_CC, 2, zend_ce_traversable, spl_ce_Countable);

	memcpy(&php_pqres_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqres_object_handlers.read_property = php_pq_object_read_prop;
	php_pqres_object_handlers.write_property = php_pq_object_write_prop;
	php_pqres_object_handlers.clone_obj = NULL;
	php_pqres_object_handlers.get_property_ptr_ptr = NULL;
	php_pqres_object_handlers.get_debug_info = php_pq_object_debug_info;
	php_pqres_object_handlers.count_elements = php_pqres_count_elements;

	zend_hash_init(&php_pqres_object_prophandlers, 6, NULL, NULL, 1);

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("status"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_status;
	zend_hash_add(&php_pqres_object_prophandlers, "status", sizeof("status"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("statusMessage"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_status_message;
	zend_hash_add(&php_pqres_object_prophandlers, "statusMessage", sizeof("statusMessage"), (void *) &ph, sizeof(ph), NULL);

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

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Statement", php_pqstm_methods);
	php_pqstm_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqstm_class_entry->create_object = php_pqstm_create_object;

	memcpy(&php_pqstm_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqstm_object_handlers.read_property = php_pq_object_read_prop;
	php_pqstm_object_handlers.write_property = php_pq_object_write_prop;
	php_pqstm_object_handlers.clone_obj = NULL;
	php_pqstm_object_handlers.get_property_ptr_ptr = NULL;
	php_pqstm_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqstm_object_prophandlers, 2, NULL, NULL, 1);

	zend_declare_property_null(php_pqstm_class_entry, ZEND_STRL("name"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqstm_object_read_name;
	zend_hash_add(&php_pqstm_object_prophandlers, "name", sizeof("name"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqstm_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqstm_object_read_connection;
	zend_hash_add(&php_pqstm_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Transaction", php_pqtxn_methods);
	php_pqtxn_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqtxn_class_entry->create_object = php_pqtxn_create_object;

	memcpy(&php_pqtxn_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqtxn_object_handlers.read_property = php_pq_object_read_prop;
	php_pqtxn_object_handlers.write_property = php_pq_object_write_prop;
	php_pqtxn_object_handlers.clone_obj = NULL;
	php_pqtxn_object_handlers.get_property_ptr_ptr = NULL;
	php_pqtxn_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqtxn_object_prophandlers, 4, NULL, NULL, 1);

	zend_declare_property_null(php_pqtxn_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_connection;
	zend_hash_add(&php_pqtxn_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqtxn_class_entry, ZEND_STRL("isolation"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_isolation;
	ph.write = php_pqtxn_object_write_isolation;
	zend_hash_add(&php_pqtxn_object_prophandlers, "isolation", sizeof("isolation"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_bool(php_pqtxn_class_entry, ZEND_STRL("readonly"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_readonly;
	ph.write = php_pqtxn_object_write_readonly;
	zend_hash_add(&php_pqtxn_object_prophandlers, "readonly", sizeof("readonly"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_bool(php_pqtxn_class_entry, ZEND_STRL("deferrable"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_deferrable;
	ph.write = php_pqtxn_object_write_deferrable;
	zend_hash_add(&php_pqtxn_object_prophandlers, "deferrable", sizeof("deferrable"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_class_constant_long(php_pqtxn_class_entry, ZEND_STRL("READ_COMMITTED"), PHP_PQTXN_READ_COMMITTED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqtxn_class_entry, ZEND_STRL("REPEATABLE_READ"), PHP_PQTXN_REPEATABLE_READ TSRMLS_CC);
	zend_declare_class_constant_long(php_pqtxn_class_entry, ZEND_STRL("SERIALIZABLE"), PHP_PQTXN_SERIALIZABLE TSRMLS_CC);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Cancel", php_pqcancel_methods);
	php_pqcancel_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqcancel_class_entry->create_object = php_pqcancel_create_object;

	memcpy(&php_pqcancel_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqcancel_object_handlers.read_property = php_pq_object_read_prop;
	php_pqcancel_object_handlers.write_property = php_pq_object_write_prop;
	php_pqcancel_object_handlers.clone_obj = NULL;
	php_pqcancel_object_handlers.get_property_ptr_ptr = NULL;
	php_pqcancel_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqcancel_object_prophandlers, 1, NULL, NULL, 1);

	zend_declare_property_null(php_pqcancel_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcancel_object_read_connection;
	zend_hash_add(&php_pqcancel_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "LOB", php_pqlob_methods);
	php_pqlob_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqlob_class_entry->create_object = php_pqlob_create_object;

	memcpy(&php_pqlob_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqlob_object_handlers.read_property = php_pq_object_read_prop;
	php_pqlob_object_handlers.write_property = php_pq_object_write_prop;
	php_pqlob_object_handlers.clone_obj = NULL;
	php_pqlob_object_handlers.get_property_ptr_ptr = NULL;
	php_pqlob_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqlob_object_prophandlers, 3, NULL, NULL, 1);

	zend_declare_property_null(php_pqlob_class_entry, ZEND_STRL("transaction"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqlob_object_read_transaction;
	zend_hash_add(&php_pqlob_object_prophandlers, "transaction", sizeof("transaction"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqlob_class_entry, ZEND_STRL("oid"), InvalidOid, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqlob_object_read_oid;
	zend_hash_add(&php_pqlob_object_prophandlers, "oid", sizeof("oid"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqlob_class_entry, ZEND_STRL("stream"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqlob_object_read_stream;
	zend_hash_add(&php_pqlob_object_prophandlers, "stream", sizeof("stream"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("INVALID_OID"), InvalidOid TSRMLS_CC);
	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("R"), INV_READ TSRMLS_CC);
	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("W"), INV_WRITE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("RW"), INV_READ|INV_WRITE TSRMLS_CC);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "COPY", php_pqcopy_methods);
	php_pqcopy_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqcopy_class_entry->create_object = php_pqcopy_create_object;

	memcpy(&php_pqcopy_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqcopy_object_handlers.read_property = php_pq_object_read_prop;
	php_pqcopy_object_handlers.write_property = php_pq_object_write_prop;
	php_pqcopy_object_handlers.clone_obj = NULL;
	php_pqcopy_object_handlers.get_property_ptr_ptr = NULL;
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

	php_persistent_handle_provide(ZEND_STRL("pq\\Connection"), &php_pqconn_resource_factory_ops, NULL, NULL TSRMLS_CC);

	/*
	REGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
static PHP_MSHUTDOWN_FUNCTION(pq)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	php_persistent_handle_cleanup(ZEND_STRL("pq\\Connection"), NULL, 0 TSRMLS_CC);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
static PHP_MINFO_FUNCTION(pq)
{
#ifdef HAVE_PQLIBVERSION
	int libpq_v;
#endif
	char libpq_version[10] = "pre-9.1";

	php_info_print_table_start();
	php_info_print_table_header(2, "PQ Support", "enabled");
	php_info_print_table_row(2, "Extension Version", PHP_PQ_EXT_VERSION);
	php_info_print_table_end();

	php_info_print_table_start();
	php_info_print_table_header(2, "Used Library", "Version");
#ifdef HAVE_PQLIBVERSION
	libpq_v = PQlibVersion();
	slprintf(libpq_version, sizeof(libpq_version), "%d.%d.%d", libpq_v/10000%100, libpq_v/100%100, libpq_v%100);
#endif
	php_info_print_table_row(2, "libpq", libpq_version);
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

const zend_function_entry pq_functions[] = {
	{0}
};

static zend_module_dep pq_module_deps[] = {
	ZEND_MOD_REQUIRED("raphf")
	ZEND_MOD_REQUIRED("spl")
	ZEND_MOD_END
};

/* {{{ pq_module_entry
 */
zend_module_entry pq_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	pq_module_deps,
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


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
