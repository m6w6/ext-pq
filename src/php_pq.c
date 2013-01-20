/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2012 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "php_pq.h"
#include <libpq-events.h>

typedef int STATUS; /* SUCCESS/FAILURE */

/*
ZEND_DECLARE_MODULE_GLOBALS(pq)
*/

const zend_function_entry pq_functions[] = {
	{0}
};

/* {{{ pq_module_entry
 */
zend_module_entry pq_module_entry = {
	STANDARD_MODULE_HEADER,
	"pq",
	pq_functions,
	PHP_MINIT(pq),
	PHP_MSHUTDOWN(pq),
	NULL,/*PHP_RINIT(pq),*/
	NULL,/*PHP_RSHUTDOWN(pq),*/
	PHP_MINFO(pq),
	PHP_PQ_EXT_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PQ
ZEND_GET_MODULE(pq)
#endif

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("pq.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_pq_globals, pq_globals)
    STD_PHP_INI_ENTRY("pq.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_pq_globals, pq_globals)
PHP_INI_END()
*/
/* }}} */

/* {{{ php_pq_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_pq_init_globals(zend_pq_globals *pq_globals)
{
	pq_globals->global_value = 0;
	pq_globals->global_string = NULL;
}
*/
/* }}} */

static zend_class_entry *php_pqconn_class_entry;
static zend_class_entry *php_pqres_class_entry;

static zend_object_handlers php_pqconn_object_handlers;
static zend_object_handlers php_pqres_object_handlers;

typedef struct php_pqconn_object {
	zend_object zo;
	PGconn *conn;
	int (*poller)(PGconn *);
	unsigned blocking:1;
} php_pqconn_object_t;

typedef struct php_pqres_object {
	zend_object zo;
	PGresult *res;
} php_pqres_object_t;

typedef enum php_pqres_fetch {
	PHP_PQRES_FETCH_ARRAY,
	PHP_PQRES_FETCH_ASSOC,
	PHP_PQRES_FETCH_OBJECT
} php_pqres_fetch_t;

static void php_pqconn_object_free(void *o TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	if (obj->conn) {
		PQfinish(obj->conn);
		obj->conn = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static void php_pqres_object_free(void *o TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	if (obj->res) {
		PQclear(obj->res);
		obj->res = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static zend_object_value php_pqconn_create_object_ex(zend_class_entry *ce, PGconn *conn, php_pqconn_object_t **ptr TSRMLS_DC)
{
	zend_object_value ov;
	php_pqconn_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);

	if (ptr) {
		*ptr = o;
	}

	if (conn) {
		o->conn = conn;
		o->blocking = !PQisnonblocking(o->conn);
	}

	ov.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqconn_object_free, NULL TSRMLS_CC);
	ov.handlers = &php_pqconn_object_handlers;

	return ov;
}

static zend_object_value php_pqres_create_object_ex(zend_class_entry *ce, PGresult *res, php_pqres_object_t **ptr TSRMLS_DC)
{
	zend_object_value ov;
	php_pqres_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);

	if (ptr) {
		*ptr = o;
	}

	if (res) {
		o->res = res;
	}

	ov.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqres_object_free, NULL TSRMLS_CC);
	ov.handlers = &php_pqres_object_handlers;

	return ov;
}

static zend_object_value php_pqconn_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqconn_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqres_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqres_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static HashTable php_pqconn_object_prophandlers;
static HashTable php_pqres_object_prophandlers;

typedef void (*php_pq_object_prophandler_func_t)(void *o, zval *return_value TSRMLS_DC);

typedef struct php_pq_object_prophandler {
	php_pq_object_prophandler_func_t read;
	php_pq_object_prophandler_func_t write;
} php_pq_object_prophandler_t;

static void php_pqconn_object_read_status(void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQstatus(obj->conn));
}

static void php_pqconn_object_read_transaction_status(void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQtransactionStatus(obj->conn));
}

static void php_pqconn_object_read_socket(void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	php_stream *stream;
	int socket = PQsocket(obj->conn);

	if ((stream = php_stream_fopen_from_fd(socket, "r+b", NULL))) {
		php_stream_to_zval(stream, return_value);
	} else {
		RETVAL_NULL();
	}
}

static void php_pqconn_object_read_error_message(void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *error = PQerrorMessage(obj->conn);

	if (error) {
		RETVAL_STRING(error, 1);
	} else {
		RETVAL_NULL();
	}
}

static void php_pqres_object_read_status(void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQresultStatus(obj->res));
}

static void php_pqres_object_read_error_message(void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;
	char *error = PQresultErrorMessage(obj->res);

	if (error) {
		RETVAL_STRING(error, 1);
	} else {
		RETVAL_NULL();
	}
}

static void php_pqres_object_read_num_rows(void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQntuples(obj->res));
}

static void php_pqres_object_read_num_cols(void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQnfields(obj->res));
}

static void php_pqres_object_read_affected_rows(void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(atoi(PQcmdTuples(obj->res)));
}

static zval *php_pqconn_object_read_prop(zval *object, zval *member, int type, const zend_literal *key TSRMLS_DC)
{
	php_pqconn_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;
	zval *return_value;

	if (!obj->conn) {
		zend_error(E_WARNING, "Connection not initialized");
	} else if (SUCCESS == zend_hash_find(&php_pqconn_object_prophandlers, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (type == BP_VAR_R) {
			ALLOC_ZVAL(return_value);
			Z_SET_REFCOUNT_P(return_value, 0);
			Z_UNSET_ISREF_P(return_value);

			handler->read(obj, return_value TSRMLS_CC);
		} else {
			zend_error(E_ERROR, "Cannot access pq\\Connection properties by reference or array key/index");
			return_value = NULL;
		}
	} else {
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	}

	return return_value;
}

static void php_pqconn_object_write_prop(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC)
{
	php_pqconn_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;

	if (zend_hash_find(&php_pqconn_object_prophandlers, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (handler->write) {
			handler->write(obj, value TSRMLS_CC);
		}
	} else {
		zend_get_std_object_handlers()->write_property(object, member, value, key TSRMLS_CC);
	}
}

static zval *php_pqres_object_read_prop(zval *object, zval *member, int type, const zend_literal *key TSRMLS_DC)
{
	php_pqres_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;
	zval *return_value;

	if (!obj->res) {
		zend_error(E_WARNING, "Result not initialized");
	} else if (SUCCESS == zend_hash_find(&php_pqres_object_prophandlers, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (type == BP_VAR_R) {
			ALLOC_ZVAL(return_value);
			Z_SET_REFCOUNT_P(return_value, 0);
			Z_UNSET_ISREF_P(return_value);

			handler->read(obj, return_value TSRMLS_CC);
		} else {
			zend_error(E_ERROR, "Cannot access pq\\Result properties by reference or array key/index");
			return_value = NULL;
		}
	} else {
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	}

	return return_value;
}

static void php_pqres_object_write_prop(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC)
{
	php_pqres_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;

	if (!obj->res) {
		zend_error(E_WARNING, "Result not initialized");
	} else if (zend_hash_find(&php_pqres_object_prophandlers, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (handler->write) {
			handler->write(obj, value TSRMLS_CC);
		}
	} else {
		zend_get_std_object_handlers()->write_property(object, member, value, key TSRMLS_CC);
	}
}

typedef struct php_pqres_iterator {
	zend_object_iterator zi;
	zval *current_val;
	unsigned index;
} php_pqres_iterator_t;

static zend_object_iterator_funcs php_pqres_iterator_funcs;

static zend_object_iterator *php_pqres_iterator_init(zend_class_entry *ce, zval *object, int by_ref TSRMLS_DC)
{
	php_pqres_iterator_t *iter;

	iter = ecalloc(1, sizeof(*iter));
	iter->zi.funcs = &php_pqres_iterator_funcs;
	iter->zi.data = object;
	Z_ADDREF_P(object);

	return (zend_object_iterator *) iter;
}

static void php_pqres_iterator_dtor(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	if (iter->current_val) {
		zval_ptr_dtor(&iter->current_val);
	}
	zval_ptr_dtor((zval **) &iter->zi.data);
	efree(iter);
}

static STATUS php_pqres_iterator_valid(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;
	php_pqres_object_t *obj = zend_object_store_get_object(iter->zi.data TSRMLS_CC);

	if (PQresultStatus(obj->res) != PGRES_TUPLES_OK) {
		return FAILURE;
	}
	if (PQntuples(obj->res) <= iter->index) {
		return FAILURE;
	}

	return SUCCESS;
}

static zval *php_pqres_row_to_zval(PGresult *res, unsigned row)
{
	zval *data;
	int c, cols;

	MAKE_STD_ZVAL(data);
	array_init(data);

	for (c = 0, cols = PQnfields(res); c < cols; ++c) {
		if (PQgetisnull(res, row, c)) {
			add_index_null(data, c);
			add_assoc_null(data, PQfname(res, c));
		} else {
			char *val = PQgetvalue(res, row, c);
			int len = PQgetlength(res, row, c);

			add_index_stringl(data, c, val, len ,1);
			add_assoc_stringl(data, PQfname(res, c), val, len, 1);
		}
	}

	return data;
}

static void php_pqres_iterator_current(zend_object_iterator *i, zval ***data_ptr TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;
	php_pqres_object_t *obj = zend_object_store_get_object(iter->zi.data TSRMLS_CC);

	if (iter->current_val) {
		zval_ptr_dtor(&iter->current_val);
	}
	iter->current_val = php_pqres_row_to_zval(obj->res, iter->index);
	*data_ptr = &iter->current_val;
}

static int php_pqres_iterator_key(zend_object_iterator *i, char **key_str, uint *key_len, ulong *key_num TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	*key_num = (ulong) iter->index;

	return HASH_KEY_IS_LONG;
}

static void php_pqres_iterator_next(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	++iter->index;
}

static void php_pqres_iterator_rewind(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	iter->index = 0;
}

static zend_object_iterator_funcs php_pqres_iterator_funcs = {
	php_pqres_iterator_dtor,
	/* check for end of iteration (FAILURE or SUCCESS if data is valid) */
	php_pqres_iterator_valid,
	/* fetch the item data for the current element */
	php_pqres_iterator_current,
	/* fetch the key for the current element (return HASH_KEY_IS_STRING or HASH_KEY_IS_LONG) (optional, may be NULL) */
	php_pqres_iterator_key,
	/* step forwards to next element */
	php_pqres_iterator_next,
	/* rewind to start of data (optional, may be NULL) */
	php_pqres_iterator_rewind,
	/* invalidate current value/key (optional, may be NULL) */
	NULL
};

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_construct, 0, 0, 1)
	ZEND_ARG_INFO(0, dsn)
	ZEND_ARG_INFO(0, block)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, __construct) {
	zend_error_handling zeh;
	char *dsn_str;
	int dsn_len;
	zend_bool block = 1;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|b", &dsn_str, &dsn_len, &block)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			PQfinish(obj->conn);
		}
		if ((obj->blocking = block)) {
			obj->conn = PQconnectdb(dsn_str);
		} else {
			obj->conn = PQconnectStart(dsn_str);
			obj->poller = (int (*)(PGconn*)) PQconnectPoll;
		}

		if (CONNECTION_BAD == PQstatus(obj->conn)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection failed: %s", PQerrorMessage(obj->conn));
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_reset, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, reset) {
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			if (obj->blocking) {
				PQreset(obj->conn);
				RETURN_TRUE; /* probably ;) */
			} if (PQresetStart(obj->conn)) {
				obj->poller = (int (*)(PGconn*)) PQresetPoll;
				RETURN_TRUE;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
		}
		RETURN_FALSE;
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_poll, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, poll) {
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			if (obj->poller) {
				RETURN_LONG(obj->poller(obj->conn));
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "No asynchronous operation active");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
		}
		RETURN_FALSE;
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, exec) {
	zend_error_handling zeh;
	char *query_str;
	int query_len;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &query_str, &query_len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->conn) {
			PGresult *res = PQexec(obj->conn, query_str);

			if (res) {
				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqres_create_object_ex(php_pqres_class_entry, res, NULL TSRMLS_CC);
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not execute query: %s", PQerrorMessage(obj->conn));
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static zend_function_entry pqconn_methods[] = {
	PHP_ME(pqconn, __construct, ai_pqconn_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqconn, reset, ai_pqconn_reset, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, poll, ai_pqconn_poll, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, exec, ai_pqconn_exec, ZEND_ACC_PUBLIC)
	{0}
};

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch, 0, 0, 0)
	ZEND_ARG_INFO(0, fetch_type)
	ZEND_ARG_INFO(0, fetch_info)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchRow) {
	zend_error_handling zeh;
	long fetch_type = PHP_PQRES_FETCH_OBJECT;
	zval *fetch_info = NULL;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lz", &fetch_type, &fetch_info)) {

	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static zend_function_entry pqres_methods[] = {
	PHP_ME(pqres, fetchRow, ai_pqres_fetch, ZEND_ACC_PUBLIC)
	{0}
};

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(pq)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	zend_hash_init(&php_pqconn_object_prophandlers, 1, NULL, NULL, 1);
	INIT_NS_CLASS_ENTRY(ce, "pq", "Connection", pqconn_methods);
	php_pqconn_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqconn_class_entry->create_object = php_pqconn_create_object;
	memcpy(&php_pqconn_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqconn_object_handlers.read_property = php_pqconn_object_read_prop;
	php_pqconn_object_handlers.write_property = php_pqconn_object_write_prop;

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("status"), CONNECTION_BAD, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_status;
	zend_hash_add(&php_pqconn_object_prophandlers, "status", sizeof("status"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("transactionStatus"), PQTRANS_UNKNOWN, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_transaction_status;
	zend_hash_add(&php_pqconn_object_prophandlers, "transactionStatus", sizeof("transactionStatus"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("socket"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_socket;
	zend_hash_add(&php_pqconn_object_prophandlers, "socket", sizeof("socket"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("errorMessage"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_error_message;
	zend_hash_add(&php_pqconn_object_prophandlers, "errorMessage", sizeof("errorMessage"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("OK"), CONNECTION_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("BAD"), CONNECTION_BAD TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("STARTED"), CONNECTION_STARTED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("MADE"), CONNECTION_MADE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("AWAITING_RESPONSE"), CONNECTION_AWAITING_RESPONSE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("AUTH_OK"), CONNECTION_AUTH_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("SSL_STARTUP"), CONNECTION_SSL_STARTUP TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("SETENV"), CONNECTION_SETENV TSRMLS_CC);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_IDLE"), PQTRANS_IDLE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_ACTIVE"), PQTRANS_ACTIVE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_INTRANS"), PQTRANS_INTRANS TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_INERROR"), PQTRANS_INERROR TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_UNKNOWN"), PQTRANS_UNKNOWN TSRMLS_CC);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_FAILED"), PGRES_POLLING_FAILED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_READING"), PGRES_POLLING_READING TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_WRITING"), PGRES_POLLING_WRITING TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_OK"), PGRES_POLLING_OK TSRMLS_CC);

	zend_hash_init(&php_pqres_object_prophandlers, 1, NULL, NULL, 1);
	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Result", pqres_methods);
	php_pqres_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqres_class_entry->create_object = php_pqres_create_object;
	php_pqres_class_entry->iterator_funcs.funcs = &php_pqres_iterator_funcs;
	php_pqres_class_entry->get_iterator = php_pqres_iterator_init;

	memcpy(&php_pqres_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqres_object_handlers.read_property = php_pqres_object_read_prop;
	php_pqres_object_handlers.write_property = php_pqres_object_write_prop;

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("status"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_status;
	zend_hash_add(&php_pqres_object_prophandlers, "status", sizeof("status"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("errorMessage"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_error_message;
	zend_hash_add(&php_pqres_object_prophandlers, "errorMessage", sizeof("errorMessage"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("numRows"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_num_rows;
	zend_hash_add(&php_pqres_object_prophandlers, "numRows", sizeof("numRows"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("numCols"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_num_cols;
	zend_hash_add(&php_pqres_object_prophandlers, "numCols", sizeof("numCols"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("affectedRows"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_affected_rows;
	zend_hash_add(&php_pqres_object_prophandlers, "affectedRows", sizeof("affectedRows"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("EMPTY_QUERY"), PGRES_EMPTY_QUERY TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COMMAND_OK"), PGRES_COMMAND_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("TUPLES_OK"), PGRES_TUPLES_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_OUT"), PGRES_COPY_OUT TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_IN"), PGRES_COPY_IN TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("BAD_RESPONSE"), PGRES_BAD_RESPONSE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("NONFATAL_ERROR"), PGRES_NONFATAL_ERROR TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FATAL_ERROR"), PGRES_FATAL_ERROR TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_BOTH"), PGRES_COPY_BOTH TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("SINGLE_TUPLE"), PGRES_SINGLE_TUPLE TSRMLS_CC);

	/*
	REGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(pq)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(pq)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "pq support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */



/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
