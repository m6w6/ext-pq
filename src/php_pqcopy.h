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


#ifndef PHP_PQCOPY_H
#define PHP_PQCOPY_H

#include "php_pqconn.h"

typedef enum php_pqcopy_direction {
	PHP_PQCOPY_FROM_STDIN,
	PHP_PQCOPY_TO_STDOUT
} php_pqcopy_direction_t;

typedef enum php_pqcopy_status {
	PHP_PQCOPY_FAIL,
	PHP_PQCOPY_CONT,
	PHP_PQCOPY_DONE
} php_pqcopy_status_t;

typedef struct php_pqcopy {
	php_pqcopy_direction_t direction;
	char *expression;
	char *options;
	php_pqconn_object_t *conn;
} php_pqcopy_t;

typedef struct php_pqcopy_object {
	PHP_PQ_OBJ_DECL(php_pqcopy_t *)
} php_pqcopy_object_t;

extern zend_class_entry *php_pqcopy_class_entry;
extern php_pqcopy_object_t *php_pqcopy_create_object_ex(zend_class_entry *ce, php_pqcopy_t *intern);

extern PHP_MINIT_FUNCTION(pqcopy);
extern PHP_MSHUTDOWN_FUNCTION(pqcopy);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
