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
#define PHP_PQRES_CONV_BYTEA	0x0008
#define PHP_PQRES_CONV_SCALAR	0x000f
#define PHP_PQRES_CONV_ARRAY	0x0010
#define PHP_PQRES_CONV_DATETIME	0x0020
#define PHP_PQRES_CONV_JSON		0x0100
#define PHP_PQRES_CONV_ALL		0xffff

typedef struct php_pqres_iterator {
	zend_object_iterator zi;
	zval current_val;
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
	PHP_PQ_OBJ_DECL(php_pqres_t *)
} php_pqres_object_t;

extern ZEND_RESULT_CODE php_pqres_success(PGresult *res);
extern php_pqres_object_t *php_pqres_init_instance_data(PGresult *res, php_pqconn_object_t *obj);
extern zval *php_pqres_row_to_zval(PGresult *res, unsigned row, php_pqres_fetch_t fetch_type, zval *data);
extern zval *php_pqres_typed_zval(php_pqres_t *res, Oid typ, zval *zv);
extern php_pqres_fetch_t php_pqres_fetch_type(php_pqres_t *res);

#include "php_pq_object.h"
#include "php_pqconn_event.h"

extern zend_class_entry *php_pqres_class_entry;
extern php_pqres_object_t *php_pqres_create_object_ex(zend_class_entry *ce, php_pqres_t *intern);

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
