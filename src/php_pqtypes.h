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


#ifndef PHP_PQTYPES_H
#define PHP_PQTYPES_H

#include "php_pqconn.h"

typedef struct php_pqtypes {
	HashTable types;
	php_pqconn_object_t *conn;
} php_pqtypes_t;

typedef struct php_pqtypes_object {
	php_pqtypes_t *intern;
	HashTable *prophandler;
	zend_object zo;
} php_pqtypes_object_t;

extern zend_class_entry *php_pqtypes_class_entry;
extern php_pqtypes_object_t *php_pqtypes_create_object_ex(zend_class_entry *ce, php_pqtypes_t *intern);

extern PHP_MINIT_FUNCTION(pqtypes);
extern PHP_MSHUTDOWN_FUNCTION(pqtypes);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
