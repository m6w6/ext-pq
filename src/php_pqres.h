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

#include "php_pqconn.h"

typedef enum php_pqres_fetch {
	PHP_PQRES_FETCH_ARRAY,
	PHP_PQRES_FETCH_ASSOC,
	PHP_PQRES_FETCH_OBJECT
} php_pqres_fetch_t;

#define PHP_PQRES_CONV_BOOL		0x0001
#define PHP_PQRES_CONV_INT		0x0002
#define PHP_PQRES_CONV_FLOAT	0x0004
#define PHP_PQRES_CONV_SCALAR	0x000f
#define PHP_PQRES_CONV_ARRAY	0x0010
#define PHP_PQRES_CONV_DATETIME	0x0020
#define PHP_PQRES_CONV_JSON		0x0100
#define PHP_PQRES_CONV_ALL		0xffff

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
	HashTable converters;
	unsigned auto_convert;
	php_pqres_fetch_t default_fetch_type;
} php_pqres_t;

typedef struct php_pqres_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqres_t *intern;
} php_pqres_object_t;

extern STATUS php_pqres_success(PGresult *res TSRMLS_DC);
extern void php_pqres_init_instance_data(PGresult *res, php_pqconn_object_t *obj, php_pqres_object_t **ptr TSRMLS_DC);
extern zval *php_pqres_row_to_zval(PGresult *res, unsigned row, php_pqres_fetch_t fetch_type, zval **data_ptr TSRMLS_DC);
extern zval *php_pqres_typed_zval(php_pqres_t *res, char *val, size_t len, Oid typ TSRMLS_DC);
extern php_pqres_fetch_t php_pqres_fetch_type(php_pqres_t *res);

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

extern zend_class_entry *php_pqres_class_entry;
extern zend_object_value php_pqres_create_object_ex(zend_class_entry *ce, php_pqres_t *intern, php_pqres_object_t **ptr TSRMLS_DC);

extern PHP_MINIT_FUNCTION(pqres);
extern PHP_MSHUTDOWN_FUNCTION(pqres);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
