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

#include <libpq-fe.h>

/*
#include <Zend/zend_interfaces.h>
#include <Zend/zend_exceptions.h>
#include <ext/spl/spl_array.h>
#include <ext/spl/spl_exceptions.h>
#include <ext/raphf/php_raphf.h>

#include <libpq-events.h>
#include <libpq/libpq-fs.h>
#include <fnmatch.h>
*/

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pqcancel.h"
#include "php_pqconn.h"
#include "php_pqcopy.h"
#include "php_pqexc.h"
#include "php_pqlob.h"
#include "php_pqres.h"
#include "php_pqstm.h"
#include "php_pqtxn.h"
#include "php_pqtypes.h"

#define PHP_MINIT_CALL(i) do { \
	if (SUCCESS != PHP_MINIT(i)(type, module_number TSRMLS_CC)) { \
		return FAILURE; \
	} \
} while(0)

static PHP_MINIT_FUNCTION(pq)
{
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

	return php_persistent_handle_provide(ZEND_STRL("pq\\Connection"), php_pqconn_get_resource_factory_ops(), NULL, NULL TSRMLS_CC);
}

#define PHP_MSHUT_CALL(i) do { \
	if (SUCCESS != PHP_MSHUTDOWN(i)(type, module_number TSRMLS_CC)) { \
		return FAILURE; \
	} \
} while(0)

static PHP_MSHUTDOWN_FUNCTION(pq)
{
	php_persistent_handle_cleanup(ZEND_STRL("pq\\Connection"), NULL, 0 TSRMLS_CC);

	PHP_MSHUT_CALL(pqlob);
	PHP_MSHUT_CALL(pqcopy);
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
	php_info_print_table_header(2, "Used Library", "Version");
#ifdef HAVE_PQLIBVERSION
	libpq_v = PQlibVersion();
	slprintf(libpq_version, sizeof(libpq_version), "%d.%d.%d", libpq_v/10000%100, libpq_v/100%100, libpq_v%100);
#endif
	php_info_print_table_row(2, "libpq", libpq_version);
	php_info_print_table_end();
}

const zend_function_entry pq_functions[] = {
	{0}
};

static zend_module_dep pq_module_deps[] = {
	ZEND_MOD_REQUIRED("raphf")
	ZEND_MOD_REQUIRED("spl")
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
