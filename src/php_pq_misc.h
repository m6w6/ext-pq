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
char *rtrim(char *e);

/* R, W, RW */
const char *strmode(long mode);

/* compare array index */
int compare_index(const void *lptr, const void *rptr TSRMLS_DC);

#define PHP_PQerrorMessage(c) rtrim(PQerrorMessage((c)))
#define PHP_PQresultErrorMessage(r) rtrim(PQresultErrorMessage((r)))

zend_class_entry *php_pqdt_class_entry;
zval *php_pqdt_from_string(char *datetime_str, size_t datetime_len, char *fmt, zval *zv TSRMLS_DC);

zend_class_entry *php_pqconv_class_entry;

HashTable *php_pq_parse_array(php_pqres_t *res, const char *val_str, size_t val_len, Oid typ TSRMLS_DC);

PHP_MINIT_FUNCTION(pq_misc);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
