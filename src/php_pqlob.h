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


#ifndef PHP_PQLOB_H
#define PHP_PQLOB_H

#include "php_pqtxn.h"

typedef struct php_pqlob {
	int lofd;
	Oid loid;
	int stream;
	php_pqtxn_object_t *txn;
} php_pqlob_t;

typedef struct php_pqlob_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqlob_t *intern;
} php_pqlob_object_t;

zend_class_entry *php_pqlob_class_entry;
zend_object_value php_pqlob_create_object_ex(zend_class_entry *ce, php_pqlob_t *intern, php_pqlob_object_t **ptr TSRMLS_DC);

PHP_MINIT_FUNCTION(pqlob);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
