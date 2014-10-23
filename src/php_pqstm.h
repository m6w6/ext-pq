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


#ifndef PHP_PQSTM_H
#define PHP_PQSTM_H

#include "php_pqconn.h"

typedef struct php_pqstm {
	php_pqconn_object_t *conn;
	char *name;
	HashTable bound;
	php_pq_params_t *params;
} php_pqstm_t;

typedef struct php_pqstm_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqstm_t *intern;
} php_pqstm_object_t;

extern zend_class_entry *php_pqstm_class_entry;
extern zend_object_value php_pqstm_create_object_ex(zend_class_entry *ce, php_pqstm_t *intern, php_pqstm_object_t **ptr TSRMLS_DC);

extern PHP_MINIT_FUNCTION(pqstm);
extern PHP_MSHUTDOWN_FUNCTION(pqstm);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
