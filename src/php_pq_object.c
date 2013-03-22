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

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include <php.h>

#include "php_pq_object.h"

void php_pq_object_to_zval(void *o, zval **zv TSRMLS_DC)
{
	php_pq_object_t *obj = o;

	if (!*zv) {
		MAKE_STD_ZVAL(*zv);
	}

	zend_objects_store_add_ref_by_handle(obj->zv.handle TSRMLS_CC);

	(*zv)->type = IS_OBJECT;
	(*zv)->value.obj = obj->zv;
}

void php_pq_object_to_zval_no_addref(void *o, zval **zv TSRMLS_DC)
{
	php_pq_object_t *obj = o;

	if (!*zv) {
		MAKE_STD_ZVAL(*zv);
	}

	/* no add ref */

	(*zv)->type = IS_OBJECT;
	(*zv)->value.obj = obj->zv;
}

void php_pq_object_addref(void *o TSRMLS_DC)
{
	php_pq_object_t *obj = o;
	zend_objects_store_add_ref_by_handle(obj->zv.handle TSRMLS_CC);
}

void php_pq_object_delref(void *o TSRMLS_DC)
{
	php_pq_object_t *obj = o;
	zend_objects_store_del_ref_by_handle_ex(obj->zv.handle, obj->zv.handlers TSRMLS_CC);
}

static int apply_pi_to_ht(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	zend_property_info *pi = p;
	HashTable *ht = va_arg(argv, HashTable *);
	zval *object = va_arg(argv, zval *);
	php_pq_object_t *obj = va_arg(argv, php_pq_object_t *);
	int addref = va_arg(argv, int);
	zval *property = zend_read_property(obj->zo.ce, object, pi->name, pi->name_length, 0 TSRMLS_CC);

	if (addref) {
		Z_ADDREF_P(property);
	}
	zend_hash_add(ht, pi->name, pi->name_length + 1, (void *) &property, sizeof(zval *), NULL);

	return ZEND_HASH_APPLY_KEEP;
}

HashTable *php_pq_object_debug_info(zval *object, int *temp TSRMLS_DC)
{
	HashTable *ht;
	php_pq_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);

	*temp = 1;
	ALLOC_HASHTABLE(ht);
	ZEND_INIT_SYMTABLE(ht);

	zend_hash_apply_with_arguments(&obj->zo.ce->properties_info TSRMLS_CC, apply_pi_to_ht, 4, ht, object, obj, 1);

	return ht;
}

zend_class_entry *ancestor(zend_class_entry *ce)
{
	while (ce->parent) {
		ce = ce->parent;
	}
	return ce;
}

zval *php_pq_object_read_prop(zval *object, zval *member, int type, const zend_literal *key TSRMLS_DC)
{
	php_pq_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;
	zval *return_value;

	if (!obj->intern) {
		zend_error(E_WARNING, "%s not initialized", ancestor(obj->zo.ce)->name);
	} else if ((SUCCESS == zend_hash_find(obj->prophandler, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) && handler->read) {
		if (type == BP_VAR_R) {
			ALLOC_ZVAL(return_value);
			Z_SET_REFCOUNT_P(return_value, 0);
			Z_UNSET_ISREF_P(return_value);

			handler->read(object, obj, return_value TSRMLS_CC);
		} else {
			zend_error(E_ERROR, "Cannot access %s properties by reference or array key/index", ancestor(obj->zo.ce)->name);
			return_value = NULL;
		}
	} else {
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	}

	return return_value;
}

void php_pq_object_write_prop(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC)
{
	php_pq_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;

	if (SUCCESS == zend_hash_find(obj->prophandler, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (handler->write) {
			handler->write(object, obj, value TSRMLS_CC);
		}
	} else {
		zend_get_std_object_handlers()->write_property(object, member, value, key TSRMLS_CC);
	}
}

