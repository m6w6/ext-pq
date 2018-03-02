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
#if PHP_PQ_HAVE_PHP_JSON_H
#include <php_json.h> /* we've added the include directory to INCLUDES */
#endif

#include <Zend/zend_smart_str.h>
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
	zend_hash_copy(&p->type.conv, conv, (copy_ctor_func_t) zval_add_ref);
}

static int apply_to_oid(zval *ztype, void *arg)
{
	Oid **types = arg;

	**types = zval_get_long(ztype);
	++*types;

	return ZEND_HASH_APPLY_KEEP;
}

unsigned php_pq_params_set_type_oids(php_pq_params_t *p, HashTable *oids)
{
	p->type.count = oids ? zend_hash_num_elements(oids) : 0;

	if (p->type.oids) {
		efree(p->type.oids);
		p->type.oids = NULL;
	}
	if (p->type.count) {
		Oid *ptr = ecalloc(p->type.count + 1, sizeof(*p->type.oids));
		/* +1 for when less types than params are specified */
		p->type.oids = ptr;
		zend_hash_apply_with_argument(oids, apply_to_oid, &ptr);
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


static zend_string *object_param_to_string(php_pq_params_t *p, zval *zobj, Oid type)
{
#if PHP_PQ_HAVE_PHP_JSON_H && defined(PHP_PQ_OID_JSON)
	smart_str str = {0};
#endif

	switch (type) {
#if PHP_PQ_HAVE_PHP_JSON_H && defined(PHP_PQ_OID_JSON)
#	ifdef PHP_PQ_OID_JSONB
	case PHP_PQ_OID_JSONB:
#	endif
	case PHP_PQ_OID_JSON:
#	if PHP_VERSION_ID >= 70100
		JSON_G(encode_max_depth) = PHP_JSON_PARSER_DEFAULT_DEPTH;
#	endif
		php_json_encode(&str, zobj, PHP_JSON_UNESCAPED_UNICODE);
		smart_str_0(&str);
		return str.s;
#endif

	case PHP_PQ_OID_DATE:
		return php_pqdt_to_string(zobj, "Y-m-d");

	case PHP_PQ_OID_ABSTIME:
		return php_pqdt_to_string(zobj, "Y-m-d H:i:s");

	case PHP_PQ_OID_TIMESTAMP:
		return php_pqdt_to_string(zobj, "Y-m-d H:i:s.u");

	case PHP_PQ_OID_TIMESTAMPTZ:
		return php_pqdt_to_string(zobj, "Y-m-d H:i:s.uO");
	}

	return zval_get_string(zobj);
}

struct apply_to_param_from_array_arg {
	php_pq_params_t *params;
	unsigned index;
	smart_str *buffer;
	Oid type;
	char delim;
	zval *zconv;
};

static int apply_to_param_from_array(zval *zparam, void *arg_ptr)
{
	struct apply_to_param_from_array_arg subarg, *arg = arg_ptr;
	char *tmp;
	size_t len;
	zend_string *str;

	if (arg->index++) {
		smart_str_appendc(arg->buffer, arg->delim);
	}

	if (arg->zconv) {
		zval ztype, rv;

		ZVAL_LONG(&ztype, arg->type);
		zend_call_method_with_2_params(arg->zconv, NULL, NULL, "converttostring", &rv, zparam, &ztype);
		str = zval_get_string(&rv);
		zval_ptr_dtor(&rv);
		goto append_string;

	} else {
		again:
		switch (Z_TYPE_P(zparam)) {
		case IS_REFERENCE:
			ZVAL_DEREF(zparam);
			goto again;

		case IS_NULL:
			smart_str_appends(arg->buffer, "NULL");
			break;

		case IS_TRUE:
			smart_str_appends(arg->buffer, "t");
			break;

		case IS_FALSE:
			smart_str_appends(arg->buffer, "f");
			break;

		case IS_LONG:
			smart_str_append_long(arg->buffer, Z_LVAL_P(zparam));
			break;

		case IS_DOUBLE:
			len = spprintf(&tmp, 0, "%F", Z_DVAL_P(zparam));
			smart_str_appendl(arg->buffer, tmp, len);
			efree(tmp);
			break;

		case IS_ARRAY:
			subarg = *arg;
			subarg.index = 0;
			smart_str_appendc(arg->buffer, '{');
			zend_hash_apply_with_argument(Z_ARRVAL_P(zparam), apply_to_param_from_array, &subarg);
			smart_str_appendc(arg->buffer, '}');
			break;

		case IS_OBJECT:
			if ((str = object_param_to_string(arg->params, zparam, arg->type))) {
				goto append_string;
			}
			/* no break */
		default:
			str = zval_get_string(zparam);

			append_string:
			str = php_addslashes(str, 1);
			smart_str_appendc(arg->buffer, '"');
			smart_str_appendl(arg->buffer, str->val, str->len);
			smart_str_appendc(arg->buffer, '"');
			zend_string_release(str);
			break;
		}
	}
	++arg->index;
	return ZEND_HASH_APPLY_KEEP;
}

static zend_string *array_param_to_string(php_pq_params_t *p, zval *zarr, Oid type)
{
	smart_str s = {0};
	struct apply_to_param_from_array_arg arg = {NULL};

	switch (type) {
#if PHP_PQ_HAVE_PHP_JSON_H && defined(PHP_PQ_OID_JSON)
#	ifdef PHP_PQ_OID_JSONB
	case PHP_PQ_OID_JSONB:
#	endif
	case PHP_PQ_OID_JSON:
		php_json_encode(&s, zarr, PHP_JSON_UNESCAPED_UNICODE);
		break;
#endif

	default:
		arg.params = p;
		arg.buffer = &s;
		arg.type = PHP_PQ_TYPE_OF_ARRAY(type);
		arg.delim = PHP_PQ_DELIM_OF_ARRAY(type);
		arg.zconv = zend_hash_index_find(&p->type.conv, PHP_PQ_TYPE_OF_ARRAY(type));
		smart_str_appendc(arg.buffer, '{');
		SEPARATE_ZVAL(zarr);
		zend_hash_apply_with_argument(Z_ARRVAL_P(zarr), apply_to_param_from_array, &arg);
		smart_str_appendc(arg.buffer, '}');
		break;
	}

	smart_str_0(&s);
	return s.s;
}

static void php_pq_params_set_param(php_pq_params_t *p, unsigned index, zval *zpp)
{
	zval *zconv = NULL;
	Oid type = p->type.count > index ? p->type.oids[index] : 0;

	if (type && (zconv = zend_hash_index_find(&p->type.conv, type))) {
		zval ztype, rv;

		ZVAL_NULL(&rv);
		ZVAL_LONG(&ztype, type);
		zend_call_method_with_2_params(zconv, NULL, NULL, "converttostring", &rv, zpp, &ztype);
		convert_to_string(&rv);
		p->param.strings[index] = Z_STRVAL_P(&rv);
		zend_hash_next_index_insert(&p->param.dtor, &rv);
	} else {
		zval tmp;
		zend_string *str = NULL;
		char tmp_str[64];
		size_t tmp_len = 0;

		again:
		switch (Z_TYPE_P(zpp)) {
		case IS_REFERENCE:
			ZVAL_DEREF(zpp);
			goto again;

		case IS_NULL:
			p->param.strings[index] = NULL;
			return;

		case IS_TRUE:
			p->param.strings[index] = "t";
			break;

		case IS_FALSE:
			p->param.strings[index] = "f";
			return;

		case IS_DOUBLE:
			tmp_len = slprintf(tmp_str, sizeof(tmp_str), "%F", Z_DVAL_P(zpp));
			str = zend_string_init(tmp_str, tmp_len, 0);
			break;

		case IS_ARRAY:
			str = array_param_to_string(p, zpp, type);
			break;

		case IS_OBJECT:
			if ((str = object_param_to_string(p, zpp, type))) {
				break;
			}
			/* no break */
		default:
			str = zval_get_string(zpp);
			break;
		}

		if (str) {
			ZVAL_STR(&tmp, str);
			p->param.strings[index] = Z_STRVAL(tmp);
			zend_hash_next_index_insert(&p->param.dtor, &tmp);
		}
	}
}

struct apply_to_params_arg {
	php_pq_params_t *params;
	unsigned index;
};

static int apply_to_params(zval *zp, void *arg_ptr)
{
	struct apply_to_params_arg *arg = arg_ptr;

	SEPARATE_ZVAL_IF_NOT_REF(zp);
	php_pq_params_set_param(arg->params, arg->index++, zp);
	return ZEND_HASH_APPLY_KEEP;
}

unsigned php_pq_params_add_param(php_pq_params_t *p, zval *param)
{
	p->param.strings = safe_erealloc(p->param.strings, ++p->param.count, sizeof(*p->param.strings), 0);
	php_pq_params_set_param(p, p->param.count-1, param);
	return p->type.count;
}

unsigned php_pq_params_set_params(php_pq_params_t *p, HashTable *params)
{
	p->param.count = params ? zend_hash_num_elements(params) : 0;

	if (p->param.strings) {
		efree(p->param.strings);
		p->param.strings = NULL;
	}
	zend_hash_clean(&p->param.dtor);
	if (p->param.count) {
		struct apply_to_params_arg arg = {p, 0};
		p->param.strings = ecalloc(p->param.count, sizeof(*p->param.strings));
		zend_hash_apply_with_argument(params, apply_to_params, &arg);
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

php_pq_params_t *php_pq_params_init(HashTable *conv, HashTable *oids, HashTable *params)
{
	php_pq_params_t *p = ecalloc(1, sizeof(*p));

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
