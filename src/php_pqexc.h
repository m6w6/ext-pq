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


#ifndef PHP_PQEXC_H
#define PHP_PQEXC_H

typedef enum php_pqexc_type {
	EX_INVALID_ARGUMENT,
	EX_RUNTIME,
	EX_CONNECTION_FAILED,
	EX_IO,
	EX_ESCAPE,
	EX_BAD_METHODCALL,
	EX_UNINITIALIZED,
	EX_DOMAIN,
	EX_SQL
} php_pqexc_type_t;

extern zend_class_entry *exce(php_pqexc_type_t type);
extern zend_object *throw_exce(php_pqexc_type_t type, const char *fmt, ...);

extern PHP_MINIT_FUNCTION(pqexc);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
