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

#define PHP_PQ_OBJ_DECL(_intern_type) \
	_intern_type intern; \
	HashTable *prophandler; \
	HashTable gc; \
	zend_object zo;

typedef struct php_pq_object {
	PHP_PQ_OBJ_DECL(void *)
} php_pq_object_t;

static inline void *PHP_PQ_OBJ(zval *zv, zend_object *zo) {
	if (zv) {
		zo = Z_OBJ_P(zv);
	}
	return (void *) (((char *) zo) - zo->handlers->offset);
}

extern zend_class_entry *ancestor(zend_class_entry *ce);

typedef void (*php_pq_object_prophandler_func_t)(void *o, zval *return_value);

typedef struct php_pq_object_prophandler {
	php_pq_object_prophandler_func_t read;
	php_pq_object_prophandler_func_t write;
	php_pq_object_prophandler_func_t gc;
} php_pq_object_prophandler_t;

extern void php_pq_object_prophandler_dtor(zval *zv);

extern void *php_pq_object_create(zend_class_entry *ce, void *intern, size_t obj_size, zend_object_handlers *oh, HashTable *ph);
extern void php_pq_object_dtor(zend_object *obj);
extern void php_pq_object_to_zval(void *o, zval *zv);
extern void php_pq_object_to_zval_no_addref(void *o, zval *zv);
extern void php_pq_object_addref(void *o);
extern void php_pq_object_delref(void *o);

#if PHP_VERSION_ID >= 80000
# define php_pq_object_debug_info php_pq_object_debug_info_80
#else
# define php_pq_object_debug_info php_pq_object_debug_info_70
#endif
extern HashTable *php_pq_object_debug_info_80(zend_object *object, int *temp);
extern HashTable *php_pq_object_debug_info_70(zval *object, int *temp);

#if PHP_VERSION_ID >= 80000
# define php_pq_object_properties php_pq_object_properties_80
#else
# define php_pq_object_properties php_pq_object_properties_70
#endif
extern HashTable *php_pq_object_properties_80(zend_object *object);
extern HashTable *php_pq_object_properties_70(zval *object);

#if PHP_VERSION_ID >= 80000
# define php_pq_object_get_gc php_pq_object_get_gc_80
#else
# define php_pq_object_get_gc php_pq_object_get_gc_70
#endif
extern HashTable *php_pq_object_get_gc_80(zend_object *object, zval **table, int *n);
extern HashTable *php_pq_object_get_gc_70(zval *object, zval **table, int *n);

#if PHP_VERSION_ID >= 80000
# define php_pq_object_read_prop php_pq_object_read_prop_80
#elif PHP_VERSION_ID >= 70400
# define php_pq_object_read_prop php_pq_object_read_prop_74
#else
# define php_pq_object_read_prop php_pq_object_read_prop_70
#endif
extern zval *php_pq_object_read_prop_80(zend_object *object, zend_string *member, int type, void **cache_slot, zval *tmp);
extern zval *php_pq_object_read_prop_74(zval *object, zval *member, int type, void **cache_slot, zval *tmp);
extern void php_pq_object_read_prop_70(zval *object, zval *member, int type, void **cache_slot, zval *tmp);

#if PHP_VERSION_ID >= 80000
# define php_pq_object_write_prop php_pq_object_write_prop_80
#elif PHP_VERSION_ID >= 70400
# define php_pq_object_write_prop php_pq_object_write_prop_74
#else
# define php_pq_object_write_prop php_pq_object_write_prop_70
#endif
extern zval *php_pq_object_write_prop_80(zend_object *object, zend_string *member, zval *value, void **cache_slot);
extern zval *php_pq_object_write_prop_74(zval *object, zval *member, zval *value, void **cache_slot);
extern void php_pq_object_write_prop_70(zval *object, zval *member, zval *value, void **cache_slot);

#if PHP_VERSION_ID >= 80000
# define php_pq_object_get_prop_ptr_null php_pq_object_get_prop_ptr_null_80
#else
# define php_pq_object_get_prop_ptr_null php_pq_object_get_prop_ptr_null_70
#endif
extern zval *php_pq_object_get_prop_ptr_null_80(zend_object *object, zend_string *member, int type, void **cache_slot);
extern zval *php_pq_object_get_prop_ptr_null_70(zval *object, zval *member, int type, void **cache_slot);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
