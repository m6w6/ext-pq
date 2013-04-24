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
#include <ext/date/php_date.h>
#if defined(HAVE_JSON) && !defined(COMPILE_DL_JSON)
#	include <ext/json/php_json.h>
#endif

#include <Zend/zend_interfaces.h>

#include <libpq/libpq-fs.h>

#include "php_pq.h"
#include "php_pq_misc.h"

char *rtrim(char *e)
{
	size_t l = strlen(e);

	while (l-- > 0 && e[l] == '\n') {
		e[l] = '\0';
	}
	return e;
}

const char *strmode(long mode)
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

int compare_index(const void *lptr, const void *rptr TSRMLS_DC)
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

	switch (Z_TYPE_PP(zparam)) {
	case IS_NULL:
		**params = NULL;
		++*params;
		break;

	case IS_BOOL:
		**params = Z_BVAL_PP(zparam) ? "t" : "f";
		++*params;
		break;

	case IS_DOUBLE:
		SEPARATE_ZVAL(zparam);
		Z_TYPE_PP(zparam) = IS_STRING;
		Z_STRLEN_PP(zparam) = spprintf(&Z_STRVAL_PP(zparam), 0, "%F", Z_DVAL_PP((zval **)p));
		/* no break */

	default:
		convert_to_string_ex(zparam);
		/* no break */

	case IS_STRING:
		**params = Z_STRVAL_PP(zparam);
		++*params;

		if (*zparam != *(zval **)p) {
			zend_hash_next_index_insert(zdtor, zparam, sizeof(zval *), NULL);
		}
		break;
	}

	return ZEND_HASH_APPLY_KEEP;
}

int php_pq_types_to_array(HashTable *ht, Oid **types TSRMLS_DC)
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

int php_pq_params_to_array(HashTable *ht, char ***params, HashTable *zdtor TSRMLS_DC)
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
Oid *php_pq_ntypes_to_array(zend_bool fill, int argc, ...)
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

zend_class_entry *php_pqdt_class_entry;

ZEND_BEGIN_ARG_INFO_EX(ai_pqdt_to_string, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqdt, __toString)
{
	zval *rv;

	zend_call_method_with_1_params(&getThis(), php_pqdt_class_entry, NULL, "format", &rv,
			zend_read_property(php_pqdt_class_entry, getThis(), ZEND_STRL("format"), 0 TSRMLS_CC));
	RETVAL_ZVAL(rv, 1, 1);
}

static zend_function_entry php_pqdt_methods[] = {
	PHP_ME(pqdt, __toString, ai_pqdt_to_string, ZEND_ACC_PUBLIC)
	PHP_MALIAS(pqdt, jsonSerialize, __toString, ai_pqdt_to_string, ZEND_ACC_PUBLIC)
	{0}
};

zval *php_pqdt_from_string(char *dt_str, size_t dt_len, char *fmt, zval *zv TSRMLS_DC)
{
	php_date_obj *dobj;

	if (!zv) {
		MAKE_STD_ZVAL(zv);
	}

	php_date_instantiate(php_pqdt_class_entry, zv TSRMLS_CC);
	dobj = zend_object_store_get_object(zv TSRMLS_CC);
	if (!php_date_initialize(dobj, dt_str, dt_len, NULL, NULL, 1 TSRMLS_CC)) {
		zval_dtor(zv);
		ZVAL_NULL(zv);
	} else if (fmt) {
		zend_update_property_string(php_pqdt_class_entry, zv, ZEND_STRL("format"), fmt TSRMLS_CC);
	}

	return zv;
}

PHP_MINIT_FUNCTION(pq_misc)
{
	zend_class_entry **json, ce = {0};

	INIT_NS_CLASS_ENTRY(ce ,"pq", "DateTime", php_pqdt_methods);
	php_pqdt_class_entry = zend_register_internal_class_ex(&ce, php_date_get_date_ce(), "DateTime" TSRMLS_CC);

	zend_declare_property_stringl(php_pqdt_class_entry, ZEND_STRL("format"), ZEND_STRL("Y-m-d H:i:s.u"), ZEND_ACC_PUBLIC TSRMLS_CC);

	/* stop reading this file right here! */
	if (SUCCESS == zend_hash_find(CG(class_table), ZEND_STRS("jsonserializable"), (void *) &json)) {
		zend_class_implements(php_pqdt_class_entry TSRMLS_CC, 1, *json);
	}

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
