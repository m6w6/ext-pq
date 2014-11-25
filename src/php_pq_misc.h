/*
    +--------------------------------------------------------------------+
    | PECL :: pq                                                         |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted provided that the conditions mentioned |
    | in the accompanying LICENSE file are met.                          |
    +--------------------------------------------------------------------+
    | Copyright (c) 2013, Michael Wallner <mike@php.net>                |
    +--------------------------------------------------------------------+
*/


#ifndef PHP_PQ_ERROR_H
#define PHP_PQ_ERROR_H

#include <libpq-fe.h>

typedef int STATUS; /* SUCCESS/FAILURE */

#include "php_pqres.h"

/* TSRM morony */
#if PHP_VERSION_ID >= 50700
#	define z_is_true(z) zend_is_true(z TSRMLS_CC)
#else
#	define z_is_true zend_is_true
#endif

/* trim LF from EOL */
extern char *php_pq_rtrim(char *e);

/* R, W, RW */
extern const char *php_pq_strmode(long mode);

/* compare array index */
extern int php_pq_compare_index(const void *lptr, const void *rptr TSRMLS_DC);

#define PHP_PQerrorMessage(c) php_pq_rtrim(PQerrorMessage((c)))
#define PHP_PQresultErrorMessage(r) php_pq_rtrim(PQresultErrorMessage((r)))

extern zend_class_entry *php_pqdt_class_entry;
extern zval *php_pqdt_from_string(zval *zv, char *input_fmt, char *dt_str, size_t dt_len, char *output_fmt, zval *ztimezone TSRMLS_DC);
extern void php_pqdt_to_string(zval *zdt, const char *format, char **str_buf, size_t *str_len TSRMLS_DC);

extern zend_class_entry *php_pqconv_class_entry;

extern HashTable *php_pq_parse_array(php_pqres_t *res, const char *val_str, size_t val_len, Oid typ TSRMLS_DC);


extern PHP_MINIT_FUNCTION(pq_misc);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
