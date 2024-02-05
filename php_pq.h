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

#define PHP_PQ_VERSION "2.2.3"

#ifdef PHP_WIN32
#	define PHP_PQ_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_PQ_API extern __attribute__ ((visibility("default")))
#else
#	define PHP_PQ_API extern
#endif

extern int pq_module_number;
extern zend_module_entry pq_module_entry;
#define phpext_pq_ptr &pq_module_entry

ZEND_BEGIN_MODULE_GLOBALS(php_pq)
	struct {
		/* for ext-raphf */
		zend_string *name;
	} connection;
ZEND_END_MODULE_GLOBALS(php_pq)

ZEND_EXTERN_MODULE_GLOBALS(php_pq);

#ifdef ZTS
#	include "TSRM/TSRM.h"
#	define PHP_PQ_G ((zend_php_pq_globals *) (*((void ***) tsrm_get_ls_cache()))[TSRM_UNSHUFFLE_RSRC_ID(php_pq_globals_id)])
#else
#	define PHP_PQ_G (&php_pq_globals)
#endif

#endif	/* PHP_PQ_H */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
