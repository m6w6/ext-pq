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

struct apply_pi_to_ht_arg {
	HashTable *ht;
	zval *object;
	php_pq_object_t *pq_obj;
	unsigned addref:1;
};

static int apply_pi_to_ht(void *p, void *a TSRMLS_DC)
{
	zend_property_info *pi = p;
	struct apply_pi_to_ht_arg *arg = a;
	zval *property = zend_read_property(arg->pq_obj->zo.ce, arg->object, pi->name, pi->name_length, 0 TSRMLS_CC);

	if (arg->addref) {
		Z_ADDREF_P(property);
	}
	zend_hash_update(arg->ht, pi->name, pi->name_length + 1, (void *) &property, sizeof(zval *), NULL);

	return ZEND_HASH_APPLY_KEEP;
}

HashTable *php_pq_object_debug_info(zval *object, int *temp TSRMLS_DC)
{
	struct apply_pi_to_ht_arg arg = {NULL};

	*temp = 1;
	ALLOC_HASHTABLE(arg.ht);
	ZEND_INIT_SYMTABLE(arg.ht);

	arg.object = object;
	arg.pq_obj = zend_object_store_get_object(object TSRMLS_CC);
	arg.addref = 1;

	zend_hash_apply_with_argument(&arg.pq_obj->zo.ce->properties_info, apply_pi_to_ht, &arg TSRMLS_CC);

	return arg.ht;
}

HashTable *php_pq_object_properties(zval *object TSRMLS_DC)
{
	struct apply_pi_to_ht_arg arg = {NULL};

	arg.ht = zend_get_std_object_handlers()->get_properties(object TSRMLS_CC);
	arg.object = object;
	arg.pq_obj = zend_object_store_get_object(object TSRMLS_CC);
	arg.addref = 1;

	zend_hash_apply_with_argument(&arg.pq_obj->zo.ce->properties_info, apply_pi_to_ht, &arg TSRMLS_CC);

	return arg.ht;
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
	zval *return_value = NULL;

	if (!obj->intern) {
		php_error(E_RECOVERABLE_ERROR, "%s not initialized", ancestor(obj->zo.ce)->name);
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	} else if ((SUCCESS != zend_hash_find(obj->prophandler, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) || !handler->read) {
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	} else if (type != BP_VAR_R) {
		php_error(E_WARNING, "Cannot access %s properties by reference or array key/index", ancestor(obj->zo.ce)->name);
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	} else {
		ALLOC_ZVAL(return_value);
		Z_SET_REFCOUNT_P(return_value, 0);
		Z_UNSET_ISREF_P(return_value);

		handler->read(object, obj, return_value TSRMLS_CC);
	}

	return return_value;
}

void php_pq_object_write_prop(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC)
{
	php_pq_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;

	if (!obj->intern) {
		php_error(E_RECOVERABLE_ERROR, "%s not initialized", ancestor(obj->zo.ce)->name);
		zend_get_std_object_handlers()->write_property(object, member, value, key TSRMLS_CC);
	} else if (SUCCESS == zend_hash_find(obj->prophandler, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (handler->write) {
			handler->write(object, obj, value TSRMLS_CC);
		}
	} else {
		zend_get_std_object_handlers()->write_property(object, member, value, key TSRMLS_CC);
	}
}

