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

#include <ext/spl/spl_iterators.h>
#include <ext/json/php_json.h>

#include <libpq-events.h>

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pq_object.h"
#include "php_pqexc.h"
#include "php_pqres.h"
#undef PHP_PQ_TYPE
#include "php_pq_type.h"

zend_class_entry *php_pqres_class_entry;
static zend_object_handlers php_pqres_object_handlers;
static HashTable php_pqres_object_prophandlers;
static zend_object_iterator_funcs php_pqres_iterator_funcs;

static inline zend_object_iterator *php_pqres_iterator_init_ex(zend_class_entry *ce, zval *object, int by_ref)
{
	php_pqres_iterator_t *iter;
	zval tmp, *zfetch_type;

	iter = ecalloc(1, sizeof(*iter));
	iter->zi.funcs = &php_pqres_iterator_funcs;
	ZVAL_COPY_VALUE(&iter->zi.data, object);

	zfetch_type = php_pq_read_property(object, "fetchType", &tmp);
	iter->fetch_type = zval_get_long(zfetch_type);
#if DBG_GC
	fprintf(stderr, "INIT iter(#%d) %p res(#%d) %p\n", iter->zi.std.handle, iter, Z_OBJ_HANDLE_P(object), PHP_PQ_OBJ(object, NULL));
#endif
	return (zend_object_iterator *) iter;
}

static zend_object_iterator *php_pqres_iterator_init(zend_class_entry *ce, zval *object, int by_ref)
{
	zend_object_iterator *iter = php_pqres_iterator_init_ex(ce, object, by_ref);

	zend_iterator_init(iter);
	Z_ADDREF_P(object);

	return iter;
}
static void php_pqres_internal_iterator_init(zval *zobj)
{
	php_pqres_object_t *obj = PHP_PQ_OBJ(zobj, NULL);

	obj->intern->iter = (php_pqres_iterator_t *) php_pqres_iterator_init_ex(Z_OBJCE_P(zobj), zobj, 0);
	obj->intern->iter->zi.funcs->rewind((zend_object_iterator *) obj->intern->iter);

}

static inline void php_pqres_iterator_dtor_ex(zend_object_iterator *i)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

#if DBG_GC
	fprintf(stderr, "FREE iter(#%d) rc=%d %p\n", iter->zi.std.handle, GC_REFCOUNT(&iter->zi.std), iter);
#endif
	if (!Z_ISUNDEF(iter->current_val)) {
		zval_ptr_dtor(&iter->current_val);
		ZVAL_UNDEF(&iter->current_val);
	}
}

static void php_pqres_iterator_dtor(zend_object_iterator *i)
{
	php_pqres_iterator_dtor_ex(i);
	zval_ptr_dtor(&i->data);
}

static void php_pqres_internal_iterator_dtor(php_pqres_object_t *obj)
{
	if (obj->intern && obj->intern->iter) {
		php_pqres_iterator_dtor_ex((zend_object_iterator *) obj->intern->iter);
		efree(obj->intern->iter);
		obj->intern->iter = NULL;
	}
}

static ZEND_RESULT_CODE php_pqres_iterator_valid(zend_object_iterator *i)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;
	php_pqres_object_t *obj = PHP_PQ_OBJ(&i->data, NULL);

	switch (PQresultStatus(obj->intern->res)) {
	case PGRES_TUPLES_OK:
#ifdef HAVE_PGRES_SINGLE_TUPLE
	case PGRES_SINGLE_TUPLE:
#endif
		if (PQntuples(obj->intern->res) <= iter->index) {
			return FAILURE;
		}
		break;
	default:
		return FAILURE;
	}

	return SUCCESS;
}

#define PHP_PQRES_JSON_OPTIONS(res) \
	(php_pqres_fetch_type(res) != PHP_PQRES_FETCH_OBJECT ? PHP_JSON_OBJECT_AS_ARRAY:0)

zval *php_pqres_typed_zval(php_pqres_t *res, Oid typ, zval *zv)
{
	zval *zconv;
	HashTable *ht;
	zend_string *str;

	if ((zconv = zend_hash_index_find(&res->converters, typ))) {
		zval ztype, rv;

		ZVAL_NULL(&rv);
		ZVAL_LONG(&ztype, typ);
		php_pq_call_method(zconv, "convertfromstring", 2, &rv, zv, &ztype);

		zval_ptr_dtor(zv);
		ZVAL_ZVAL(zv, &rv, 0, 0);

		return zv;
	}

	str = zval_get_string(zv);
	zval_ptr_dtor(zv);

	switch (typ) {
	case PHP_PQ_OID_BOOL:
		if (!(res->auto_convert & PHP_PQRES_CONV_BOOL)) {
			goto noconversion;
		}
		ZVAL_BOOL(zv, *str->val == 't');
		break;

	case PHP_PQ_OID_INT8:
	case PHP_PQ_OID_TID:
	case PHP_PQ_OID_INT4:
	case PHP_PQ_OID_INT2:
	case PHP_PQ_OID_XID:
	case PHP_PQ_OID_OID:
		if (!(res->auto_convert & PHP_PQRES_CONV_INT)) {
			goto noconversion;
		}
		{
			zend_long lval;
			double dval;

			switch (is_numeric_str_function(str, &lval, &dval)) {
				case IS_LONG:
					ZVAL_LONG(zv, lval);
					break;
				case IS_DOUBLE:
					ZVAL_DOUBLE(zv, dval);
					break;
				default:
					goto noconversion;
			}
		}
		break;

	case PHP_PQ_OID_FLOAT4:
	case PHP_PQ_OID_FLOAT8:
		if (!(res->auto_convert & PHP_PQRES_CONV_FLOAT)) {
			goto noconversion;
		}
		ZVAL_DOUBLE(zv, zend_strtod(str->val, NULL));
		break;

	case PHP_PQ_OID_DATE:
		if (!(res->auto_convert & PHP_PQRES_CONV_DATETIME)) {
			goto noconversion;
		}
		php_pqdt_from_string(zv, NULL, str->val, str->len, "Y-m-d", NULL);
		break;
#ifdef PHP_PQ_OID_ABSTIME
	case PHP_PQ_OID_ABSTIME:
		if (!(res->auto_convert & PHP_PQRES_CONV_DATETIME)) {
			goto noconversion;
		}
		php_pqdt_from_string(zv, NULL, str->val, str->len, "Y-m-d H:i:s", NULL);
		break;
#endif
	case PHP_PQ_OID_TIMESTAMP:
		if (!(res->auto_convert & PHP_PQRES_CONV_DATETIME)) {
			goto noconversion;
		}
		php_pqdt_from_string(zv, NULL, str->val, str->len, "Y-m-d H:i:s.u", NULL);
		break;

	case PHP_PQ_OID_TIMESTAMPTZ:
		if (!(res->auto_convert & PHP_PQRES_CONV_DATETIME)) {
			goto noconversion;
		}
		php_pqdt_from_string(zv, NULL, str->val, str->len, "Y-m-d H:i:s.uO", NULL);
		break;

#ifdef PHP_PQ_OID_JSON
# ifdef PHP_PQ_OID_JSONB
	case PHP_PQ_OID_JSONB:
# endif
	case PHP_PQ_OID_JSON:
		if (!(res->auto_convert & PHP_PQRES_CONV_JSON)) {
			goto noconversion;
		}
		php_json_decode_ex(zv, str->val, str->len, PHP_PQRES_JSON_OPTIONS(res), 512 /* PHP_JSON_DEFAULT_DEPTH */);
		break;
#endif

	case PHP_PQ_OID_BYTEA:
		if (!(res->auto_convert & PHP_PQRES_CONV_BYTEA)) {
			goto noconversion;
		} else {
			size_t to_len;
			char *to_str = (char *) PQunescapeBytea((unsigned char *) str->val, &to_len);

			if (!to_str) {
				ZVAL_NULL(zv);
				php_error_docref(NULL, E_WARNING, "Failed to unsescape BYTEA: '%s'", str->val);
			} else {
				ZVAL_STRINGL(zv, to_str, to_len);
				PQfreemem(to_str);
			}
		}
		break;

	default:
		if (!(res->auto_convert & PHP_PQRES_CONV_ARRAY)) {
			goto noconversion;
		}
		if (PHP_PQ_TYPE_IS_ARRAY(typ) && (ht = php_pq_parse_array(res, str->val, str->len, PHP_PQ_TYPE_OF_ARRAY(typ)))) {
			ZVAL_ARR(zv, ht);
		} else {
			goto noconversion;
		}
		break;
	}

	zend_string_release(str);
	return zv;

	noconversion:
	ZVAL_STR(zv, str);
	return zv;
}

static inline zval *php_pqres_get_col(php_pqres_t *r, unsigned row, unsigned col, zval *zv)
{
	if (PQgetisnull(r->res, row, col)) {
		ZVAL_NULL(zv);
	} else {
		ZVAL_STRINGL(zv, PQgetvalue(r->res, row, col), PQgetlength(r->res, row, col));
		zv = php_pqres_typed_zval(r, PQftype(r->res, col), zv);
	}

	return zv;
}

static inline void php_pqres_add_col_to_zval(php_pqres_t *r, unsigned row, unsigned col, php_pqres_fetch_t fetch_type, zval *data)
{
	if (PQgetisnull(r->res, row, col)) {
		switch (fetch_type) {
		case PHP_PQRES_FETCH_OBJECT:
			add_property_null(data, PQfname(r->res, col));
			break;

		case PHP_PQRES_FETCH_ASSOC:
			add_assoc_null(data, PQfname(r->res, col));
			break;

		case PHP_PQRES_FETCH_ARRAY:
			add_index_null(data, col);
			break;
		}
	} else {
		zval zv;

		ZVAL_STRINGL(&zv, PQgetvalue(r->res, row, col), PQgetlength(r->res, row, col));
		php_pqres_typed_zval(r, PQftype(r->res, col), &zv);

		switch (fetch_type) {
		case PHP_PQRES_FETCH_OBJECT:
			add_property_zval(data, PQfname(r->res, col), &zv);
			zval_ptr_dtor(&zv);
			break;

		case PHP_PQRES_FETCH_ASSOC:
			add_assoc_zval(data, PQfname(r->res, col), &zv);
			break;

		case PHP_PQRES_FETCH_ARRAY:
			add_index_zval(data, col, &zv);
			break;
		}
	}
}

zval *php_pqres_row_to_zval(PGresult *res, unsigned row, php_pqres_fetch_t fetch_type, zval *data)
{
	int c, cols = PQnfields(res);
	php_pqres_object_t *res_obj = PQresultInstanceData(res, php_pqconn_event);

	if (Z_TYPE_P(data) != IS_OBJECT && Z_TYPE_P(data) != IS_ARRAY) {
		if (PHP_PQRES_FETCH_OBJECT == fetch_type) {
			object_init(data);
		} else {
			array_init_size(data, cols);
		}
	}

	if (PQntuples(res) > row) {
		for (c = 0; c < cols; ++c) {
			php_pqres_add_col_to_zval(res_obj->intern, row, c, fetch_type, data);
		}
	}

	return data;
}

static zval *php_pqres_iterator_current(zend_object_iterator *i)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;
	php_pqres_object_t *obj = PHP_PQ_OBJ(&i->data, NULL);

	if (Z_ISUNDEF(iter->current_val)) {
		php_pqres_row_to_zval(obj->intern->res, iter->index, iter->fetch_type, &iter->current_val);
	}
	return &iter->current_val;
}

static void php_pqres_iterator_key(zend_object_iterator *i, zval *key)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	ZVAL_LONG(key, iter->index);
}

static void php_pqres_iterator_invalidate(zend_object_iterator *i)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	if (!Z_ISUNDEF(iter->current_val)) {
		zval_ptr_dtor(&iter->current_val);
		ZVAL_UNDEF(&iter->current_val);
	}
}

static void php_pqres_iterator_next(zend_object_iterator *i)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	php_pqres_iterator_invalidate(i);
	++iter->index;
}

static void php_pqres_iterator_rewind(zend_object_iterator *i)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	php_pqres_iterator_invalidate(i);
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
#if PHP_VERSION_ID >= 80000
	, NULL
#endif
};

static inline ZEND_RESULT_CODE php_pqres_count_elements_ex(zend_object *object, zend_long *count)
{
	php_pqres_object_t *obj = PHP_PQ_OBJ(NULL, object);

	if (!obj->intern) {
		return FAILURE;
	} else {
		*count = (zend_long) PQntuples(obj->intern->res);
		return SUCCESS;
	}
}
#if PHP_VERSION_ID >= 80000
static ZEND_RESULT_CODE php_pqres_count_elements(zend_object *object, zend_long *count)
{
	return php_pqres_count_elements_ex(object, count);
}
#else
static ZEND_RESULT_CODE php_pqres_count_elements(zval *object, zend_long *count)
{
	return php_pqres_count_elements_ex(Z_OBJ_P(object), count);
}
#endif

ZEND_RESULT_CODE php_pqres_success(PGresult *res)
{
	zval zexc, zsqlstate;

	switch (PQresultStatus(res)) {
	case PGRES_BAD_RESPONSE:
	case PGRES_NONFATAL_ERROR:
	case PGRES_FATAL_ERROR:
		ZVAL_OBJ(&zexc, throw_exce(EX_SQL, "%s", PHP_PQresultErrorMessage(res)));
		ZVAL_STRING(&zsqlstate, PQresultErrorField(res, PG_DIAG_SQLSTATE));
		php_pq_update_property(&zexc, "sqlstate", &zsqlstate);
		zval_ptr_dtor(&zsqlstate);
		return FAILURE;
	default:
		return SUCCESS;
	}
}

php_pqres_object_t *php_pqres_init_instance_data(PGresult *res, php_pqconn_object_t *conn_obj)
{
	php_pqres_object_t *obj;
	php_pqres_t *r = ecalloc(1, sizeof(*r));

	r->res = res;
	zend_hash_init(&r->bound, 0, 0, ZVAL_PTR_DTOR, 0);
	zend_hash_init(&r->converters, zend_hash_num_elements(&conn_obj->intern->converters), 0, ZVAL_PTR_DTOR, 0);
	zend_hash_copy(&r->converters, &conn_obj->intern->converters, (copy_ctor_func_t) zval_add_ref);

	r->auto_convert = conn_obj->intern->default_auto_convert;
	r->default_fetch_type = conn_obj->intern->default_fetch_type;

	obj = php_pqres_create_object_ex(php_pqres_class_entry, r);
	PQresultSetInstanceData(res, php_pqconn_event, obj);

	return obj;
}

php_pqres_fetch_t php_pqres_fetch_type(php_pqres_t *res)
{
	return res->iter ? res->iter->fetch_type : res->default_fetch_type;
}

static void php_pqres_object_free(zend_object *o)
{
	php_pqres_object_t *obj = PHP_PQ_OBJ(NULL, o);

	if (obj->intern) {
		if (obj->intern->res) {
			PQresultSetInstanceData(obj->intern->res, php_pqconn_event, NULL);
			PQclear(obj->intern->res);
			obj->intern->res = NULL;
		}

		php_pqres_internal_iterator_dtor(obj);

		zend_hash_destroy(&obj->intern->bound);
		zend_hash_destroy(&obj->intern->converters);

		efree(obj->intern);
		obj->intern = NULL;
	}
	php_pq_object_dtor(o);
}

php_pqres_object_t *php_pqres_create_object_ex(zend_class_entry *ce, php_pqres_t *intern)
{
	return php_pq_object_create(ce, intern, sizeof(php_pqres_object_t),
			&php_pqres_object_handlers, &php_pqres_object_prophandlers);
}

static zend_object *php_pqres_create_object(zend_class_entry *class_type)
{
	return &php_pqres_create_object_ex(class_type, NULL)->zo;
}

static void php_pqres_object_read_status(void *o, zval *return_value)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQresultStatus(obj->intern->res));
}

static void php_pqres_object_read_status_message(void *o, zval *return_value)
{
	php_pqres_object_t *obj = o;

	RETVAL_STRING(PQresStatus(PQresultStatus(obj->intern->res))+sizeof("PGRES"));
}

static void php_pqres_object_read_error_message(void *o, zval *return_value)
{
	php_pqres_object_t *obj = o;
	char *error = PHP_PQresultErrorMessage(obj->intern->res);

	if (error) {
		RETVAL_STRING(error);
	} else {
		RETVAL_NULL();
	}
}

#ifndef PG_DIAG_SEVERITY
# define PG_DIAG_SEVERITY 'S'
#endif
#ifndef PG_DIAG_SQLSTATE
# define PG_DIAG_SQLSTATE 'C'
#endif
#ifndef PG_DIAG_MESSAGE_PRIMARY
# define PG_DIAG_MESSAGE_PRIMARY 'M'
#endif
#ifndef PG_DIAG_MESSAGE_DETAIL
# define PG_DIAG_MESSAGE_DETAIL 'D'
#endif
#ifndef PG_DIAG_MESSAGE_HINT
# define PG_DIAG_MESSAGE_HINT 'H'
#endif
#ifndef PG_DIAG_STATEMENT_POSITION
# define PG_DIAG_STATEMENT_POSITION 'P'
#endif
#ifndef PG_DIAG_INTERNAL_POSITION
# define PG_DIAG_INTERNAL_POSITION 'p'
#endif
#ifndef PG_DIAG_INTERNAL_QUERY
# define PG_DIAG_INTERNAL_QUERY 'q'
#endif
#ifndef PG_DIAG_CONTEXT
# define PG_DIAG_CONTEXT 'W'
#endif
#ifndef PG_DIAG_SCHEMA_NAME
# define PG_DIAG_SCHEMA_NAME 's'
#endif
#ifndef PG_DIAG_TABLE_NAME
# define PG_DIAG_TABLE_NAME 't'
#endif
#ifndef PG_DIAG_COLUMN_NAME
# define PG_DIAG_COLUMN_NAME 'c'
#endif
#ifndef PG_DIAG_DATATYPE_NAME
# define PG_DIAG_DATATYPE_NAME 'd'
#endif
#ifndef PG_DIAG_CONSTRAINT_NAME
# define PG_DIAG_CONSTRAINT_NAME 'n'
#endif
#ifndef PG_DIAG_SOURCE_FILE
# define PG_DIAG_SOURCE_FILE 'F'
#endif
#ifndef PG_DIAG_SOURCE_LINE
# define PG_DIAG_SOURCE_LINE 'L'
#endif
#ifndef PG_DIAG_SOURCE_FUNCTION
# define PG_DIAG_SOURCE_FUNCTION 'R'
#endif

static void php_pqres_object_read_diag(void *o, zval *return_value)
{
	php_pqres_object_t *obj = o;
	int i;
	struct {
		char code;
		const char *const name;
	} diag[] = {
		{PG_DIAG_SEVERITY,			"severity"},
		{PG_DIAG_SQLSTATE,			"sqlstate"},
		{PG_DIAG_MESSAGE_PRIMARY,	"message_primary"},
		{PG_DIAG_MESSAGE_DETAIL,	"message_detail"},
		{PG_DIAG_MESSAGE_HINT,		"message_hint"},
		{PG_DIAG_STATEMENT_POSITION,"statement_position"},
		{PG_DIAG_INTERNAL_POSITION,	"internal_position"},
		{PG_DIAG_INTERNAL_QUERY,	"internal_query"},
		{PG_DIAG_CONTEXT,			"context"},
		{PG_DIAG_SCHEMA_NAME,		"schema_name"},
		{PG_DIAG_TABLE_NAME,		"table_name"},
		{PG_DIAG_COLUMN_NAME,		"column_name"},
		{PG_DIAG_DATATYPE_NAME,		"datatype_name"},
		{PG_DIAG_CONSTRAINT_NAME,	"constraint_name"},
		{PG_DIAG_SOURCE_FILE,		"source_file"},
		{PG_DIAG_SOURCE_LINE,		"source_line"},
		{PG_DIAG_SOURCE_FUNCTION,	"source_function"},
	};

	array_init_size(return_value, 32);
	for (i = 0; i < sizeof(diag)/sizeof(diag[0]); ++i) {
		char *value = PQresultErrorField(obj->intern->res, diag[i].code);

		if (value) {
			add_assoc_string(return_value, diag[i].name, value);
		} else {
			add_assoc_null(return_value, diag[i].name);
		}
	}
}

static void php_pqres_object_read_num_rows(void *o, zval *return_value)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQntuples(obj->intern->res));
}

static void php_pqres_object_read_num_cols(void *o, zval *return_value)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQnfields(obj->intern->res));
}

static void php_pqres_object_read_affected_rows(void *o, zval *return_value)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(atoi(PQcmdTuples(obj->intern->res)));
}

static void php_pqres_object_read_fetch_type(void *o, zval *return_value)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(php_pqres_fetch_type(obj->intern));
}

static void php_pqres_object_write_fetch_type(void *o, zval *value)
{
	php_pqres_object_t *obj = o;

	if (!obj->intern->iter) {
		zval object;

		ZVAL_OBJ(&object, &obj->zo);
		php_pqres_internal_iterator_init(&object);
	}
	obj->intern->iter->fetch_type = zval_get_long(value);
}

static void php_pqres_object_read_auto_conv(void *o, zval *return_value)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(obj->intern->auto_convert);
}

static void php_pqres_object_write_auto_conv(void *o, zval *value)
{
	php_pqres_object_t *obj = o;

	obj->intern->auto_convert = zval_get_long(value);
}

static ZEND_RESULT_CODE php_pqres_iteration(zval *zobj, php_pqres_object_t *obj, php_pqres_fetch_t fetch_type, zval *row)
{
	ZEND_RESULT_CODE rv;
	php_pqres_fetch_t orig_fetch;

	if (!obj) {
		obj = PHP_PQ_OBJ(zobj, NULL);
	}

	if (obj->intern->iter) {
		obj->intern->iter->zi.funcs->move_forward((zend_object_iterator *) obj->intern->iter);
	} else {
		php_pqres_internal_iterator_init(zobj);
	}
	orig_fetch = obj->intern->iter->fetch_type;
	obj->intern->iter->fetch_type = fetch_type;
	if (SUCCESS == (rv = obj->intern->iter->zi.funcs->valid((zend_object_iterator *) obj->intern->iter))) {
		zval *tmp = obj->intern->iter->zi.funcs->get_current_data((zend_object_iterator *) obj->intern->iter);
		ZVAL_COPY_VALUE(row, tmp);
	}
	obj->intern->iter->fetch_type = orig_fetch;

	return rv;
}

typedef struct php_pqres_col {
	char *name;
	int num;
} php_pqres_col_t;

static ZEND_RESULT_CODE column_nn(php_pqres_object_t *obj, zval *zcol, php_pqres_col_t *col)
{
	zend_long index = -1;
	char *name = NULL;

	if (!zcol) {
		index = 0;
	} else {
		switch (Z_TYPE_P(zcol)) {
		case IS_NULL:
			index = 0;
			break;

		case IS_LONG:
			index = Z_LVAL_P(zcol);
			break;

		default:
			convert_to_string(zcol);
			/* no break */

		case IS_STRING:
			if (!is_numeric_string(Z_STRVAL_P(zcol), Z_STRLEN_P(zcol), &index, NULL, 0)) {
				name = Z_STRVAL_P(zcol);
			}
			break;
		}
	}

	if (name) {
		col->name = name;
		col->num = PQfnumber(obj->intern->res, name);
	} else {
		col->name = PQfname(obj->intern->res, index);
		col->num = index;
	}

	if (!col->name) {
		php_error_docref(NULL, E_WARNING, "Failed to find column at index %ld", index);
		return FAILURE;
	}
	if (col->num == -1) {
		php_error_docref(NULL, E_WARNING, "Failed to find column with name '%s'", name);
		return FAILURE;
	}
	return SUCCESS;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_bind, 0, 0, 2)
	ZEND_ARG_INFO(0, col)
	ZEND_ARG_INFO(1, ref)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, bind) {
	zval *zcol, *zref;
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "z/z", &zcol, &zref);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Result not initialized");
		} else {
			php_pqres_col_t col;

			if (SUCCESS != column_nn(obj, zcol, &col)) {
				RETVAL_FALSE;
			} else {
				Z_TRY_ADDREF_P(zref);

				if (!zend_hash_index_update(&obj->intern->bound, col.num, zref)) {
					php_error_docref(NULL, E_WARNING, "Failed to bind column %s@%d", col.name, col.num);
					RETVAL_FALSE;
				} else {
					zend_hash_sort(&obj->intern->bound, php_pq_compare_index, 0);
					RETVAL_TRUE;
				}
			}
		}
	}
}

static int apply_bound(zval *zbound, int argc, va_list argv, zend_hash_key *key)
{
	zval *zvalue;
	zval *zrow = va_arg(argv, zval *);
	ZEND_RESULT_CODE *rv = va_arg(argv, ZEND_RESULT_CODE *);

	if (!(zvalue = zend_hash_index_find(Z_ARRVAL_P(zrow), key->h))) {
		php_error_docref(NULL, E_WARNING, "Failed to find column ad index %lu", key->h);
		*rv = FAILURE;
		return ZEND_HASH_APPLY_STOP;
	} else {
		ZVAL_DEREF(zbound);
		zval_dtor(zbound);
		ZVAL_COPY(zbound, zvalue);
		*rv = SUCCESS;
		return ZEND_HASH_APPLY_KEEP;
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_bound, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchBound) {
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Result not initialized");
		} else {
			zval row;

			zend_replace_error_handling(EH_THROW, exce(EX_RUNTIME), &zeh);
			if (SUCCESS == php_pqres_iteration(getThis(), obj, PHP_PQRES_FETCH_ARRAY, &row)) {
				zend_hash_apply_with_arguments(&obj->intern->bound, apply_bound, 2, &row, &rv);

				if (SUCCESS == rv) {
					RETVAL_ZVAL(&row, 1, 0);
				}
			}
			zend_restore_error_handling(&zeh);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_row, 0, 0, 0)
	ZEND_ARG_INFO(0, fetch_type)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchRow) {
	zend_error_handling zeh;
	php_pqres_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);
	zend_long fetch_type = -1;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &fetch_type);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Result not initialized");
		} else {
			zval row;

			if (fetch_type == -1) {
				 fetch_type = php_pqres_fetch_type(obj->intern);
			}

			zend_replace_error_handling(EH_THROW, exce(EX_RUNTIME), &zeh);
			if (SUCCESS == php_pqres_iteration(getThis(), obj, fetch_type, &row)) {
				RETVAL_ZVAL(&row, 1, 0);
			}
			zend_restore_error_handling(&zeh);
		}
	}
}

static zval *column_at(zval *row, int col)
{
	zval *data = NULL;
	HashTable *ht = HASH_OF(row);
	int count = zend_hash_num_elements(ht);

	if (col >= count) {
		php_error_docref(NULL, E_WARNING, "Column index %d exceeds column count %d", col, count);
	} else {
		zend_hash_internal_pointer_reset(ht);
		while (col-- > 0) {
			zend_hash_move_forward(ht);
		}
		data = zend_hash_get_current_data(ht);
	}
	return data;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_col, 0, 0, 1)
	ZEND_ARG_INFO(1, ref)
	ZEND_ARG_INFO(0, col)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchCol) {
	zend_error_handling zeh;
	zval *zcol = NULL, *zref;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "z|z/!", &zref, &zcol);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Result not initialized");
		} else {
			zval row;

			zend_replace_error_handling(EH_THROW, exce(EX_RUNTIME), &zeh);
			if (SUCCESS == php_pqres_iteration(getThis(), obj, php_pqres_fetch_type(obj->intern), &row)) {
				php_pqres_col_t col;

				if (SUCCESS != column_nn(obj, zcol, &col)) {
					RETVAL_FALSE;
				} else {
					zval *zres = column_at(&row, col.num);

					if (!zres) {
						RETVAL_FALSE;
					} else {
						ZVAL_DEREF(zref);
						zval_dtor(zref);
						ZVAL_ZVAL(zref, zres, 1, 0);
						RETVAL_TRUE;
					}
				}
			}
			zend_restore_error_handling(&zeh);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_all_cols, 0, 0, 0)
	ZEND_ARG_INFO(0, col)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchAllCols) {
	zend_error_handling zeh;
	zval *zcol = NULL;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|z!", &zcol);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Result not initialized");
		} else {
			php_pqres_col_t col;

			zend_replace_error_handling(EH_THROW, exce(EX_RUNTIME), &zeh);
			if (SUCCESS == column_nn(obj, zcol, &col)) {
				int r, rows = PQntuples(obj->intern->res);
				zval tmp;

				array_init(return_value);
				for (r = 0; r < rows; ++r) {
					add_next_index_zval(return_value, php_pqres_get_col(obj->intern, r, col.num, &tmp));
				}
			}
			zend_restore_error_handling(&zeh);
		}
	}
}

struct apply_to_col_arg {
	php_pqres_object_t *obj;
	php_pqres_col_t *cols;
	ZEND_RESULT_CODE status;
};

static int apply_to_col(zval *c, void *a)
{
	struct apply_to_col_arg *arg = a;

	if (SUCCESS != column_nn(arg->obj, c, arg->cols)) {
		arg->status = FAILURE;
		return ZEND_HASH_APPLY_STOP;
	} else {
		arg->status = SUCCESS;
		++arg->cols;
		return ZEND_HASH_APPLY_KEEP;
	}
}

static php_pqres_col_t *php_pqres_convert_to_cols(php_pqres_object_t *obj, HashTable *ht)
{
	struct apply_to_col_arg arg = {NULL};
	php_pqres_col_t *tmp;

	arg.obj = obj;
	arg.cols = ecalloc(zend_hash_num_elements(ht), sizeof(*tmp));
	tmp = arg.cols;
	zend_hash_apply_with_argument(ht, apply_to_col, &arg);

	if (SUCCESS == arg.status) {
		return tmp;
	} else {
		efree(tmp);
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
	zend_long fetch_type = -1;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|z/!z/!l", &zkeys, &zvals, &fetch_type);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Result not initialized");
		} else {
			int ks = 0, vs = 0;
			php_pqres_col_t def = {PQfname(obj->intern->res, 0), 0}, *keys = NULL, *vals = NULL;

			if (zkeys) {
				convert_to_array(zkeys);

				if ((ks = zend_hash_num_elements(Z_ARRVAL_P(zkeys)))) {
					keys = php_pqres_convert_to_cols(obj, Z_ARRVAL_P(zkeys));
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
					vals = php_pqres_convert_to_cols(obj, Z_ARRVAL_P(zvals));
				}
			}

			if (fetch_type == -1) {
				fetch_type = php_pqres_fetch_type(obj->intern);
			}

			if (keys) {
				int rows, r;
				zval *cur;

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
					zval *ptr;

					cur = return_value;
					for (k = 0; k < ks; ++k) {
						char *key = PQgetvalue(obj->intern->res, r, keys[k].num);
						int len = PQgetlength(obj->intern->res, r, keys[k].num);

						if (!(ptr = zend_symtable_str_find(HASH_OF(cur), key, len))) {
							zval tmp;

							switch (fetch_type) {
							case PHP_PQRES_FETCH_ARRAY:
							case PHP_PQRES_FETCH_ASSOC:
								array_init(&tmp);
								break;
							case PHP_PQRES_FETCH_OBJECT:
								object_init(&tmp);
								break;
							}
							if (!(ptr = zend_symtable_str_update(HASH_OF(cur), key, len, &tmp))) {
								throw_exce(EX_RUNTIME, "Failed to create map");
								goto err;
							}
							cur = ptr;
						}
						cur = ptr;
					}
					if (vals && vs) {
						for (v = 0; v < vs; ++v) {
							char *val = PQgetvalue(obj->intern->res, r, vals[v].num);
							int len = PQgetlength(obj->intern->res, r, vals[v].num);

							switch (fetch_type) {
							case PHP_PQRES_FETCH_ARRAY:
								add_index_stringl(cur, vals[v].num, val, len);
								break;
							case PHP_PQRES_FETCH_ASSOC:
								add_assoc_stringl(cur, vals[v].name, val, len);
								break;
							case PHP_PQRES_FETCH_OBJECT:
								add_property_stringl(cur, vals[v].name, val, len);
								break;
							}
						}
					} else {
						php_pqres_row_to_zval(obj->intern->res, r, fetch_type, cur);
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
	zend_long fetch_type = -1;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &fetch_type);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Result not initialized");
		} else {
			int r, rows = PQntuples(obj->intern->res);
			zval tmp;

			if (fetch_type == -1) {
				 fetch_type = php_pqres_fetch_type(obj->intern);
			}

			array_init(return_value);
			for (r = 0; r < rows; ++r) {
				ZVAL_NULL(&tmp);
				add_next_index_zval(return_value, php_pqres_row_to_zval(obj->intern->res, r, fetch_type, &tmp));
			}
		}
	}
}

ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_TYPE_INFO_EX(ai_pqres_count, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, count) {
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		long count;

		if (SUCCESS != php_pqres_count_elements_ex(Z_OBJ_P(getThis()), &count)) {
			throw_exce(EX_UNINITIALIZED, "pq\\Result not initialized");
		} else {
			RETVAL_LONG(count);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_desc, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, desc) {
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Result not initialized");
		} else {
			int p, params;

			array_init(return_value);
			for (p = 0, params = PQnparams(obj->intern->res); p < params; ++p) {
				add_next_index_long(return_value, PQparamtype(obj->intern->res, p));
			}
		}
	}
}

#if PHP_VERSION_ID >= 80000
ZEND_BEGIN_ARG_WITH_TENTATIVE_RETURN_OBJ_INFO_EX(ai_pqres_getIterator, 0, 0, Traversable, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, getIterator)
{
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Result not initialized");
		} else {
			zend_create_internal_iterator_zval(return_value, getThis());
		}
	}
}
#endif

static zend_function_entry php_pqres_methods[] = {
	PHP_ME(pqres, bind, ai_pqres_bind, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchBound, ai_pqres_fetch_bound, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchRow, ai_pqres_fetch_row, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchCol, ai_pqres_fetch_col, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchAll, ai_pqres_fetch_all, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchAllCols, ai_pqres_fetch_all_cols, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, count, ai_pqres_count, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, map, ai_pqres_map, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, desc, ai_pqres_desc, ZEND_ACC_PUBLIC)
#if PHP_VERSION_ID >= 80000
	PHP_ME(pqres, getIterator, ai_pqres_getIterator, ZEND_ACC_PUBLIC)
#endif
	{0}
};

PHP_MSHUTDOWN_FUNCTION(pqres)
{
	zend_hash_destroy(&php_pqres_object_prophandlers);
	return SUCCESS;
}

PHP_MINIT_FUNCTION(pqres)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Result", php_pqres_methods);
	php_pqres_class_entry = zend_register_internal_class_ex(&ce, NULL);
	php_pqres_class_entry->create_object = php_pqres_create_object;
	php_pqres_class_entry->get_iterator = php_pqres_iterator_init;
#if PHP_VERSION_ID >= 80000
	zend_class_implements(php_pqres_class_entry, 2, zend_ce_aggregate, zend_ce_countable);
#else
	zend_class_implements(php_pqres_class_entry, 2, zend_ce_traversable, zend_ce_countable);
#endif

	memcpy(&php_pqres_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqres_object_handlers.offset = XtOffsetOf(php_pqres_object_t, zo);
	php_pqres_object_handlers.free_obj = php_pqres_object_free;
	php_pqres_object_handlers.read_property = php_pq_object_read_prop;
	php_pqres_object_handlers.write_property = php_pq_object_write_prop;
	php_pqres_object_handlers.clone_obj = NULL;
	php_pqres_object_handlers.get_property_ptr_ptr = php_pq_object_get_prop_ptr_null;
	php_pqres_object_handlers.get_gc = php_pq_object_get_gc;
	php_pqres_object_handlers.get_debug_info = php_pq_object_debug_info;
	php_pqres_object_handlers.get_properties = php_pq_object_properties;
	php_pqres_object_handlers.count_elements = php_pqres_count_elements;

	zend_hash_init(&php_pqres_object_prophandlers, 9, NULL, php_pq_object_prophandler_dtor, 1);

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("status"), ZEND_ACC_PUBLIC);
	ph.read = php_pqres_object_read_status;
	zend_hash_str_add_mem(&php_pqres_object_prophandlers, "status", sizeof("status")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("statusMessage"), ZEND_ACC_PUBLIC);
	ph.read = php_pqres_object_read_status_message;
	zend_hash_str_add_mem(&php_pqres_object_prophandlers, "statusMessage", sizeof("statusMessage")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("errorMessage"), ZEND_ACC_PUBLIC);
	ph.read = php_pqres_object_read_error_message;
	zend_hash_str_add_mem(&php_pqres_object_prophandlers, "errorMessage", sizeof("errorMessage")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("diag"), ZEND_ACC_PUBLIC);
	ph.read = php_pqres_object_read_diag;
	zend_hash_str_add_mem(&php_pqres_object_prophandlers, "diag", sizeof("diag")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("numRows"), 0, ZEND_ACC_PUBLIC);
	ph.read = php_pqres_object_read_num_rows;
	zend_hash_str_add_mem(&php_pqres_object_prophandlers, "numRows", sizeof("numRows")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("numCols"), 0, ZEND_ACC_PUBLIC);
	ph.read = php_pqres_object_read_num_cols;
	zend_hash_str_add_mem(&php_pqres_object_prophandlers, "numCols", sizeof("numCols")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("affectedRows"), 0, ZEND_ACC_PUBLIC);
	ph.read = php_pqres_object_read_affected_rows;
	zend_hash_str_add_mem(&php_pqres_object_prophandlers, "affectedRows", sizeof("affectedRows")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("fetchType"), PHP_PQRES_FETCH_ARRAY, ZEND_ACC_PUBLIC);
	ph.read = php_pqres_object_read_fetch_type;
	ph.write = php_pqres_object_write_fetch_type;
	zend_hash_str_add_mem(&php_pqres_object_prophandlers, "fetchType", sizeof("fetchType")-1, (void *) &ph, sizeof(ph));
	ph.write = NULL;

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("autoConvert"), PHP_PQRES_CONV_ALL, ZEND_ACC_PUBLIC);
	ph.read = php_pqres_object_read_auto_conv;
	ph.write = php_pqres_object_write_auto_conv;
	zend_hash_str_add_mem(&php_pqres_object_prophandlers, "autoConvert", sizeof("autoConvert")-1, (void *) &ph, sizeof(ph));
	ph.write = NULL;

	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("EMPTY_QUERY"), PGRES_EMPTY_QUERY);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COMMAND_OK"), PGRES_COMMAND_OK);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("TUPLES_OK"), PGRES_TUPLES_OK);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_OUT"), PGRES_COPY_OUT);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_IN"), PGRES_COPY_IN);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("BAD_RESPONSE"), PGRES_BAD_RESPONSE);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("NONFATAL_ERROR"), PGRES_NONFATAL_ERROR);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FATAL_ERROR"), PGRES_FATAL_ERROR);
#ifdef HAVE_PGRES_COPY_BOTH
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_BOTH"), PGRES_COPY_BOTH);
#endif
#ifdef HAVE_PGRES_SINGLE_TUPLE
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("SINGLE_TUPLE"), PGRES_SINGLE_TUPLE);
#endif

	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FETCH_ARRAY"), PHP_PQRES_FETCH_ARRAY);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FETCH_ASSOC"), PHP_PQRES_FETCH_ASSOC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FETCH_OBJECT"), PHP_PQRES_FETCH_OBJECT);

	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_BOOL"), PHP_PQRES_CONV_BOOL);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_INT"), PHP_PQRES_CONV_INT);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_FLOAT"), PHP_PQRES_CONV_FLOAT);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_SCALAR"), PHP_PQRES_CONV_SCALAR);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_ARRAY"), PHP_PQRES_CONV_ARRAY);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_DATETIME"), PHP_PQRES_CONV_DATETIME);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_JSON"), PHP_PQRES_CONV_JSON);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_BYTEA"), PHP_PQRES_CONV_BYTEA);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_ALL"), PHP_PQRES_CONV_ALL);

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
