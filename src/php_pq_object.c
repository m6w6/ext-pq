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

void php_pq_object_to_zval(void *o, zval *zv)
{
	php_pq_object_t *obj = o;

	ZVAL_OBJ(zv, &obj->zo);
	Z_ADDREF_P(zv);
}

void php_pq_object_to_zval_no_addref(void *o, zval *zv)
{
	php_pq_object_t *obj = o;

	ZVAL_OBJ(zv, &obj->zo);
}

void php_pq_object_addref(void *o)
{
	php_pq_object_t *obj = o;
	++GC_REFCOUNT(&obj->zo);
}

void php_pq_object_delref(void *o TSRMLS_DC)
{
	php_pq_object_t *obj = o;
	zend_objects_store_del(&obj->zo);
}

struct apply_pi_to_ht_arg {
	HashTable *ht;
	zval *object;
	php_pq_object_t *pq_obj;
	unsigned addref:1;
};

static int apply_pi_to_ht(zval *p, void *a)
{
	zend_property_info *pi = Z_PTR_P(p);
	struct apply_pi_to_ht_arg *arg = a;
	zval tmp_prop, *property = zend_read_property(arg->pq_obj->zo.ce, arg->object, pi->name->val, pi->name->len, 0, &tmp_prop);

	if (arg->addref) {
		Z_TRY_ADDREF_P(property);
	}
	zend_hash_update(arg->ht, pi->name, property);

	return ZEND_HASH_APPLY_KEEP;
}

HashTable *php_pq_object_debug_info(zval *object, int *temp)
{
	struct apply_pi_to_ht_arg arg = {NULL};

	*temp = 1;
	ALLOC_HASHTABLE(arg.ht);
	ZEND_INIT_SYMTABLE(arg.ht);

	arg.object = object;
	arg.pq_obj = PHP_PQ_OBJ(object, NULL);
	arg.addref = 1;

	zend_hash_apply_with_argument(&arg.pq_obj->zo.ce->properties_info, apply_pi_to_ht, &arg);

	return arg.ht;
}

HashTable *php_pq_object_properties(zval *object)
{
	struct apply_pi_to_ht_arg arg = {NULL};

	arg.ht = zend_get_std_object_handlers()->get_properties(object);
	arg.object = object;
	arg.pq_obj = PHP_PQ_OBJ(object, NULL);
	arg.addref = 1;

	zend_hash_apply_with_argument(&arg.pq_obj->zo.ce->properties_info, apply_pi_to_ht, &arg);

	return arg.ht;
}

zend_class_entry *ancestor(zend_class_entry *ce)
{
	while (ce->parent) {
		ce = ce->parent;
	}
	return ce;
}

zval *php_pq_object_read_prop(zval *object, zval *member, int type, void **cache_slot, zval *tmp)
{
	php_pq_object_t *obj = PHP_PQ_OBJ(object, NULL);
	php_pq_object_prophandler_t *handler;
	zval *return_value = NULL;

	if (!obj->intern) {
		php_error(E_RECOVERABLE_ERROR, "%s not initialized", ancestor(obj->zo.ce)->name);
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, cache_slot, tmp);
	} else if (!(handler= zend_hash_find_ptr(obj->prophandler, Z_STR_P(member))) || !handler->read) {
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, cache_slot, tmp);
	} else if (type != BP_VAR_R) {
		php_error(E_WARNING, "Cannot access %s properties by reference or array key/index", ancestor(obj->zo.ce)->name->val);
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, cache_slot, tmp);
	} else {
		return_value = tmp;
		handler->read(object, obj, return_value);
	}

	return return_value;
}

void php_pq_object_write_prop(zval *object, zval *member, zval *value, void **cache_slot)
{
	php_pq_object_t *obj = PHP_PQ_OBJ(object, NULL);
	php_pq_object_prophandler_t *handler;

	if (!obj->intern) {
		php_error(E_RECOVERABLE_ERROR, "%s not initialized", ancestor(obj->zo.ce)->name->val);
		zend_get_std_object_handlers()->write_property(object, member, value, cache_slot);
	} else if ((handler = zend_hash_find_ptr(obj->prophandler, Z_STR_P(member)))) {
		if (handler->write) {
			handler->write(object, obj, value);
		}
	} else {
		zend_get_std_object_handlers()->write_property(object, member, value, cache_slot);
	}
}

