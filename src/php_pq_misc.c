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
#include "php_pqconn_event.h"
#undef PHP_PQ_TYPE
#include "php_pq_type.h"


/* clear result object associated with a result handle */
void php_pqres_clear(PGresult *r) {
	php_pq_object_t *o = PQresultInstanceData(r, php_pqconn_event);

	if (o) {
		php_pq_object_delref(o);
	} else {
		PQclear(r);
	}
}

/* clear any asynchronous results */
void php_pqconn_clear(PGconn *conn) {
	PGresult *r;
	php_pqconn_event_data_t *evdata = PQinstanceData(conn, php_pqconn_event);

	while ((r = PQgetResult(conn))) {
		php_pqres_clear(r);
	}

	if (evdata && evdata->obj) {
		if (php_pq_callback_is_enabled(&evdata->obj->intern->onevent)) {
			if (php_pq_callback_is_locked(&evdata->obj->intern->onevent)) {
				php_pq_callback_disable(&evdata->obj->intern->onevent);
			} else {
				php_pq_callback_dtor(&evdata->obj->intern->onevent);
			}
		}
	}
}

/* safe wrappers to clear any asynchronous wrappers before querying synchronously */
PGresult *php_pq_exec(PGconn *conn, const char *query) {
	php_pqconn_clear(conn);
	return PQexec(conn, query);
}
PGresult *php_pq_exec_params(PGconn *conn, const char *command, int nParams, const Oid *paramTypes, const char *const * paramValues, const int *paramLengths, const int *paramFormats, int resultFormat) {
	php_pqconn_clear(conn);
	return PQexecParams(conn, command, nParams, paramTypes, paramValues, paramLengths, paramFormats, resultFormat);
}
PGresult *php_pq_prepare(PGconn *conn, const char *stmtName, const char *query, int nParams, const Oid *paramTypes) {
	php_pqconn_clear(conn);
	return PQprepare(conn, stmtName, query, nParams, paramTypes);
}
PGresult *php_pq_exec_prepared(PGconn *conn, const char *stmtName, int nParams, const char *const * paramValues, const int *paramLengths, const int *paramFormats, int resultFormat) {
	php_pqconn_clear(conn);
	return PQexecPrepared(conn, stmtName, nParams, paramValues, paramLengths, paramFormats, resultFormat);
}

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

int php_pq_compare_index(const void *lptr, const void *rptr)
{
	zend_ulong l = ((const Bucket *) lptr)->h;
	zend_ulong r = ((const Bucket *) rptr)->h;

	if (l < r) {
		return -1;
	}
	if (l > r) {
		return 1;
	}
	return 0;
}

void php_pq_hash_ptr_dtor(zval *p)
{
	efree(Z_PTR_P(p));
}

zend_class_entry *php_pqdt_class_entry;

ZEND_BEGIN_ARG_INFO_EX(ai_pqdt_to_string, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqdt, __toString)
{
	zval rv, tmp;

	ZVAL_NULL(&rv);
	zend_call_method_with_1_params(getThis(), php_pqdt_class_entry, NULL, "format", &rv,
			zend_read_property(php_pqdt_class_entry, getThis(), ZEND_STRL("format"), 0, &tmp));
	RETVAL_ZVAL(&rv, 1, 1);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqdt_create_from_format, 0, 0, 2)
	ZEND_ARG_INFO(0, format)
	ZEND_ARG_INFO(0, datetime)
#if PHP_VERSION_ID >= 70200
	ZEND_ARG_OBJ_INFO(0, object, DateTimeZone, 1)
#else
	ZEND_ARG_INFO(0, timezone)
#endif
ZEND_END_ARG_INFO();
static PHP_METHOD(pqdt, createFromFormat)
{
	zend_error_handling zeh;
	char *fmt_str, *dt_str;
	size_t fmt_len, dt_len;
	zval *ztz = NULL;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "ss|O", &fmt_str, &fmt_len, &dt_str, &dt_len, &ztz, php_date_get_timezone_ce());
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqdt_from_string(return_value, fmt_str, dt_str, dt_len, "Y-m-d H:i:s.uO", ztz);
	}
}

static zend_function_entry php_pqdt_methods[] = {
	PHP_ME(pqdt, createFromFormat, ai_pqdt_create_from_format, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	PHP_ME(pqdt, __toString, ai_pqdt_to_string, ZEND_ACC_PUBLIC)
	PHP_MALIAS(pqdt, jsonSerialize, __toString, ai_pqdt_to_string, ZEND_ACC_PUBLIC)
	{0}
};

zval *php_pqdt_from_string(zval *zv, char *input_fmt, char *dt_str, size_t dt_len, char *output_fmt, zval *ztimezone)
{
	php_date_obj *dobj;

	php_date_instantiate(php_pqdt_class_entry, zv);
	dobj = php_date_obj_from_obj(Z_OBJ_P(zv));
	if (!php_date_initialize(dobj, dt_str, dt_len, input_fmt, ztimezone, 1)) {
		zval_dtor(zv);
		ZVAL_NULL(zv);
	} else if (output_fmt) {
		zend_update_property_string(php_pqdt_class_entry, zv, ZEND_STRL("format"), output_fmt);
	}

	return zv;
}

zend_string *php_pqdt_to_string(zval *zdt, const char *format)
{
	zval rv;

	ZVAL_NULL(&rv);

	if (Z_OBJ_HT_P(zdt)->cast_object
	&&	SUCCESS == Z_OBJ_HT_P(zdt)->cast_object(zdt, &rv, IS_STRING)
	) {
		return Z_STR(rv);
	} else if (instanceof_function(Z_OBJCE_P(zdt), php_date_get_date_ce())) {
		zval rv, zfmt;

		ZVAL_NULL(&rv);
		ZVAL_STRING(&zfmt, format);
		zend_call_method_with_1_params(zdt, Z_OBJCE_P(zdt), NULL, "format", &rv, &zfmt);
		zval_ptr_dtor(&zfmt);

		if (Z_TYPE(rv) == IS_STRING) {
			return Z_STR(rv);
		}
		zval_ptr_dtor(&rv);
	}

	return NULL;
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
	zend_class_entry *json, ce = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Converter", php_pqconv_methods);
	php_pqconv_class_entry = zend_register_internal_interface(&ce);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce ,"pq", "DateTime", php_pqdt_methods);
	php_pqdt_class_entry = zend_register_internal_class_ex(&ce, php_date_get_date_ce());

	zend_declare_property_stringl(php_pqdt_class_entry, ZEND_STRL("format"), ZEND_STRL("Y-m-d H:i:s.uO"), ZEND_ACC_PUBLIC);

	/* stop reading this file right here! */
	if ((json = zend_hash_str_find_ptr(CG(class_table), ZEND_STRL("jsonserializable")))) {
		zend_class_implements(php_pqdt_class_entry, 1, json);
	}

	return SUCCESS;
}

typedef struct _HashTableList {
	zval arr;
	struct _HashTableList *parent;
} HashTableList;

typedef struct _ArrayParserState {
	const char *ptr, *end;
	HashTableList *list;
	php_pqres_t *res;
	Oid typ;
	unsigned quotes:1;
	unsigned escaped:1;
} ArrayParserState;

static char caa(ArrayParserState *a, const char *any, unsigned advance)
{
	const char *p = any;

	do {
		if (*p == *a->ptr) {
			a->ptr += advance;
			return *p;
		}
	} while (*++p);

	php_error_docref(NULL, E_WARNING, "Failed to parse array: expected one of '%s', got '%c'", any, *a->ptr); \
	return 0;
}

static ZEND_RESULT_CODE add_element(ArrayParserState *a, const char *start)
{
	zval zelem;
	zend_string *zstr = zend_string_init(start, a->ptr - start, 0);

	if (a->quotes) {
		php_stripslashes(zstr);
		ZVAL_STR(&zelem, zstr);
	} else if (!zend_string_equals_literal(zstr, "NULL")) {
		ZVAL_STR(&zelem, zstr);
	} else {
		zend_string_release(zstr);
		ZVAL_NULL(&zelem);
	}

	if (!ZVAL_IS_NULL(&zelem)) {
		php_pqres_typed_zval(a->res, a->typ, &zelem);
	}

	add_next_index_zval(&a->list->arr, &zelem);
	return SUCCESS;
}

static ZEND_RESULT_CODE parse_array(ArrayParserState *a);

static ZEND_RESULT_CODE parse_element(ArrayParserState *a, char delim)
{
	const char *el;

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
				php_error_docref(NULL, E_WARNING, "Failed to parse element, unexpected quote: '%.*s'", (int) (a->ptr - el), el);
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

	php_error_docref(NULL, E_WARNING, "Failed to parse element, reached end of input");
	return FAILURE;
}

static ZEND_RESULT_CODE parse_elements(ArrayParserState *a)
{
	char delims[] = {'}', PHP_PQ_DELIM_OF_ARRAY(a->typ), 0};

	while (SUCCESS == parse_element(a, delims[1])) {
		switch (caa(a, delims, 0)) {
		case 0:
			return FAILURE;

		case '}':
			return SUCCESS;

		default:
			if (!*++a->ptr) {
				php_error_docref(NULL, E_WARNING, "Failed to parse elements, reached end of input");
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
	array_init(&list->arr);

	if (a->list) {
		add_next_index_zval(&a->list->arr, &list->arr);
		list->parent = a->list;
	}
	a->list = list;

	if (SUCCESS != parse_elements(a)) {
		return FAILURE;
	}

	if (!caa(a, "}", 1)) {
		return FAILURE;
	}

	/* step one level back up */
	if (a->list->parent) {
		HashTableList *l = a->list->parent;

		efree(a->list);
		a->list = l;
	}

	return SUCCESS;
}

HashTable *php_pq_parse_array(php_pqres_t *res, const char *val_str, size_t val_len, Oid typ)
{
	HashTable *ht = NULL;
	ArrayParserState a = {0};

	a.typ = typ;
	a.ptr = val_str;
	a.end = val_str + val_len;
	a.res = res;

	if (SUCCESS != parse_array(&a)) {
		while (a.list) {
			HashTableList *l = a.list->parent;

			zval_dtor(&a.list->arr);
			efree(a.list);
			a.list = l;
		}
		return ht;
	}

	if (*a.ptr) {
		php_error_docref(NULL, E_NOTICE, "Trailing input: '%s'", a.ptr);
	}

	while (a.list) {
		HashTableList *l = a.list->parent;

		ht = Z_ARRVAL(a.list->arr);
		efree(a.list);
		a.list = l;
	}

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
