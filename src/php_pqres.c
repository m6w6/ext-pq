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

static zend_object_iterator *php_pqres_iterator_init(zend_class_entry *ce, zval *object, int by_ref TSRMLS_DC)
{
	php_pqres_iterator_t *iter;
	zval *prop, *zfetch_type;

	iter = ecalloc(1, sizeof(*iter));
	iter->zi.funcs = &php_pqres_iterator_funcs;
	iter->zi.data = object;
	/* do not addref, because the iterator lives inside this object anyway */

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

zval *php_pqres_typed_zval(php_pqres_t *res, char *val, size_t len, Oid typ TSRMLS_DC)
{
	zval *zv, **zconv;

	MAKE_STD_ZVAL(zv);

	if (SUCCESS == zend_hash_index_find(&res->converters, typ, (void *) &zconv)) {
		zval *ztype, *tmp = NULL;

		MAKE_STD_ZVAL(ztype);
		ZVAL_LONG(ztype, typ);
		ZVAL_STRINGL(zv, val, len, 1);
		zend_call_method_with_2_params(zconv, NULL, NULL, "convertfromstring", &tmp, zv, ztype);
		zval_ptr_dtor(&ztype);

		if (tmp) {
			zval_ptr_dtor(&zv);
			zv = tmp;
		}

		return zv;
	}

	switch (typ) {
	case PHP_PQ_OID_BOOL:
		if (!(res->auto_convert & PHP_PQRES_CONV_BOOL)) {
			goto noconversion;
		}
		ZVAL_BOOL(zv, *val == 't');
		break;
#if SIZEOF_LONG >= 8
	case PHP_PQ_OID_INT8:
	case PHP_PQ_OID_TID:
#endif
	case PHP_PQ_OID_INT4:
	case PHP_PQ_OID_INT2:
	case PHP_PQ_OID_XID:
	case PHP_PQ_OID_OID:
		if (!(res->auto_convert & PHP_PQRES_CONV_INT)) {
			goto noconversion;
		}
		ZVAL_LONG(zv, zend_atol(val, len));
		break;

	case PHP_PQ_OID_FLOAT4:
	case PHP_PQ_OID_FLOAT8:
		if (!(res->auto_convert & PHP_PQRES_CONV_FLOAT)) {
			goto noconversion;
		}
		ZVAL_DOUBLE(zv, zend_strtod(val, NULL));
		break;

	case PHP_PQ_OID_DATE:
		if (!(res->auto_convert & PHP_PQRES_CONV_DATETIME)) {
			goto noconversion;
		}
		php_pqdt_from_string(val, len, "Y-m-d", zv TSRMLS_CC);
		break;

	case PHP_PQ_OID_ABSTIME:
		if (!(res->auto_convert & PHP_PQRES_CONV_DATETIME)) {
			goto noconversion;
		}
		php_pqdt_from_string(val, len, "Y-m-d H:i:s", zv TSRMLS_CC);
		break;

	case PHP_PQ_OID_TIMESTAMP:
		if (!(res->auto_convert & PHP_PQRES_CONV_DATETIME)) {
			goto noconversion;
		}
		php_pqdt_from_string(val, len, "Y-m-d H:i:s.u", zv TSRMLS_CC);
		break;

	case PHP_PQ_OID_TIMESTAMPTZ:
		if (!(res->auto_convert & PHP_PQRES_CONV_DATETIME)) {
			goto noconversion;
		}
		php_pqdt_from_string(val, len, "Y-m-d H:i:s.uO", zv TSRMLS_CC);
		break;

#ifdef PHP_PQ_OID_JSON
#	ifdef PHP_PQ_OID_JSONB
	case PHP_PQ_OID_JSONB:
#	endif
	case PHP_PQ_OID_JSON:
		if (!(res->auto_convert & PHP_PQRES_CONV_JSON)) {
			goto noconversion;
		}
		php_json_decode_ex(zv, val, len, 0, 512 /* PHP_JSON_DEFAULT_DEPTH */ TSRMLS_CC);
		break;
#endif

	default:
		if (!(res->auto_convert & PHP_PQRES_CONV_ARRAY)) {
			goto noconversion;
		}
		if (PHP_PQ_TYPE_IS_ARRAY(typ) && (Z_ARRVAL_P(zv) = php_pq_parse_array(res, val, len, PHP_PQ_TYPE_OF_ARRAY(typ) TSRMLS_CC))) {
			Z_TYPE_P(zv) = IS_ARRAY;
		} else {
			noconversion:
			ZVAL_STRINGL(zv, val, len, 1);
		}
		break;
	}

	return zv;
}

static inline zval *php_pqres_get_col(php_pqres_t *r, unsigned row, unsigned col TSRMLS_DC)
{
	zval *zv;

	if (PQgetisnull(r->res, row, col)) {
		MAKE_STD_ZVAL(zv);
		ZVAL_NULL(zv);
	} else {
		zv = php_pqres_typed_zval(r, PQgetvalue(r->res, row, col), PQgetlength(r->res, row, col), PQftype(r->res, col) TSRMLS_CC);
	}

	return zv;
}

static inline void php_pqres_add_col_to_zval(php_pqres_t *r, unsigned row, unsigned col, php_pqres_fetch_t fetch_type, zval *data TSRMLS_DC)
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
		zval *zv;

		zv = php_pqres_typed_zval(r, PQgetvalue(r->res, row, col), PQgetlength(r->res, row, col), PQftype(r->res, col) TSRMLS_CC);

		switch (fetch_type) {
		case PHP_PQRES_FETCH_OBJECT:
			add_property_zval(data, PQfname(r->res, col), zv);
			zval_ptr_dtor(&zv);
			break;

		case PHP_PQRES_FETCH_ASSOC:
			add_assoc_zval(data, PQfname(r->res, col), zv);
			break;

		case PHP_PQRES_FETCH_ARRAY:
			add_index_zval(data, col, zv);
			break;
		}
	}
}

zval *php_pqres_row_to_zval(PGresult *res, unsigned row, php_pqres_fetch_t fetch_type, zval **data_ptr TSRMLS_DC)
{
	zval *data = NULL;
	int c, cols;
	php_pqres_object_t *res_obj = PQresultInstanceData(res, php_pqconn_event);

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
			php_pqres_add_col_to_zval(res_obj->intern, row, c, fetch_type, data TSRMLS_CC);
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

#if PHP_VERSION_ID >= 50500
static void php_pqres_iterator_key(zend_object_iterator *i, zval *key TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	ZVAL_LONG(key, iter->index);
}
#else
static int php_pqres_iterator_key(zend_object_iterator *i, char **key_str, uint *key_len, ulong *key_num TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	*key_num = (ulong) iter->index;

	return HASH_KEY_IS_LONG;
}
#endif

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

STATUS php_pqres_success(PGresult *res TSRMLS_DC)
{
	zval *zexc;

	switch (PQresultStatus(res)) {
	case PGRES_BAD_RESPONSE:
	case PGRES_NONFATAL_ERROR:
	case PGRES_FATAL_ERROR:
		zexc = throw_exce(EX_SQL TSRMLS_CC, "%s", PHP_PQresultErrorMessage(res));
		zend_update_property_string(Z_OBJCE_P(zexc), zexc, ZEND_STRL("sqlstate"), PQresultErrorField(res, PG_DIAG_SQLSTATE) TSRMLS_CC);
		return FAILURE;
	default:
		return SUCCESS;
	}
}

void php_pqres_init_instance_data(PGresult *res, php_pqconn_object_t *conn_obj, php_pqres_object_t **ptr TSRMLS_DC)
{
	php_pqres_object_t *obj;
	php_pqres_t *r = ecalloc(1, sizeof(*r));

	r->res = res;
	zend_hash_init(&r->bound, 0, 0, ZVAL_PTR_DTOR, 0);
	zend_hash_init(&r->converters, 0, 0, ZVAL_PTR_DTOR, 0);
	zend_hash_copy(&r->converters, &conn_obj->intern->converters, (copy_ctor_func_t) zval_add_ref, NULL, sizeof(zval *));

	r->auto_convert = conn_obj->intern->default_auto_convert;
	r->default_fetch_type = conn_obj->intern->default_fetch_type;

	php_pqres_create_object_ex(php_pqres_class_entry, r, &obj TSRMLS_CC);
	PQresultSetInstanceData(res, php_pqconn_event, obj);

	if (ptr) {
		*ptr = obj;
	}
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
		zend_hash_destroy(&obj->intern->converters);

		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

zend_object_value php_pqres_create_object_ex(zend_class_entry *ce, php_pqres_t *intern, php_pqres_object_t **ptr TSRMLS_DC)
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

static zend_object_value php_pqres_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqres_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
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
		RETVAL_LONG(obj->intern->default_fetch_type);
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

static void php_pqres_object_read_auto_conv(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(obj->intern->auto_convert);
}

static void php_pqres_object_write_auto_conv(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;
	zval *zauto_conv = value;

	if (Z_TYPE_P(value) != IS_LONG) {
		if (Z_REFCOUNT_P(value) > 1) {
			zval *tmp;
			MAKE_STD_ZVAL(tmp);
			ZVAL_ZVAL(tmp, zauto_conv, 1, 0);
			convert_to_long(tmp);
			zauto_conv = tmp;
		} else {
			convert_to_long_ex(&zauto_conv);
		}
	}

	obj->intern->auto_convert = Z_LVAL_P(zauto_conv);

	if (zauto_conv != value) {
		zval_ptr_dtor(&zauto_conv);
	}
}

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
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to find column at index %ld", index);
		return FAILURE;
	}
	if (col->num == -1) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to find column with name '%s'", name);
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
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z/z", &zcol, &zref);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
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
				 fetch_type = obj->intern->iter ? obj->intern->iter->fetch_type : obj->intern->default_fetch_type;
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

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_col, 0, 0, 1)
	ZEND_ARG_INFO(1, ref)
	ZEND_ARG_INFO(0, col)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchCol) {
	zend_error_handling zeh;
	zval *zcol = NULL, *zref;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|z/!", &zref, &zcol);
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
				php_pqres_col_t col;

				if (SUCCESS != column_nn(obj, zcol, &col TSRMLS_CC)) {
					RETVAL_FALSE;
				} else {
					zval **zres = column_at(*row, col.num TSRMLS_CC);

					if (!zres) {
						RETVAL_FALSE;
					} else {
						zval_dtor(zref);
						ZVAL_ZVAL(zref, *zres, 1, 0);
						RETVAL_TRUE;
					}
				}
			}
			zend_restore_error_handling(&zeh TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_all_cols, 0, 0, 0)
	ZEND_ARG_INFO(0, col)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchAllCols) {
	zend_error_handling zeh;
	zval *zcol = NULL;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|z!", &zcol);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Result not initialized");
		} else {
			php_pqres_col_t col;

			zend_replace_error_handling(EH_THROW, exce(EX_RUNTIME), &zeh TSRMLS_CC);
			if (SUCCESS == column_nn(obj, zcol, &col TSRMLS_CC)) {
				int r, rows = PQntuples(obj->intern->res);

				array_init(return_value);
				for (r = 0; r < rows; ++r) {
					add_next_index_zval(return_value, php_pqres_get_col(obj->intern, r, col.num TSRMLS_CC));
				}
			}
			zend_restore_error_handling(&zeh TSRMLS_CC);
		}
	}
}

struct apply_to_col_arg {
	php_pqres_object_t *obj;
	php_pqres_col_t *cols;
	STATUS status;
};

static int apply_to_col(void *p, void *a TSRMLS_DC)
{
	zval **c = p;
	struct apply_to_col_arg *arg = a;

	if (SUCCESS != column_nn(arg->obj, *c, arg->cols TSRMLS_CC)) {
		arg->status = FAILURE;
		return ZEND_HASH_APPLY_STOP;
	} else {
		arg->status = SUCCESS;
		++arg->cols;
		return ZEND_HASH_APPLY_KEEP;
	}
}

static php_pqres_col_t *php_pqres_convert_to_cols(php_pqres_object_t *obj, HashTable *ht TSRMLS_DC)
{
	struct apply_to_col_arg arg = {NULL};
	php_pqres_col_t *tmp;

	arg.obj = obj;
	arg.cols = ecalloc(zend_hash_num_elements(ht), sizeof(*tmp));
	tmp = arg.cols;
	zend_hash_apply_with_argument(ht, apply_to_col, &arg TSRMLS_CC);

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
				fetch_type = obj->intern->iter ? obj->intern->iter->fetch_type : obj->intern->default_fetch_type;
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
				 fetch_type = obj->intern->iter ? obj->intern->iter->fetch_type : obj->intern->default_fetch_type;
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

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_desc, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, desc) {
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Result not initialized");
		} else {
			int p, params;

			array_init(return_value);
			for (p = 0, params = PQnparams(obj->intern->res); p < params; ++p) {
				add_next_index_long(return_value, PQparamtype(obj->intern->res, p));
			}
		}
	}
}

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
	php_pqres_object_handlers.get_gc = NULL;
	php_pqres_object_handlers.get_debug_info = php_pq_object_debug_info;
	php_pqres_object_handlers.get_properties = php_pq_object_properties;
	php_pqres_object_handlers.count_elements = php_pqres_count_elements;

	zend_hash_init(&php_pqres_object_prophandlers, 8, NULL, NULL, 1);

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

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("autoConvert"), PHP_PQRES_CONV_ALL, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_auto_conv;
	ph.write = php_pqres_object_write_auto_conv;
	zend_hash_add(&php_pqres_object_prophandlers, "autoConvert", sizeof("autoConvert"), (void *) &ph, sizeof(ph), NULL);
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

	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_BOOL"), PHP_PQRES_CONV_BOOL TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_INT"), PHP_PQRES_CONV_INT TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_FLOAT"), PHP_PQRES_CONV_FLOAT TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_SCALAR"), PHP_PQRES_CONV_SCALAR TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_ARRAY"), PHP_PQRES_CONV_ARRAY TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_DATETIME"), PHP_PQRES_CONV_DATETIME TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_JSON"), PHP_PQRES_CONV_JSON TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("CONV_ALL"), PHP_PQRES_CONV_ALL TSRMLS_CC);

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
