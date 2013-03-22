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


#ifndef PHP_PQRES_H
#define PHP_PQRES_H

typedef enum php_pqres_fetch {
	PHP_PQRES_FETCH_ARRAY,
	PHP_PQRES_FETCH_ASSOC,
	PHP_PQRES_FETCH_OBJECT
} php_pqres_fetch_t;

typedef struct php_pqres_iterator {
	zend_object_iterator zi;
	zval *current_val;
	unsigned index;
	php_pqres_fetch_t fetch_type;
} php_pqres_iterator_t;

typedef struct php_pqres {
	PGresult *res;
	php_pqres_iterator_t *iter;
	HashTable bound;
} php_pqres_t;

typedef struct php_pqres_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqres_t *intern;
} php_pqres_object_t;

STATUS php_pqres_success(PGresult *res TSRMLS_DC);
void php_pqres_init_instance_data(PGresult *res, php_pqres_object_t **ptr TSRMLS_DC);
zval *php_pqres_row_to_zval(PGresult *res, unsigned row, php_pqres_fetch_t fetch_type, zval **data_ptr TSRMLS_DC);

#include "php_pq_object.h"
#include "php_pqconn_event.h"
#define PHP_PQclear(_r) do { \
	php_pqres_object_t *_o = PQresultInstanceData((_r), php_pqconn_event); \
	if (_o) { \
		php_pq_object_delref(_o TSRMLS_CC); \
	} else { \
		PQclear(_r); \
	} \
} while(0)

zend_class_entry *php_pqres_class_entry;
zend_object_value php_pqres_create_object_ex(zend_class_entry *ce, php_pqres_t *intern, php_pqres_object_t **ptr TSRMLS_DC);

PHP_MINIT_FUNCTION(pqres);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
