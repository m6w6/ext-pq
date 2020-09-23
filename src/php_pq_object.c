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
			handler->gc(arg->pq_obj, &return_value);
		}
	} else {
		zval tmp_prop, *property = NULL;

		property = php_pq_object_read_prop_80(&arg->pq_obj->zo, pi->name, BP_VAR_R, NULL, &tmp_prop);
		zend_hash_update(arg->ht, pi->name, property);
	}

	return ZEND_HASH_APPLY_KEEP;
}

HashTable *php_pq_object_debug_info_80(zend_object *object, int *temp)
{
	struct apply_pi_to_ht_arg arg = {NULL};

	*temp = 1;
	ALLOC_HASHTABLE(arg.ht);
	ZEND_INIT_SYMTABLE(arg.ht);

	arg.pq_obj = PHP_PQ_OBJ(NULL, object);
	arg.gc = 0;

	zend_hash_apply_with_argument(&arg.pq_obj->zo.ce->properties_info, apply_pi_to_ht, &arg);

	return arg.ht;
}
HashTable *php_pq_object_debug_info_70(zval *object, int *temp)
{
	return php_pq_object_debug_info_80(Z_OBJ_P(object), temp);
}

static inline HashTable *php_pq_object_properties_ex(zend_object *object, HashTable *props)
{
	struct apply_pi_to_ht_arg arg = {NULL};

	arg.ht = props;
	arg.pq_obj = PHP_PQ_OBJ(NULL, object);
	arg.gc = 0;

	zend_hash_apply_with_argument(&arg.pq_obj->zo.ce->properties_info, apply_pi_to_ht, &arg);

	return arg.ht;
}
HashTable *php_pq_object_properties_80(zend_object *object)
{
	return php_pq_object_properties_ex(object, zend_std_get_properties(object));
}
HashTable *php_pq_object_properties_70(zval *object)
{
	return php_pq_object_properties_ex(Z_OBJ_P(object), zend_std_get_properties(object));
}

static inline HashTable *php_pq_object_get_gc_ex(zend_object *object, HashTable *props, zval **table, int *n)
{
	struct apply_pi_to_ht_arg arg = {NULL};

	arg.pq_obj = PHP_PQ_OBJ(NULL, object);
	arg.ht = &arg.pq_obj->gc;
	arg.gc = 1;

	zend_hash_clean(arg.ht);
	zend_hash_copy(arg.ht, props, NULL);
	zend_hash_apply_with_argument(&arg.pq_obj->zo.ce->properties_info, apply_pi_to_ht, &arg);

	*table = NULL;
	*n = 0;

	return arg.ht;
}
HashTable *php_pq_object_get_gc_80(zend_object *object, zval **table, int *n)
{
	return php_pq_object_get_gc_ex(object, zend_std_get_properties(object), table, n);
}
HashTable *php_pq_object_get_gc_70(zval *object, zval **table, int *n)
{
	return php_pq_object_get_gc_ex(Z_OBJ_P(object), zend_std_get_properties(object), table, n);
}

zend_class_entry *ancestor(zend_class_entry *ce)
{
	while (ce->parent) {
		ce = ce->parent;
	}
	return ce;
}

zval *php_pq_object_read_prop_ex(zend_object *object, zend_string *member, int type, void **cache_slot, zval *tmp, zval *def)
{
	php_pq_object_t *obj = PHP_PQ_OBJ(NULL, object);
	php_pq_object_prophandler_t *handler;
	zval *return_value = def;

	if (!obj->intern) {
		php_error(E_RECOVERABLE_ERROR, "%s not initialized", ancestor(obj->zo.ce)->name->val);
	} else if (!(handler = zend_hash_find_ptr(obj->prophandler, member)) || !handler->read) {
		/* default handler */
	} else if (type != BP_VAR_R) {
		php_error(E_WARNING, "Cannot access %s properties by reference or array key/index", ancestor(obj->zo.ce)->name->val);
	} else {
		handler->read(obj, tmp);
		zend_get_std_object_handlers()->write_property(object, member, tmp, cache_slot);
		return_value = tmp;

		if (cache_slot) {
			*cache_slot = NULL;
		}
	}

	return return_value;
}
zval *php_pq_object_read_prop_80(zend_object *object, zend_string *member, int type, void **cache_slot, zval *tmp)
{
	return php_pq_object_read_prop_ex(object, member, type, cache_slot, tmp, zend_std_read_property(object, member, type, cache_slot, tmp));
}
zval *php_pq_object_read_prop_70(zval *object, zval *member, int type, void **cache_slot, zval *tmp)
{
	zend_string *member_str = zval_get_string(member);
	zval *return_value = php_pq_object_read_prop_ex(Z_OBJ_P(object), member_str, type, cache_slot, tmp, zend_std_read_property(object, member, type, cache_slot, tmp));
	zend_string_release(member_str);
	return return_value;
}

zval *php_pq_object_write_prop_ex(zend_object *object, zend_string *member, zval *value, void **cache_slot, int *update_std)
{
	php_pq_object_t *obj = PHP_PQ_OBJ(NULL, object);
	php_pq_object_prophandler_t *handler;

	if (!obj->intern) {
		php_error(E_RECOVERABLE_ERROR, "%s not initialized", ancestor(obj->zo.ce)->name->val);
		*update_std = 1;
	} else if ((handler = zend_hash_find_ptr(obj->prophandler, member))) {
		if (handler->write) {
			handler->write(obj, value);
		}
		*update_std = 0;
	} else {
		*update_std = 1;
	}
	return value;
}
zval *php_pq_object_write_prop_80(zend_object *object, zend_string *member, zval *value, void **cache_slot)
{
	int update_std = 0;
	zval *return_value = php_pq_object_write_prop_ex(object, member, value, cache_slot, &update_std);
	if (update_std) {
		return_value = zend_std_write_property(object, member, value, cache_slot);
	}
	return return_value;
}
zval *php_pq_object_write_prop_74(zval *object, zval *member, zval *value, void **cache_slot)
{
	int update_std = 0;
	zend_string *member_str = zval_get_string(member);
	zval *return_value = php_pq_object_write_prop_ex(Z_OBJ_P(object), member_str, value, cache_slot, &update_std);
	zend_string_release(member_str);
	if (update_std) {
		return_value = zend_std_write_property(object, member, value, cache_slot);
	}
	return return_value;
}
void php_pq_object_write_prop_70(zval *object, zval *member, zval *value, void **cache_slot)
{
	(void) php_pq_object_write_prop_74(object, member, value, cache_slot);
}

zval *php_pq_object_get_prop_ptr_null_80(zend_object *object, zend_string *member, int type, void **cache_slot)
{
	return NULL;
}
zval *php_pq_object_get_prop_ptr_null_70(zval *object, zval *member, int type, void **cache_slot)
{
	return NULL;
}

void php_pq_object_prophandler_dtor(zval *zv) {
	pefree(Z_PTR_P(zv), 1);
}

