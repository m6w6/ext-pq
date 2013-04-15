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

#ifndef PHP_PQ_OBJECT_H
#define PHP_PQ_OBJECT_H

typedef struct php_pq_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	void *intern;
} php_pq_object_t;

typedef void (*php_pq_object_prophandler_func_t)(zval *object, void *o, zval *return_value TSRMLS_DC);

typedef struct php_pq_object_prophandler {
	php_pq_object_prophandler_func_t read;
	php_pq_object_prophandler_func_t write;
} php_pq_object_prophandler_t;

void php_pq_object_to_zval(void *o, zval **zv TSRMLS_DC);
void php_pq_object_to_zval_no_addref(void *o, zval **zv TSRMLS_DC);
void php_pq_object_addref(void *o TSRMLS_DC);
void php_pq_object_delref(void *o TSRMLS_DC);
HashTable *php_pq_object_debug_info(zval *object, int *temp TSRMLS_DC);
HashTable *php_pq_object_properties(zval *object TSRMLS_DC);
HashTable *php_pq_object_gc(zval *object, zval ***gc_argv, int *gc_argc TSRMLS_DC);
zend_class_entry *ancestor(zend_class_entry *ce);
zval *php_pq_object_read_prop(zval *object, zval *member, int type, const zend_literal *key TSRMLS_DC);
void php_pq_object_write_prop(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
