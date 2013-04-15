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

#include "php_pq_callback.h"

void php_pq_callback_dtor(php_pq_callback_t *cb)
{
	if (cb->fci.size > 0) {
		zend_fcall_info_args_clear(&cb->fci, 1);
		zval_ptr_dtor(&cb->fci.function_name);
		if (cb->fci.object_ptr) {
			zval_ptr_dtor(&cb->fci.object_ptr);
		}
	}
	cb->fci.size = 0;
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
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
