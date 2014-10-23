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


#ifndef PHP_PQTXN_H
#define PHP_PQTXN_H

#include "php_pqconn.h"

typedef enum php_pqtxn_isolation {
	PHP_PQTXN_READ_COMMITTED,
	PHP_PQTXN_REPEATABLE_READ,
	PHP_PQTXN_SERIALIZABLE,
} php_pqtxn_isolation_t;

typedef struct php_pqtxn {
	php_pqconn_object_t *conn;
	php_pqtxn_isolation_t isolation;
	unsigned savepoint;
	unsigned open:1;
	unsigned readonly:1;
	unsigned deferrable:1;
} php_pqtxn_t;

typedef struct php_pqtxn_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqtxn_t *intern;
} php_pqtxn_object_t;

extern const char *isolation_level(long *isolation);

extern zend_class_entry *php_pqtxn_class_entry;
extern zend_object_value php_pqtxn_create_object_ex(zend_class_entry *ce, php_pqtxn_t *intern, php_pqtxn_object_t **ptr TSRMLS_DC);

extern PHP_MINIT_FUNCTION(pqtxn);
extern PHP_MSHUTDOWN_FUNCTION(pqtxn);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
