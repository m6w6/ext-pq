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

#define SMART_STR_PREALLOC 256
#include <ext/standard/php_smart_str.h>

#include <libpq-events.h>

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pq_object.h"
#include "php_pqconn_event.h"
#include "php_pqstm.h"
#include "php_pqres.h"

static int apply_event(void *p, void *a TSRMLS_DC)
{
	php_pq_callback_t *cb = p;
	zval *args = a;
	zval *retval = NULL;

	zend_fcall_info_args(&cb->fci, args TSRMLS_CC);
	zend_fcall_info_call(&cb->fci, &cb->fcc, &retval, NULL TSRMLS_CC);
	if (retval) {
		zval_ptr_dtor(&retval);
	}

	return ZEND_HASH_APPLY_KEEP;
}


static inline PGresult *relisten(PGconn *conn, const char *channel_str, size_t channel_len TSRMLS_DC)
{
	char *quoted_channel = PQescapeIdentifier(conn, channel_str, channel_len);
	PGresult *res = NULL;

	if (quoted_channel) {
		smart_str cmd = {0};

		smart_str_appends(&cmd, "LISTEN ");
		smart_str_appends(&cmd, quoted_channel);
		smart_str_0(&cmd);

		res = PQexec(conn, cmd.c);

		smart_str_free(&cmd);
		PQfreemem(quoted_channel);
	}

	return res;
}

static int apply_relisten(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	php_pqconn_object_t *obj = va_arg(argv, php_pqconn_object_t *);
	PGresult *res = relisten(obj->intern->conn, key->arKey, key->nKeyLength - 1 TSRMLS_CC);

	if (res) {
		php_pqres_clear(res);
	}

	return ZEND_HASH_APPLY_KEEP;
}

static int apply_reprepare(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	php_pqconn_object_t *obj = va_arg(argv, php_pqconn_object_t *);
	php_pqstm_t *stm = *(php_pqstm_object_t **) p;

	php_pqconn_prepare(NULL, obj, stm->name, stm->query, stm->params TSRMLS_CC);

	return ZEND_HASH_APPLY_KEEP;
}

static void php_pqconn_event_connreset(PGEventConnReset *event)
{
	php_pqconn_event_data_t *data = PQinstanceData(event->conn, php_pqconn_event);

	if (data) {
		HashTable *evhs;
		TSRMLS_DF(data);

		/* restore listeners */
		zend_hash_apply_with_arguments(&data->obj->intern->listeners TSRMLS_CC, apply_relisten, 1, data->obj);

		/* restore statements */
		zend_hash_apply_with_arguments(&data->obj->intern->statements TSRMLS_CC, apply_reprepare, 1, data->obj);

		/* eventhandler */
		if (SUCCESS == zend_hash_find(&data->obj->intern->eventhandlers, ZEND_STRS("reset"), (void *) &evhs)) {
			zval *args, *connection = NULL;

			MAKE_STD_ZVAL(args);
			array_init(args);
			php_pq_object_to_zval(data->obj, &connection TSRMLS_CC);
			add_next_index_zval(args, connection);
			zend_hash_apply_with_argument(evhs, apply_event, args TSRMLS_CC);
			zval_ptr_dtor(&args);
		}
	}
}

static void php_pqconn_event_resultcreate(PGEventResultCreate *event)
{
	php_pqconn_event_data_t *data = PQinstanceData(event->conn, php_pqconn_event);

	if (data) {
		php_pqres_object_t *obj;
		HashTable *evhs;
		TSRMLS_DF(data);

		php_pqres_init_instance_data(event->result, data->obj, &obj TSRMLS_CC);

		/* event listener */
		if (SUCCESS == zend_hash_find(&data->obj->intern->eventhandlers, ZEND_STRS("result"), (void *) &evhs)) {
			zval *args, *connection = NULL, *res = NULL;

			MAKE_STD_ZVAL(args);
			array_init(args);
			php_pq_object_to_zval(data->obj, &connection TSRMLS_CC);
			add_next_index_zval(args, connection);
			php_pq_object_to_zval(obj, &res TSRMLS_CC);
			add_next_index_zval(args, res);
			zend_hash_apply_with_argument(evhs, apply_event, args TSRMLS_CC);
			zval_ptr_dtor(&args);
		}

		/* async callback */
		if (php_pq_callback_is_enabled(&data->obj->intern->onevent)) {
			zval *res = NULL;

			php_pq_object_to_zval(obj, &res TSRMLS_CC);
			zend_fcall_info_argn(&data->obj->intern->onevent.fci TSRMLS_CC, 1, &res);
			zend_fcall_info_call(&data->obj->intern->onevent.fci, &data->obj->intern->onevent.fcc, NULL, NULL TSRMLS_CC);
			zval_ptr_dtor(&res);
		}

	}
}

static void php_pqconn_event_resultdestroy(PGEventResultDestroy *event)
{
	php_pqres_object_t *obj = PQresultInstanceData(event->result, php_pqconn_event);

	if (obj) {
		obj->intern->res = NULL;
	}
}

int php_pqconn_event(PGEventId id, void *e, void *data)
{
	switch (id) {
	case PGEVT_CONNRESET:
		php_pqconn_event_connreset(e);
		break;
	case PGEVT_RESULTCREATE:
		php_pqconn_event_resultcreate(e);
		break;
	case PGEVT_RESULTDESTROY:
		php_pqconn_event_resultdestroy(e);
		break;
	default:
		break;
	}

	return 1;
}

php_pqconn_event_data_t *php_pqconn_event_data_init(php_pqconn_object_t *obj TSRMLS_DC)
{
	php_pqconn_event_data_t *data = emalloc(sizeof(*data));

	data->obj = obj;
	TSRMLS_CF(data);

	return data;
}

void php_pqconn_notice_recv(void *p, const PGresult *res)
{
	php_pqconn_event_data_t *data = p;

	if (data) {
		HashTable *evhs;
		TSRMLS_DF(data);

		if (SUCCESS == zend_hash_find(&data->obj->intern->eventhandlers, ZEND_STRS("notice"), (void *) &evhs)) {
			zval *args, *connection = NULL;

			MAKE_STD_ZVAL(args);
			array_init(args);
			php_pq_object_to_zval(data->obj, &connection TSRMLS_CC);
			add_next_index_zval(args, connection);
			add_next_index_string(args, PHP_PQresultErrorMessage(res), 1);
			zend_hash_apply_with_argument(evhs, apply_event, args TSRMLS_CC);
			zval_ptr_dtor(&args);
		}
	}
}

void php_pqconn_notice_ignore(void *p, const PGresult *res)
{
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
