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

#include <Zend/zend_interfaces.h>

#include <libpq-fe.h>

#include "php_pq.h"
#include "php_pq_params.h"

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

static int apply_to_param_from_array(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	zval **zparam = p;
	unsigned j, *i = va_arg(argv, unsigned *);
	smart_str *s = va_arg(argv, smart_str *);
	zval **zconv = va_arg(argv, zval **);
	char *tmp;
	size_t len;
	int tmp_len;

	if ((*i)++) {
		smart_str_appendc(s, ',');
	}

	if (zconv) {
		zval *rv = NULL;

		zend_call_method_with_1_params(zconv, NULL, NULL, "converttostring", &rv, *zparam);
		convert_to_string(rv);
		smart_str_appendl(s, Z_STRVAL_P(rv), Z_STRLEN_P(rv));
		zval_ptr_dtor(&rv);
	} else {
		switch (Z_TYPE_PP(zparam)) {
		case IS_NULL:
			smart_str_appends(s, "NULL");
			break;

		case IS_BOOL:
			smart_str_appends(s, Z_BVAL_PP(zparam) ? "t" : "f");
			break;

		case IS_LONG:
			smart_str_append_long(s, Z_LVAL_PP(zparam));
			break;

		case IS_DOUBLE:
			len = spprintf(&tmp, 0, "%F", Z_DVAL_PP(zparam));
			smart_str_appendl(s, tmp, len);
			efree(tmp);
			break;

		case IS_ARRAY:
			j = 0;
			smart_str_appendc(s, '{');
			zend_hash_apply_with_arguments(Z_ARRVAL_PP(zparam) TSRMLS_CC, apply_to_param_from_array, 2, &j, s, zconv);
			smart_str_appendc(s, '}');
			break;

		default:
			SEPARATE_ZVAL(zparam);
			if (Z_TYPE_PP(zparam) != IS_STRING) {
				convert_to_string(*zparam);
			}

			tmp = php_addslashes(Z_STRVAL_PP(zparam), Z_STRLEN_PP(zparam), &tmp_len, 0 TSRMLS_CC);
			smart_str_appendc(s, '"');
			smart_str_appendl(s, tmp, tmp_len);
			smart_str_appendc(s, '"');

			if (*zparam != *((zval **) p)) {
				zval_ptr_dtor(zparam);
			}
			break;
		}
	}
	++(*i);
	return ZEND_HASH_APPLY_KEEP;
}

static void array_param_to_string(zval **zconv, HashTable *ht, char **str, int *len TSRMLS_DC)
{
	smart_str s = {0};
	unsigned i = 0;

	smart_str_appendc(&s, '{');
	zend_hash_apply_with_arguments(ht TSRMLS_CC, apply_to_param_from_array, 3, &i, &s, zconv);
	smart_str_appendc(&s, '}');

	smart_str_0(&s);
	*str = s.c;
	*len = s.len;
}

static void php_pq_params_set_param(php_pq_params_t *p, unsigned index, zval **zp)
{
	zval **zconv = NULL;
	Oid type = p->type.count > index ? p->type.oids[index] : 0;
	TSRMLS_DF(p);

	if (type && SUCCESS == zend_hash_index_find(&p->type.conv, type, (void *) &zconv)) {
		zval *rv = NULL;

		zend_call_method_with_1_params(zconv, NULL, NULL, "converttostring", &rv, *zp);
		convert_to_string(rv);
		p->param.strings[index] = Z_STRVAL_P(rv);
		zend_hash_next_index_insert(&p->param.dtor, (void *) &rv, sizeof(zval *), NULL);
	} else {
		zval **zpp = zp;

		switch (Z_TYPE_PP(zp)) {
		case IS_NULL:
			p->param.strings[index] = NULL;
			return;

		case IS_BOOL:
			p->param.strings[index] = Z_BVAL_PP(zp) ? "t" : "f";
			return;

		case IS_DOUBLE:
			SEPARATE_ZVAL(zp);
			Z_TYPE_PP(zp) = IS_STRING;
			Z_STRLEN_PP(zp) = spprintf(&Z_STRVAL_PP(zp), 0, "%F", Z_DVAL_PP((zval **)p));
			break;

		case IS_ARRAY:
		{

#if HAVE_PHP_PQ_TYPE_H
#	undef PHP_PQ_TYPE
#	include "php_pq_type.h"
#else
#	define PHP_PQ_TYPE_OF_ARRAY(oid) 0
#endif

			zval *tmp;
			MAKE_STD_ZVAL(tmp);
			Z_TYPE_P(tmp) = IS_STRING;
			zend_hash_index_find(&p->type.conv, PHP_PQ_TYPE_OF_ARRAY(type), (void *) &zconv);
			array_param_to_string(zconv, Z_ARRVAL_PP(zp), &Z_STRVAL_P(tmp), &Z_STRLEN_P(tmp) TSRMLS_CC);
			zp = &tmp;
			break;
		}

		default:
			convert_to_string_ex(zp);
			break;
		}

		p->param.strings[index] = Z_STRVAL_PP(zp);

		if (*zp != *zpp) {
			zend_hash_next_index_insert(&p->param.dtor, zp, sizeof(zval *), NULL);
		}
	}
}

static int apply_to_params(void *zp TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	php_pq_params_t *p = (php_pq_params_t *) va_arg(argv, php_pq_params_t *);
	unsigned *index = (unsigned *) va_arg(argv, unsigned *);

	php_pq_params_set_param(p, (*index)++, zp);
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
		unsigned index = 0;
		p->param.strings = ecalloc(p->param.count, sizeof(*p->param.strings));
		zend_hash_apply_with_arguments(params TSRMLS_CC, apply_to_params, 2, p, &index);
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
