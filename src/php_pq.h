/*
    +--------------------------------------------------------------------+
    | PECL :: pq                                                         |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted provided that the conditions mentioned |
    | in the accompanying LICENSE file are met.                          |
    +--------------------------------------------------------------------+
    | Copyright (c) 2013, Michael Wallner <mike@php.net>                |
    +--------------------------------------------------------------------+
*/


#ifndef PHP_PQ_H
#define PHP_PQ_H

#define PHP_PQ_EXT_VERSION "0.1.0"

int pq_module_number;
zend_module_entry pq_module_entry;
#define phpext_pq_ptr &pq_module_entry

#ifdef PHP_WIN32
#	define PHP_PQ_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_PQ_API __attribute__ ((visibility("default")))
#else
#	define PHP_PQ_API
#endif

#ifdef ZTS
#	include "TSRM.h"
#endif

/* 
ZEND_BEGIN_MODULE_GLOBALS(pq)
	long  global_value;
	char *global_string;
ZEND_END_MODULE_GLOBALS(pq)

#ifdef ZTS
#define PQ_G(v) TSRMG(pq_globals_id, zend_pq_globals *, v)
#else
#define PQ_G(v) (pq_globals.v)
#endif
*/

#endif	/* PHP_PQ_H */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
