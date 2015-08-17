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
	char *query;
	unsigned allocated:1;
} php_pqstm_t;

typedef struct php_pqstm_object {
	PHP_PQ_OBJ_DECL(php_pqstm_t *)
} php_pqstm_object_t;

extern zend_class_entry *php_pqstm_class_entry;
extern php_pqstm_object_t *php_pqstm_create_object_ex(zend_class_entry *ce, php_pqstm_t *intern);
extern php_pqstm_t *php_pqstm_init(php_pqconn_object_t *conn, const char *name, const char *query, php_pq_params_t *params);

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
