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

#include <libpq-events.h>

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pq_object.h"
#include "php_pqconn_event.h"
#include "php_pqres.h"

static int apply_event(zval *p, void *a)
{
	php_pq_callback_t *cb = Z_PTR_P(p);
	zval *args = a;
	zval rv;

	ZVAL_NULL(&rv);
	zend_fcall_info_args(&cb->fci, args);
	zend_fcall_info_call(&cb->fci, &cb->fcc, &rv, NULL);
	zend_fcall_info_args_clear(&cb->fci, 0);
	zval_ptr_dtor(&rv);

	return ZEND_HASH_APPLY_KEEP;
}

static void php_pqconn_event_connreset(PGEventConnReset *event)
{
	php_pqconn_event_data_t *data = PQinstanceData(event->conn, php_pqconn_event);

	if (data) {
		zval *zevhs;

		if ((zevhs = zend_hash_str_find(&data->obj->intern->eventhandlers, ZEND_STRL("reset")))) {
			zval args, connection;

			array_init(&args);
			php_pq_object_to_zval(data->obj, &connection);
			add_next_index_zval(&args, &connection);
			zend_hash_apply_with_argument(Z_ARRVAL_P(zevhs), apply_event, &args);
			zval_ptr_dtor(&args);
		}
	}
}

static void php_pqconn_event_resultcreate(PGEventResultCreate *event)
{
	php_pqconn_event_data_t *data = PQinstanceData(event->conn, php_pqconn_event);

	if (data) {
		php_pqres_object_t *obj = php_pqres_init_instance_data(event->result, data->obj);
		zval *zevhs;

		/* event listener */
		if ((zevhs = zend_hash_str_find(&data->obj->intern->eventhandlers, ZEND_STRL("result")))) {
			zval args, connection, res;

			array_init(&args);
			php_pq_object_to_zval(data->obj, &connection);
			add_next_index_zval(&args, &connection);
			php_pq_object_to_zval(obj, &res);
			add_next_index_zval(&args, &res);
			zend_hash_apply_with_argument(Z_ARRVAL_P(zevhs), apply_event, &args);
			zval_ptr_dtor(&args);
		}

		/* async callback */
		if (php_pq_callback_is_enabled(&data->obj->intern->onevent)) {
			zval res;

			php_pq_object_to_zval(obj, &res);
			zend_fcall_info_argn(&data->obj->intern->onevent.fci, 1, &res);
			zend_fcall_info_call(&data->obj->intern->onevent.fci, &data->obj->intern->onevent.fcc, NULL, NULL);
			zval_ptr_dtor(&res);
		}

	}
}

static void php_pqconn_event_resultdestroy(PGEventResultDestroy *event)
{
	php_pqres_object_t *obj = PQresultInstanceData(event->result, php_pqconn_event);

	if (obj) {
		obj->intern->res = NULL;
		assert(GC_REFCOUNT(&obj->zo));
		php_pq_object_delref(obj);
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

php_pqconn_event_data_t *php_pqconn_event_data_init(php_pqconn_object_t *obj)
{
	php_pqconn_event_data_t *data = emalloc(sizeof(*data));

	data->obj = obj;
	data->res = NULL;

	return data;
}

void php_pqconn_notice_recv(void *p, const PGresult *res)
{
	php_pqconn_event_data_t *data = p;

	if (data) {
		zval *zevhs;

		if ((zevhs = zend_hash_str_find(&data->obj->intern->eventhandlers, ZEND_STRL("notice")))) {
			zval args, connection;

			array_init(&args);
			php_pq_object_to_zval(data->obj, &connection);
			add_next_index_zval(&args, &connection);
			add_next_index_string(&args, PHP_PQresultErrorMessage(res));
			zend_hash_apply_with_argument(Z_ARRVAL_P(zevhs), apply_event, &args);
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
