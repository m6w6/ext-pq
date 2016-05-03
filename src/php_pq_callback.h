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

#ifndef PHP_PQ_CALLBACK_H
#define PHP_PQ_CALLBACK_H

#include <Zend/zend_interfaces.h>

typedef struct php_pq_callback {
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	struct php_pq_callback *recursion;
} php_pq_callback_t;

extern void php_pq_callback_dtor(php_pq_callback_t *cb);
extern void php_pq_callback_addref(php_pq_callback_t *cb);
extern zval *php_pq_callback_to_zval(php_pq_callback_t *cb);
extern zend_bool php_pq_callback_is_locked(php_pq_callback_t *cb TSRMLS_DC);
extern void php_pq_callback_recurse(php_pq_callback_t *old, php_pq_callback_t *new TSRMLS_DC);
extern zend_bool php_pq_callback_is_enabled(php_pq_callback_t *cb);
extern void php_pq_callback_disable(php_pq_callback_t *cb TSRMLS_DC);
extern void php_pq_callback_recurse_ex(php_pq_callback_t *old, php_pq_callback_t *new TSRMLS_DC);
extern zend_bool php_pq_callback_is_recurrent(php_pq_callback_t *cb);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
