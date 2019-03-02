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

void *php_pq_object_create(zend_class_entry *ce, void *intern, size_t obj_size, zend_object_handlers *oh, HashTable *ph)
{
	php_pq_object_t *o = ecalloc(1, obj_size + zend_object_properties_size(ce));

	zend_object_std_init(&o->zo, ce);
	object_properties_init(&o->zo, ce);
	o->zo.handlers = oh;
	o->intern = intern;
	o->prophandler = ph;

	zend_hash_init(&o->gc, 0, NULL, NULL, 0);

	return o;
}

void php_pq_object_dtor(zend_object *o)
{
	php_pq_object_t *obj = PHP_PQ_OBJ(NULL, o);

	zend_hash_destroy(&obj->gc);
	zend_object_std_dtor(o);
}

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
#ifdef GC_ADDREF
	GC_ADDREF(&obj->zo);
#else
	++GC_REFCOUNT(&obj->zo);
#endif
}

void php_pq_object_delref(void *o)
{
	php_pq_object_t *obj = o;
	zval tmp;

	/* this should gc immediately */
	ZVAL_OBJ(&tmp, &obj->zo);
	zval_ptr_dtor(&tmp);
}

struct apply_pi_to_ht_arg {
	HashTable *ht;
	zval *object;
	php_pq_object_t *pq_obj;
	unsigned gc:1;
};

static int apply_pi_to_ht(zval *p, void *a)
{
	zend_property_info *pi = Z_PTR_P(p);
	struct apply_pi_to_ht_arg *arg = a;

	if (arg->gc) {
		php_pq_object_prophandler_t *handler;

		if ((handler = zend_hash_find_ptr(arg->pq_obj->prophandler, pi->name)) && handler->gc) {
			zval member, return_value;

			ZVAL_STR(&member, pi->name);
			ZVAL_ARR(&return_value, arg->ht);
			handler->gc(arg->object, arg->pq_obj, &return_value);
		}
	} else {
		zval tmp_prop, *property = NULL;

		property = zend_read_property(arg->pq_obj->zo.ce, arg->object, pi->name->val, pi->name->len, 0, &tmp_prop);
		zend_hash_update(arg->ht, pi->name, property);
	}

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
	arg.gc = 0;

	zend_hash_apply_with_argument(&arg.pq_obj->zo.ce->properties_info, apply_pi_to_ht, &arg);

	return arg.ht;
}

HashTable *php_pq_object_properties(zval *object)
{
	struct apply_pi_to_ht_arg arg = {NULL};

	arg.ht = zend_get_std_object_handlers()->get_properties(object);
	arg.object = object;
	arg.pq_obj = PHP_PQ_OBJ(object, NULL);
	arg.gc = 0;

	zend_hash_apply_with_argument(&arg.pq_obj->zo.ce->properties_info, apply_pi_to_ht, &arg);

	return arg.ht;
}

HashTable *php_pq_object_get_gc(zval *object, zval **table, int *n)
{
	struct apply_pi_to_ht_arg arg = {NULL};

	arg.object = object;
	arg.pq_obj = PHP_PQ_OBJ(object, NULL);
	arg.ht = &arg.pq_obj->gc;
	arg.gc = 1;

	zend_hash_clean(arg.ht);
	zend_hash_copy(arg.ht, zend_std_get_properties(object), NULL);
	zend_hash_apply_with_argument(&arg.pq_obj->zo.ce->properties_info, apply_pi_to_ht, &arg);

	*table = NULL;
	*n = 0;

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

	return_value = zend_get_std_object_handlers()->read_property(object, member, type, cache_slot, tmp);

	if (!obj->intern) {
		php_error(E_RECOVERABLE_ERROR, "%s not initialized", ancestor(obj->zo.ce)->name->val);
	} else if (!(handler = zend_hash_find_ptr(obj->prophandler, Z_STR_P(member))) || !handler->read) {
		/* default handler */
	} else if (type != BP_VAR_R) {
		php_error(E_WARNING, "Cannot access %s properties by reference or array key/index", ancestor(obj->zo.ce)->name->val);
	} else {
		handler->read(object, obj, tmp);
		zend_get_std_object_handlers()->write_property(object, member, tmp, cache_slot);
		return_value = tmp;

		/*
		zval dtor;

		ZVAL_COPY_VALUE(&dtor, return_value);

		ZVAL_ZVAL(return_value, tmp, 0, 0);
		zval_ptr_dtor(&dtor);

		*/

		if (cache_slot) {
			*cache_slot = NULL;
		}
	}

	return return_value;
}

php_pq_object_write_prop_t php_pq_object_write_prop(zval *object, zval *member, zval *value, void **cache_slot)
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
#if PHP_VERSION_ID >= 70400
	return value;
#endif
}

zval *php_pq_object_get_prop_ptr_null(zval *object, zval *member, int type, void **cache_slot)
{
	return NULL;
}

void php_pq_object_prophandler_dtor(zval *zv) {
	pefree(Z_PTR_P(zv), 1);
}

