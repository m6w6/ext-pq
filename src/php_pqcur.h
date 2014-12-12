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

#ifndef PHP_PQCUR_H
#define PHP_PQCUR_H

#include "php_pqconn.h"

#define PHP_PQ_DECLARE_BINARY       0x01
#define PHP_PQ_DECLARE_INSENSITIVE  0x02
#define PHP_PQ_DECLARE_WITH_HOLD    0x04

#define PHP_PQ_DECLARE_SCROLL       0x10
#define PHP_PQ_DECLARE_NO_SCROLL    0x20

typedef struct php_pqcur {
	php_pqconn_object_t *conn;
	char *name;
	char *decl;
	unsigned open:1;
	int query_offset;
	long flags;
} php_pqcur_t;

typedef struct php_pqcur_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqcur_t *intern;
} php_pqcur_object_t;

extern zend_class_entry *php_pqcur_class_entry;
extern zend_object_value php_pqcur_create_object_ex(zend_class_entry *ce, php_pqcur_t *intern, php_pqcur_object_t **ptr TSRMLS_DC);

extern char *php_pqcur_declare_str(const char *name_str, size_t name_len, unsigned flags, const char *query_str, size_t query_len, int *query_offset);
extern php_pqcur_t *php_pqcur_init(php_pqconn_object_t *conn, const char *name, char *decl, int query_offset, long flags TSRMLS_DC);

extern PHP_MINIT_FUNCTION(pqcur);
extern PHP_MSHUTDOWN_FUNCTION(pqcur);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
