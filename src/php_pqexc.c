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

#include <Zend/zend_exceptions.h>
#include <ext/spl/spl_exceptions.h>

#include "php_pq.h"
#include "php_pq_object.h"
#include "php_pqexc.h"

static zend_class_entry *php_pqexc_interface_class_entry;
static zend_class_entry *php_pqexc_invalid_argument_class_entry;
static zend_class_entry *php_pqexc_runtime_class_entry;
static zend_class_entry *php_pqexc_bad_methodcall_class_entry;
static zend_class_entry *php_pqexc_domain_class_entry;

static zend_function_entry php_pqexc_methods[] = {
	{0}
};

zend_class_entry *exce(php_pqexc_type_t type)
{
	switch (type) {
	default:
	case EX_INVALID_ARGUMENT:
		return php_pqexc_invalid_argument_class_entry;
	case EX_RUNTIME:
	case EX_CONNECTION_FAILED:
	case EX_IO:
	case EX_ESCAPE:
		return php_pqexc_runtime_class_entry;
	case EX_UNINITIALIZED:
	case EX_BAD_METHODCALL:
		return php_pqexc_bad_methodcall_class_entry;
	case EX_DOMAIN:
	case EX_SQL:
		return php_pqexc_domain_class_entry;
	}
}

zend_object *throw_exce(php_pqexc_type_t type, const char *fmt, ...)
{
	char *msg;
	zend_object *zexc;
	va_list argv;

	va_start(argv, fmt);
	vspprintf(&msg, 0, fmt, argv);
	va_end(argv);

	zexc = zend_throw_exception(exce(type), msg, type);
	efree(msg);

	return zexc;
}

PHP_MINIT_FUNCTION(pqexc)
{
	zend_class_entry ce = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Exception", php_pqexc_methods);
	php_pqexc_interface_class_entry = zend_register_internal_interface(&ce);
	zend_class_implements(php_pqexc_interface_class_entry, 1, zend_ce_throwable);

	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("INVALID_ARGUMENT"), EX_INVALID_ARGUMENT);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("RUNTIME"), EX_RUNTIME);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("CONNECTION_FAILED"), EX_CONNECTION_FAILED);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("IO"), EX_IO);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("ESCAPE"), EX_ESCAPE);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("BAD_METHODCALL"), EX_BAD_METHODCALL);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("UNINITIALIZED"), EX_UNINITIALIZED);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("DOMAIN"), EX_DOMAIN);
	zend_declare_class_constant_long(php_pqexc_interface_class_entry, ZEND_STRL("SQL"), EX_SQL);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq\\Exception", "InvalidArgumentException", php_pqexc_methods);
	php_pqexc_invalid_argument_class_entry = zend_register_internal_class_ex(&ce, spl_ce_InvalidArgumentException);
	zend_class_implements(php_pqexc_invalid_argument_class_entry, 1, php_pqexc_interface_class_entry);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq\\Exception", "RuntimeException", php_pqexc_methods);
	php_pqexc_runtime_class_entry = zend_register_internal_class_ex(&ce, spl_ce_RuntimeException);
	zend_class_implements(php_pqexc_runtime_class_entry, 1, php_pqexc_interface_class_entry);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq\\Exception", "BadMethodCallException", php_pqexc_methods);
	php_pqexc_bad_methodcall_class_entry = zend_register_internal_class_ex(&ce, spl_ce_BadMethodCallException);
	zend_class_implements(php_pqexc_bad_methodcall_class_entry, 1, php_pqexc_interface_class_entry);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq\\Exception", "DomainException", php_pqexc_methods);
	php_pqexc_domain_class_entry = zend_register_internal_class_ex(&ce, spl_ce_DomainException);
	zend_class_implements(php_pqexc_domain_class_entry, 1, php_pqexc_interface_class_entry);
	zend_declare_property_null(php_pqexc_domain_class_entry, ZEND_STRL("sqlstate"), ZEND_ACC_PUBLIC);

	return SUCCESS;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
