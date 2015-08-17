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
	void *intern;
	HashTable *prophandler;
	zend_object zo;
} php_pq_object_t;

static inline void *PHP_PQ_OBJ(zval *zv, zend_object *zo) {
	if (zv) {
		zo = Z_OBJ_P(zv);
	}
	return (void *) (((char *) zo) - zo->handlers->offset);
}

typedef void (*php_pq_object_prophandler_func_t)(zval *object, void *o, zval *return_value);

typedef struct php_pq_object_prophandler {
	php_pq_object_prophandler_func_t read;
	php_pq_object_prophandler_func_t write;
} php_pq_object_prophandler_t;

extern void php_pq_object_to_zval(void *o, zval *zv);
extern void php_pq_object_to_zval_no_addref(void *o, zval *zv);
extern void php_pq_object_addref(void *o);
extern void php_pq_object_delref(void *o);
extern HashTable *php_pq_object_debug_info(zval *object, int *temp);
extern HashTable *php_pq_object_properties(zval *object);
extern zend_class_entry *ancestor(zend_class_entry *ce);
extern zval *php_pq_object_read_prop(zval *object, zval *member, int type, void **cache_slot, zval *tmp);
extern void php_pq_object_write_prop(zval *object, zval *member, zval *value, void **cache_slot);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
