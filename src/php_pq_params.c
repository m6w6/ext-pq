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
#include <ext/standard/php_string.h>
#include <ext/standard/php_smart_str.h>
#if PHP_PQ_HAVE_PHP_JSON_H
#include <php_json.h> /* we've added the include directory to INCLUDES */
#endif

#include <Zend/zend_interfaces.h>

#include <libpq-fe.h>

#include "php_pq.h"
#include "php_pq_params.h"
#include "php_pq_misc.h"
#undef PHP_PQ_TYPE
#include "php_pq_type.h"

void php_pq_params_set_type_conv(php_pq_params_t *p, HashTable *conv)
{
	zend_hash_clean(&p->type.conv);
	zend_hash_copy(&p->type.conv, conv, (copy_ctor_func_t) zval_add_ref, NULL, sizeof(zval *));
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

unsigned php_pq_params_set_type_oids(php_pq_params_t *p, HashTable *oids)
{
	p->type.count = oids ? zend_hash_num_elements(oids) : 0;
	TSRMLS_DF(p);

	if (p->type.oids) {
		efree(p->type.oids);
		p->type.oids = NULL;
	}
	if (p->type.count) {
		Oid *ptr = ecalloc(p->type.count + 1, sizeof(*p->type.oids));
		/* +1 for when less types than params are specified */
		p->type.oids = ptr;
		zend_hash_apply_with_argument(oids, apply_to_oid, &ptr TSRMLS_CC);
	}
	return p->type.count;
}

unsigned php_pq_params_add_type_oid(php_pq_params_t *p, Oid type)
{
	p->type.oids = safe_erealloc(p->type.oids, ++p->type.count, sizeof(*p->type.oids), sizeof(*p->type.oids));
	p->type.oids[p->type.count] = 0;
	p->type.oids[p->type.count-1] = type;
	return p->type.count;
}


static zval *object_param_to_string(php_pq_params_t *p, zval *zobj, Oid type TSRMLS_DC)
{
	zval *return_value = NULL;
	smart_str str = {0};

	switch (type) {
#if PHP_PQ_HAVE_PHP_JSON_H && defined(PHP_PQ_OID_JSON)
#	ifdef PHP_PQ_OID_JSONB
	case PHP_PQ_OID_JSONB:
#	endif
	case PHP_PQ_OID_JSON:
		php_json_encode(&str, zobj, PHP_JSON_UNESCAPED_UNICODE TSRMLS_CC);
		smart_str_0(&str);
		break;
#endif

	case PHP_PQ_OID_DATE:
		php_pqdt_to_string(zobj, "Y-m-d", &str.c, &str.len TSRMLS_CC);
		break;

	case PHP_PQ_OID_ABSTIME:
		php_pqdt_to_string(zobj, "Y-m-d H:i:s", &str.c, &str.len TSRMLS_CC);
		break;

	case PHP_PQ_OID_TIMESTAMP:
		php_pqdt_to_string(zobj, "Y-m-d H:i:s.u", &str.c, &str.len TSRMLS_CC);
		break;

	case PHP_PQ_OID_TIMESTAMPTZ:
		php_pqdt_to_string(zobj, "Y-m-d H:i:s.uO", &str.c, &str.len TSRMLS_CC);
		break;

	default:
		MAKE_STD_ZVAL(return_value);
		MAKE_COPY_ZVAL(&zobj, return_value);
		convert_to_string(return_value);
		break;
	}

	if (str.c) {
		MAKE_STD_ZVAL(return_value);
		RETVAL_STRINGL(str.c, str.len, 0);
	}

	return return_value;
}

struct apply_to_param_from_array_arg {
	php_pq_params_t *params;
	unsigned index;
	smart_str *buffer;
	Oid type;
	zval **zconv;
};

static int apply_to_param_from_array(void *ptr, void *arg_ptr TSRMLS_DC)
{
	struct apply_to_param_from_array_arg subarg, *arg = arg_ptr;
	zval *ztmp, **zparam = ptr, *zcopy = *zparam;
	char *tmp;
	size_t len;
	int tmp_len;

	if (arg->index++) {
		smart_str_appendc(arg->buffer, ',');
	}

	if (arg->zconv) {
		zval *ztype, *rv = NULL;

		MAKE_STD_ZVAL(ztype);
		ZVAL_LONG(ztype, arg->type);
		zend_call_method_with_2_params(arg->zconv, NULL, NULL, "converttostring", &rv, zcopy, ztype);
		zval_ptr_dtor(&ztype);

		if (rv) {
			convert_to_string(rv);
			zcopy = rv;
		} else {
			return ZEND_HASH_APPLY_STOP;
		}

		goto append_string;

	} else {
		switch (Z_TYPE_P(zcopy)) {
		case IS_NULL:
			smart_str_appends(arg->buffer, "NULL");
			break;

		case IS_BOOL:
			smart_str_appends(arg->buffer, Z_BVAL_P(zcopy) ? "t" : "f");
			break;

		case IS_LONG:
			smart_str_append_long(arg->buffer, Z_LVAL_P(zcopy));
			break;

		case IS_DOUBLE:
			len = spprintf(&tmp, 0, "%F", Z_DVAL_P(zcopy));
			smart_str_appendl(arg->buffer, tmp, len);
			efree(tmp);
			break;

		case IS_ARRAY:
			subarg = *arg;
			subarg.index = 0;
			smart_str_appendc(arg->buffer, '{');
			zend_hash_apply_with_argument(Z_ARRVAL_P(zcopy), apply_to_param_from_array, &subarg TSRMLS_CC);
			smart_str_appendc(arg->buffer, '}');
			break;

		case IS_OBJECT:
			if ((ztmp = object_param_to_string(arg->params, zcopy, arg->type TSRMLS_CC))) {
				zcopy = ztmp;
			}
			/* no break */
		default:
			SEPARATE_ZVAL(&zcopy);
			convert_to_string(zcopy);

			append_string:
			tmp = php_addslashes(Z_STRVAL_P(zcopy), Z_STRLEN_P(zcopy), &tmp_len, 0 TSRMLS_CC);
			smart_str_appendc(arg->buffer, '"');
			smart_str_appendl(arg->buffer, tmp, tmp_len);
			smart_str_appendc(arg->buffer, '"');

			if (zcopy != *zparam) {
				zval_ptr_dtor(&zcopy);
			}
			efree(tmp);
			break;
		}
	}
	++arg->index;
	return ZEND_HASH_APPLY_KEEP;
}

static zval *array_param_to_string(php_pq_params_t *p, zval *zarr, Oid type TSRMLS_DC)
{
	zval *zcopy, *return_value;
	smart_str s = {0};
	struct apply_to_param_from_array_arg arg = {NULL};

	switch (type) {
#if PHP_PQ_HAVE_PHP_JSON_H && defined(PHP_PQ_OID_JSON)
#	ifdef PHP_PQ_OID_JSONB
	case PHP_PQ_OID_JSONB:
#	endif
	case PHP_PQ_OID_JSON:
		php_json_encode(&s, zarr, PHP_JSON_UNESCAPED_UNICODE TSRMLS_CC);
		smart_str_0(&s);
		break;
#endif

	default:
		arg.params = p;
		arg.buffer = &s;
		arg.type = PHP_PQ_TYPE_OF_ARRAY(type);
		zend_hash_index_find(&p->type.conv, PHP_PQ_TYPE_OF_ARRAY(type), (void *) &arg.zconv);
		smart_str_appendc(arg.buffer, '{');
		MAKE_STD_ZVAL(zcopy);
		MAKE_COPY_ZVAL(&zarr, zcopy);
		zend_hash_apply_with_argument(Z_ARRVAL_P(zcopy), apply_to_param_from_array, &arg TSRMLS_CC);
		zval_ptr_dtor(&zcopy);
		smart_str_appendc(arg.buffer, '}');
		smart_str_0(&s);
		break;
	}

	/* must not return NULL */
	MAKE_STD_ZVAL(return_value);

	if (s.c) {
		RETVAL_STRINGL(s.c, s.len, 0);
	} else {
		RETVAL_EMPTY_STRING();
	}

	return return_value;
}

static void php_pq_params_set_param(php_pq_params_t *p, unsigned index, zval **zpp)
{
	zval **zconv = NULL;
	Oid type = p->type.count > index ? p->type.oids[index] : 0;
	TSRMLS_DF(p);

	if (type && SUCCESS == zend_hash_index_find(&p->type.conv, type, (void *) &zconv)) {
		zval *ztype, *rv = NULL;

		MAKE_STD_ZVAL(ztype);
		ZVAL_LONG(ztype, type);
		zend_call_method_with_2_params(zconv, NULL, NULL, "converttostring", &rv, *zpp, ztype);
		zval_ptr_dtor(&ztype);
		if (rv) {
			convert_to_string(rv);
			p->param.strings[index] = Z_STRVAL_P(rv);
			zend_hash_next_index_insert(&p->param.dtor, (void *) &rv, sizeof(zval *), NULL);
		}
	} else {
		zval *tmp, *zcopy = *zpp;

		switch (Z_TYPE_P(zcopy)) {
		case IS_NULL:
			p->param.strings[index] = NULL;
			return;

		case IS_BOOL:
			p->param.strings[index] = Z_BVAL_P(zcopy) ? "t" : "f";
			return;

		case IS_DOUBLE:
			MAKE_STD_ZVAL(zcopy);
			MAKE_COPY_ZVAL(zpp, zcopy);
			Z_TYPE_P(zcopy) = IS_STRING;
			Z_STRLEN_P(zcopy) = spprintf(&Z_STRVAL_P(zcopy), 0, "%F", Z_DVAL_PP(zpp));
			break;

		case IS_ARRAY:
			MAKE_STD_ZVAL(zcopy);
			MAKE_COPY_ZVAL(zpp, zcopy);
			tmp = array_param_to_string(p, zcopy, type TSRMLS_CC);
			zval_ptr_dtor(&zcopy);
			zcopy = tmp;
			break;

		case IS_OBJECT:
			if ((tmp = object_param_to_string(p, zcopy, type TSRMLS_CC))) {
				zcopy = tmp;
				break;
			}
			/* no break */

		default:
			convert_to_string_ex(&zcopy);
			break;
		}

		p->param.strings[index] = Z_STRVAL_P(zcopy);

		if (zcopy != *zpp) {
			zend_hash_next_index_insert(&p->param.dtor, (void *) &zcopy, sizeof(zval *), NULL);
		}
	}
}

struct apply_to_params_arg {
	php_pq_params_t *params;
	unsigned index;
};

static int apply_to_params(void *zp, void *arg_ptr TSRMLS_DC)
{
	struct apply_to_params_arg *arg = arg_ptr;

	SEPARATE_ZVAL_IF_NOT_REF((zval **) zp);
	php_pq_params_set_param(arg->params, arg->index++, zp);
	return ZEND_HASH_APPLY_KEEP;
}

unsigned php_pq_params_add_param(php_pq_params_t *p, zval *param)
{
	p->param.strings = safe_erealloc(p->param.strings, ++p->param.count, sizeof(*p->param.strings), 0);
	php_pq_params_set_param(p, p->param.count-1, &param);
	return p->type.count;
}

unsigned php_pq_params_set_params(php_pq_params_t *p, HashTable *params)
{
	p->param.count = params ? zend_hash_num_elements(params) : 0;
	TSRMLS_DF(p);

	if (p->param.strings) {
		efree(p->param.strings);
		p->param.strings = NULL;
	}
	zend_hash_clean(&p->param.dtor);
	if (p->param.count) {
		struct apply_to_params_arg arg = {p, 0};
		p->param.strings = ecalloc(p->param.count, sizeof(*p->param.strings));
		zend_hash_apply_with_argument(params, apply_to_params, &arg TSRMLS_CC);
	}
	return p->param.count;
}

void php_pq_params_free(php_pq_params_t **p)
{
	if (*p) {
		php_pq_params_set_type_oids(*p, NULL);
		php_pq_params_set_params(*p, NULL);

		zend_hash_destroy(&(*p)->param.dtor);
		zend_hash_destroy(&(*p)->type.conv);

		efree(*p);
		*p = NULL;
	}
}

php_pq_params_t *php_pq_params_init(HashTable *conv, HashTable *oids, HashTable *params TSRMLS_DC)
{
	php_pq_params_t *p = ecalloc(1, sizeof(*p));

	TSRMLS_CF(p);
	zend_hash_init(&p->type.conv, 0, NULL, ZVAL_PTR_DTOR, 0);
	zend_hash_init(&p->param.dtor, 0, NULL, ZVAL_PTR_DTOR, 0);

	if (conv) {
		php_pq_params_set_type_conv(p, conv);
	}
	if (oids) {
		php_pq_params_set_type_oids(p, oids);
	}
	if (params) {
		php_pq_params_set_params(p, params);
	}

	return p;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
