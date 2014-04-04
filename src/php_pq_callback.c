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
	if (cb->fci.size > 0 && Z_TYPE_P(cb->fci.function_name) == IS_OBJECT) {
		const zend_function *closure = zend_get_closure_method_def(cb->fci.function_name TSRMLS_CC);

		if (closure->type == ZEND_USER_FUNCTION) {
			zend_execute_data *ex = EG(current_execute_data);

			while (ex) {
				if (ex->op_array == &closure->op_array) {
					return 1;
				}
				ex = ex->prev_execute_data;
			}
		}
	}
	return 0;
}

void php_pq_callback_recurse(php_pq_callback_t *old, php_pq_callback_t *new TSRMLS_DC)
{
	if (new && new->fci.size > 0 && php_pq_callback_is_locked(old TSRMLS_CC)) {
		new->recursion = emalloc(sizeof(*old));
		memcpy(new->recursion, old, sizeof(*old));
	} else if (new && new->fci.size > 0) {
		php_pq_callback_dtor(old);
		php_pq_callback_addref(new);
		memcpy(old, new, sizeof(*old));
		new->fci.size = 0;
	} else {
		php_pq_callback_dtor(old);
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
