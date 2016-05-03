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
#include <Zend/zend_closures.h>

#include "php_pq_callback.h"

void php_pq_callback_dtor(php_pq_callback_t *cb)
{
	if (cb->recursion) {
		php_pq_callback_dtor(cb->recursion);
		efree(cb->recursion);
		cb->recursion = NULL;
	}
	if (cb->fci.size > 0) {
		zend_fcall_info_args_clear(&cb->fci, 1);
		zval_ptr_dtor(&cb->fci.function_name);
		if (cb->fci.object_ptr) {
			zval_ptr_dtor(&cb->fci.object_ptr);
		}
		cb->fci.size = 0;
	}
}

void php_pq_callback_addref(php_pq_callback_t *cb)
{
	Z_ADDREF_P(cb->fci.function_name);
	if (cb->fci.object_ptr) {
		Z_ADDREF_P(cb->fci.object_ptr);
	}
}

zval *php_pq_callback_to_zval(php_pq_callback_t *cb)
{
	zval *zcb;

	php_pq_callback_addref(cb);

	if (cb->fci.object_ptr) {
		MAKE_STD_ZVAL(zcb);
		array_init_size(zcb, 2);
		add_next_index_zval(zcb, cb->fci.object_ptr);
		add_next_index_zval(zcb, cb->fci.function_name);
	} else {
		zcb = cb->fci.function_name;
	}

	return zcb;
}

zend_bool php_pq_callback_is_locked(php_pq_callback_t *cb TSRMLS_DC)
{
	if (php_pq_callback_is_enabled(cb)) {
		const zend_function *closure;
		const zend_execute_data *ex;

		if (Z_TYPE_P(cb->fci.function_name) != IS_OBJECT) {
			return 0;
		}

		closure = zend_get_closure_method_def(cb->fci.function_name TSRMLS_CC);
		if (closure->type != ZEND_USER_FUNCTION) {
			return 0;
		}

		for (ex = EG(current_execute_data); ex; ex = ex->prev_execute_data) {
			if (ex->op_array == &closure->op_array) {
				return 1;
			}
		}
	}

	if (!php_pq_callback_is_recurrent(cb)) {
		return 0;
	}
	return php_pq_callback_is_locked(cb->recursion TSRMLS_CC);
}

void php_pq_callback_recurse(php_pq_callback_t *old, php_pq_callback_t *new TSRMLS_DC)
{
	if (php_pq_callback_is_locked(old TSRMLS_CC)) {
		php_pq_callback_recurse_ex(old, new TSRMLS_CC);
	} else {
		php_pq_callback_dtor(old);
		if (php_pq_callback_is_enabled(new)) {
			php_pq_callback_addref(new);
			memcpy(old, new, sizeof(*old));
			new->fci.size = 0;
		}
	}
}

extern zend_bool php_pq_callback_is_enabled(php_pq_callback_t *cb)
{
	return cb && cb->fci.size > 0;
}

extern zend_bool php_pq_callback_is_recurrent(php_pq_callback_t *cb)
{
	return cb && cb->recursion != NULL;
}

extern void php_pq_callback_disable(php_pq_callback_t *cb TSRMLS_DC)
{
	if (php_pq_callback_is_enabled(cb)) {
		php_pq_callback_recurse_ex(cb, NULL TSRMLS_CC);
	}
}

extern void php_pq_callback_recurse_ex(php_pq_callback_t *old, php_pq_callback_t *new TSRMLS_DC)
{
	php_pq_callback_t *tmp = emalloc(sizeof(*tmp));

	if (new) {
		memcpy(tmp, old, sizeof(*tmp));
		memcpy(old, new, sizeof(*old));
		old->recursion = tmp;

		php_pq_callback_addref(old);
		php_pq_callback_disable(tmp TSRMLS_CC);
	} else {
		memcpy(tmp, old, sizeof(*tmp));
		memset(old, 0, sizeof(*old));
		old->recursion = tmp;
	}
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
