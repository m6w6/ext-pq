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


#ifndef PHP_PQCANCEL_H
#define PHP_PQCANCEL_H

#include "php_pqconn.h"

typedef struct php_pqcancel {
	PGcancel *cancel;
	php_pqconn_object_t *conn;
} php_pqcancel_t;

typedef struct php_pqcancel_object {
	php_pqcancel_t *intern;
	HashTable *prophandler;
	zend_object zo;
} php_pqcancel_object_t;

extern zend_class_entry *php_pqcancel_class_entry;
extern php_pqcancel_object_t *php_pqcancel_create_object_ex(zend_class_entry *ce, php_pqcancel_t *intern);

extern PHP_MINIT_FUNCTION(pqcancel);
extern PHP_MSHUTDOWN_FUNCTION(pqcancel);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
