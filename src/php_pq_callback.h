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
	void *data;
} php_pq_callback_t;

void php_pq_callback_dtor(php_pq_callback_t *cb);
void php_pq_callback_addref(php_pq_callback_t *cb);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
