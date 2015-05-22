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

#include <Zend/zend_interfaces.h>

#include <libpq/libpq-fs.h>

#include "php_pq.h"
#include "php_pqexc.h"
#include "php_pq_misc.h"
#undef PHP_PQ_TYPE
#include "php_pq_type.h"

char *php_pq_rtrim(char *e)
{
	size_t l = strlen(e);

	while (l-- > 0 && e[l] == '\n') {
		e[l] = '\0';
	}
	return e;
}

const char *php_pq_strmode(long mode)
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

int php_pq_compare_index(const void *lptr, const void *rptr TSRMLS_DC)
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

zend_class_entry *php_pqdt_class_entry;

ZEND_BEGIN_ARG_INFO_EX(ai_pqdt_to_string, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqdt, __toString)
{
	zval *rv = NULL;

	zend_call_method_with_1_params(&getThis(), php_pqdt_class_entry, NULL, "format", &rv,
			zend_read_property(php_pqdt_class_entry, getThis(), ZEND_STRL("format"), 0 TSRMLS_CC));
	if (rv) {
		RETVAL_ZVAL(rv, 1, 1);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqdt_create_from_format, 0, 0, 2)
	ZEND_ARG_INFO(0, format)
	ZEND_ARG_INFO(0, datetime)
	ZEND_ARG_INFO(0, timezone)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqdt, createFromFormat)
{
	zend_error_handling zeh;
	char *fmt_str, *dt_str;
	int fmt_len, dt_len;
	zval *ztz = NULL;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|O", &fmt_str, &fmt_len, &dt_str, &dt_len, &ztz, php_date_get_timezone_ce());
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqdt_from_string(return_value, fmt_str, dt_str, dt_len, "Y-m-d H:i:s.uO", ztz TSRMLS_CC);
	}
}

static zend_function_entry php_pqdt_methods[] = {
	PHP_ME(pqdt, createFromFormat, ai_pqdt_create_from_format, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	PHP_ME(pqdt, __toString, ai_pqdt_to_string, ZEND_ACC_PUBLIC)
	PHP_MALIAS(pqdt, jsonSerialize, __toString, ai_pqdt_to_string, ZEND_ACC_PUBLIC)
	{0}
};

zval *php_pqdt_from_string(zval *zv, char *input_fmt, char *dt_str, size_t dt_len, char *output_fmt, zval *ztimezone TSRMLS_DC)
{
	php_date_obj *dobj;

	if (!zv) {
		MAKE_STD_ZVAL(zv);
	}

	php_date_instantiate(php_pqdt_class_entry, zv TSRMLS_CC);
	dobj = zend_object_store_get_object(zv TSRMLS_CC);
	if (!php_date_initialize(dobj, dt_str, dt_len, input_fmt, ztimezone, 1 TSRMLS_CC)) {
		zval_dtor(zv);
		ZVAL_NULL(zv);
	} else if (output_fmt) {
		zend_update_property_string(php_pqdt_class_entry, zv, ZEND_STRL("format"), output_fmt TSRMLS_CC);
	}

	return zv;
}

void php_pqdt_to_string(zval *zdt, const char *format, char **str_buf, size_t *str_len TSRMLS_DC)
{
	zval rv;

	INIT_PZVAL(&rv);
	ZVAL_NULL(&rv);

	if (Z_OBJ_HT_P(zdt)->cast_object
	&&	SUCCESS == Z_OBJ_HT_P(zdt)->cast_object(zdt, &rv, IS_STRING TSRMLS_CC)
	) {
		*str_len = Z_STRLEN(rv);
		*str_buf = Z_STRVAL(rv);
	} else if (instanceof_function(Z_OBJCE_P(zdt), php_date_get_date_ce() TSRMLS_CC)) {
		zval *rv = NULL, *zfmt;

		MAKE_STD_ZVAL(zfmt);
		ZVAL_STRING(zfmt, format, 1);
		zend_call_method_with_1_params(&zdt, Z_OBJCE_P(zdt), NULL, "format", &rv, zfmt);
		zval_ptr_dtor(&zfmt);

		if (rv) {
			if (Z_TYPE_P(rv) == IS_STRING) {
				*str_len = Z_STRLEN_P(rv);
				*str_buf = estrndup(Z_STRVAL_P(rv), *str_len);
			}
			zval_ptr_dtor(&rv);
		}
	}
}

zend_class_entry *php_pqconv_class_entry;

ZEND_BEGIN_ARG_INFO_EX(ai_pqconv_convert_types, 0, 0, 0)
ZEND_END_ARG_INFO();

ZEND_BEGIN_ARG_INFO_EX(ai_pqconv_convert_from_string, 0, 0, 2)
	ZEND_ARG_INFO(0, data)
	ZEND_ARG_INFO(0, type)
ZEND_END_ARG_INFO();

ZEND_BEGIN_ARG_INFO_EX(ai_pqconv_convert_to_string, 0, 0, 2)
	ZEND_ARG_INFO(0, data)
	ZEND_ARG_INFO(0, type)
ZEND_END_ARG_INFO();

zend_function_entry php_pqconv_methods[] = {
	PHP_ABSTRACT_ME(pqconv, convertTypes, ai_pqconv_convert_types)
	PHP_ABSTRACT_ME(pqconv, convertFromString, ai_pqconv_convert_from_string)
	PHP_ABSTRACT_ME(pqconv, convertToString, ai_pqconv_convert_to_string)
	{0}
};


PHP_MINIT_FUNCTION(pq_misc)
{
	zend_class_entry **json, ce = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Converter", php_pqconv_methods);
	php_pqconv_class_entry = zend_register_internal_interface(&ce TSRMLS_CC);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce ,"pq", "DateTime", php_pqdt_methods);
	php_pqdt_class_entry = zend_register_internal_class_ex(&ce, php_date_get_date_ce(), "DateTime" TSRMLS_CC);

	zend_declare_property_stringl(php_pqdt_class_entry, ZEND_STRL("format"), ZEND_STRL("Y-m-d H:i:s.uO"), ZEND_ACC_PUBLIC TSRMLS_CC);

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
	php_pqres_t *res;
#ifdef ZTS
	void ***ts;
#endif
	Oid typ;
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

static ZEND_RESULT_CODE add_element(ArrayParserState *a, const char *start)
{
	zval *zelem;
	size_t el_len = a->ptr - start;
	char *el_str = estrndup(start, el_len);
	TSRMLS_FETCH_FROM_CTX(a->ts);

	if (a->quotes) {
		int tmp_len = el_len;

		php_stripslashes(el_str, &tmp_len TSRMLS_CC);
		el_len = tmp_len;
	} else if ((a->ptr - start == 4) && !strncmp(start, "NULL", 4)) {
		efree(el_str);
		el_str = NULL;
		el_len = 0;
	}

	if (!el_str) {
		MAKE_STD_ZVAL(zelem);
		ZVAL_NULL(zelem);
	} else {
		zelem = php_pqres_typed_zval(a->res, el_str, el_len, a->typ TSRMLS_CC);

		efree(el_str);
	}

	return zend_hash_next_index_insert(&a->list->ht, &zelem, sizeof(zval *), NULL);
}

static ZEND_RESULT_CODE parse_array(ArrayParserState *a);

static ZEND_RESULT_CODE parse_element(ArrayParserState *a, char delim)
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
		case '\\':
			a->escaped = !a->escaped;
			break;

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

		default:
			if (delim != *a->ptr) {
				a->escaped = 0;
				break;
			}
			/* no break */
		case '}':
			if (!a->quotes) {
				return add_element(a, el);
			}
			break;

		}
	}

	php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to parse element, reached end of input");
	return FAILURE;
}

static ZEND_RESULT_CODE parse_elements(ArrayParserState *a)
{
	char delims[] = {'}', PHP_PQ_DELIM_OF_ARRAY(a->typ), 0};
	TSRMLS_FETCH_FROM_CTX(a->ts);

	while (SUCCESS == parse_element(a, delims[1])) {
		switch (caa(a, delims, 0)) {
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

static ZEND_RESULT_CODE parse_array(ArrayParserState *a)
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

HashTable *php_pq_parse_array(php_pqres_t *res, const char *val_str, size_t val_len, Oid typ TSRMLS_DC)
{
	HashTable *ht = NULL;
	ArrayParserState a = {0};
	TSRMLS_SET_CTX(a.ts);

	a.typ = typ;
	a.ptr = val_str;
	a.end = val_str + val_len;
	a.res = res;

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
