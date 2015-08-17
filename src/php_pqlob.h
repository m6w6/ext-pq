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
	php_stream *stream;
	php_pqtxn_object_t *txn;
} php_pqlob_t;

typedef struct php_pqlob_object {
	php_pqlob_t *intern;
	HashTable *prophandler;
	zend_object zo;
} php_pqlob_object_t;

extern zend_class_entry *php_pqlob_class_entry;
extern php_pqlob_object_t *php_pqlob_create_object_ex(zend_class_entry *ce, php_pqlob_t *intern);

extern PHP_MINIT_FUNCTION(pqlob);
extern PHP_MSHUTDOWN_FUNCTION(pqlob);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
