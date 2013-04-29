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
#include <ext/standard/php_string.h>
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

typedef struct _HashTableList {
	HashTable ht;
	struct _HashTableList *parent;
} HashTableList;

typedef struct _ArrayParserState {
	const char *ptr, *end;
	HashTableList *list;
#ifdef ZTS
	void ***ts;
#endif
	unsigned quotes:1;
	unsigned escaped:1;
} ArrayParserState;

static char caa(ArrayParserState *a, const char *any, unsigned advance)
{
	const char *p = any;
	TSRMLS_FETCH_FROM_CTX(a->ts);

	do {
		if (*p == *a->ptr) {
			a->ptr += advance;
			return *p;
		}
	} while (*++p);

	php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to parse array: expected one of '%s', got '%c'", any, *a->ptr); \
	return 0;
}

static STATUS add_element(ArrayParserState *a, const char *start)
{
	zval *zelem;
	TSRMLS_FETCH_FROM_CTX(a->ts);

	MAKE_STD_ZVAL(zelem);
	if (a->quotes) {
		ZVAL_STRINGL(zelem, start, a->ptr - start, 1);
		php_stripslashes(Z_STRVAL_P(zelem), &Z_STRLEN_P(zelem) TSRMLS_CC);
	} else if ((a->ptr - start == 4) && !strncmp(start, "NULL", 4)) {
		ZVAL_NULL(zelem);
	} else {
		long lval = 0;
		double dval = 0;

		switch (is_numeric_string(start, a->ptr - start, &lval, &dval, 0)) {
		case IS_LONG:
			ZVAL_LONG(zelem, lval);
			break;

		case IS_DOUBLE:
			ZVAL_DOUBLE(zelem, dval);
			break;

		default:
			ZVAL_STRINGL(zelem, start, a->ptr - start, 1);
			break;
		}
	}

	return zend_hash_next_index_insert(&a->list->ht, &zelem, sizeof(zval *), NULL);
}

static STATUS parse_array(ArrayParserState *a);

static STATUS parse_element(ArrayParserState *a)
{
	const char *el;
	TSRMLS_FETCH_FROM_CTX(a->ts);

	switch (*a->ptr) {
	case '{':
		return parse_array(a);

	case '"':
		a->quotes = 1;
		++a->ptr;
		break;
	}

	for (el = a->ptr; a->ptr < a->end; ++a->ptr) {
		switch (*a->ptr) {
		case '"':
			if (a->escaped) {
				a->escaped = 0;
			} else if (a->quotes) {
				if (SUCCESS != add_element(a, el)) {
					return FAILURE;
				}
				a->quotes = 0;
				++a->ptr;
				return SUCCESS;
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to parse element, unexpected quote: '%.*s'", (int) (a->ptr - el), el);
				return FAILURE;
			}
			break;

		case ',':
		case '}':
			if (!a->quotes) {
				return add_element(a, el);
			}
			break;

		case '\\':
			a->escaped = !a->escaped;
			break;

		default:
			a->escaped = 0;
			break;
		}
	}

	php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to parse element, reached end of input");
	return FAILURE;
}

static STATUS parse_elements(ArrayParserState *a)
{
	TSRMLS_FETCH_FROM_CTX(a->ts);

	while (SUCCESS == parse_element(a)) {
		switch (caa(a, ",}", 0)) {
		case 0:
			return FAILURE;

		case '}':
			return SUCCESS;

		default:
			if (!*++a->ptr) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to parse elements, reached end of input");
				return FAILURE;
			}
			break;
		}
	}

	return FAILURE;
}

static STATUS parse_array(ArrayParserState *a)
{
	HashTableList *list;

	if (!caa(a, "{", 1)) {
		return FAILURE;
	}

	list = ecalloc(1, sizeof(*list));
	ZEND_INIT_SYMTABLE(&list->ht);

	if (a->list) {
		zval *zcur;

		MAKE_STD_ZVAL(zcur);
		Z_TYPE_P(zcur) = IS_ARRAY;
		Z_ARRVAL_P(zcur) = &list->ht;

		zend_hash_next_index_insert(&a->list->ht, &zcur, sizeof(zval *), NULL);

		list->parent = a->list;
	}
	a->list = list;

	if (SUCCESS != parse_elements(a)) {
		return FAILURE;
	}

	if (!caa(a, "}", 1)) {
		return FAILURE;
	}

	if (a->list->parent) {
		a->list = a->list->parent;
	}

	return SUCCESS;
}

HashTable *php_pq_parse_array(const char *val_str, size_t val_len TSRMLS_DC)
{
	HashTable *ht = NULL;
	ArrayParserState a = {0};
	TSRMLS_SET_CTX(a.ts);

	a.ptr = val_str;
	a.end = val_str + val_len;

	if (SUCCESS != parse_array(&a)) {
		while (a.list) {
			HashTableList *l = a.list->parent;

			zend_hash_destroy(&a.list->ht);
			efree(a.list);
			a.list = l;
		}
		return ht;
	}

	if (*a.ptr) {
		php_error_docref(NULL TSRMLS_CC, E_NOTICE, "Trailing input: '%s'", a.ptr);
	}

	do {
		ht = &a.list->ht;
	} while ((a.list = a.list->parent));

	return ht;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
