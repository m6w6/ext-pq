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
#include <ext/standard/info.h>

#include <libpq-events.h>

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pqcancel.h"
#include "php_pqconn.h"
#include "php_pqcopy.h"
#include "php_pqcur.h"
#include "php_pqexc.h"
#include "php_pqlob.h"
#include "php_pqres.h"
#include "php_pqstm.h"
#include "php_pqtxn.h"
#include "php_pqtypes.h"

ZEND_DECLARE_MODULE_GLOBALS(php_pq);

static void php_pq_globals_init_once(zend_php_pq_globals *G)
{
	memset(G, 0, sizeof(*G));
}

#define PHP_MINIT_CALL(i) do { \
	if (SUCCESS != PHP_MINIT(i)(type, module_number)) { \
		return FAILURE; \
	} \
} while(0)

static PHP_MINIT_FUNCTION(pq)
{
	ZEND_INIT_MODULE_GLOBALS(php_pq, php_pq_globals_init_once, NULL);

	PHP_MINIT_CALL(pq_misc);
	PHP_MINIT_CALL(pqexc);

	PHP_MINIT_CALL(pqconn);
	PHP_MINIT_CALL(pqcancel);
	PHP_MINIT_CALL(pqtypes);

	PHP_MINIT_CALL(pqres);
	PHP_MINIT_CALL(pqstm);
	PHP_MINIT_CALL(pqtxn);
	PHP_MINIT_CALL(pqcur);

	PHP_MINIT_CALL(pqcopy);
	PHP_MINIT_CALL(pqlob);

	return SUCCESS;
}

#define PHP_MSHUT_CALL(i) do { \
	if (SUCCESS != PHP_MSHUTDOWN(i)(type, module_number)) { \
		return FAILURE; \
	} \
} while(0)

static PHP_MSHUTDOWN_FUNCTION(pq)
{
	PHP_MSHUT_CALL(pqlob);
	PHP_MSHUT_CALL(pqcopy);
	PHP_MSHUT_CALL(pqcur);
	PHP_MSHUT_CALL(pqtxn);
	PHP_MSHUT_CALL(pqstm);
	PHP_MSHUT_CALL(pqres);
	PHP_MSHUT_CALL(pqtypes);
	PHP_MSHUT_CALL(pqcancel);
	PHP_MSHUT_CALL(pqconn);

	return SUCCESS;
}

static PHP_MINFO_FUNCTION(pq)
{
#ifdef HAVE_PQLIBVERSION
	int libpq_v;
#endif
	char libpq_version[10] = "pre-9.1";

	php_info_print_table_start();
	php_info_print_table_header(2, "PQ Support", "enabled");
	php_info_print_table_row(2, "Extension Version", PHP_PQ_VERSION);
	php_info_print_table_end();

	php_info_print_table_start();
	php_info_print_table_header(3, "Used Library", "Compiled", "Linked");
#ifdef HAVE_PQLIBVERSION
	libpq_v = PQlibVersion();
	php_pq_version_to_string(libpq_v, libpq_version, sizeof(libpq_version));
#endif
	php_info_print_table_row(3, "libpq", PHP_PQ_LIBVERSION, libpq_version);
	php_info_print_table_end();
}

static const zend_function_entry pq_functions[] = {
	{0}
};

static zend_module_dep pq_module_deps[] = {
	ZEND_MOD_REQUIRED("raphf")
	ZEND_MOD_REQUIRED("spl")
	ZEND_MOD_OPTIONAL("json")
	ZEND_MOD_END
};

zend_module_entry pq_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	pq_module_deps,
	"pq",
	pq_functions,
	PHP_MINIT(pq),
	PHP_MSHUTDOWN(pq),
	NULL,/*PHP_RINIT(pq),*/
	NULL,/*PHP_RSHUTDOWN(pq),*/
	PHP_MINFO(pq),
	PHP_PQ_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PQ
ZEND_GET_MODULE(pq)
#endif


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
