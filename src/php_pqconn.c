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
#include <fnmatch.h>

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pq_object.h"
#include "php_pqexc.h"
#include "php_pqconn.h"
#include "php_pqconn_event.h"
#include "php_pqres.h"
#include "php_pqstm.h"
#include "php_pqtxn.h"
#include "php_pqcur.h"

zend_class_entry *php_pqconn_class_entry;
static zend_object_handlers php_pqconn_object_handlers;
static HashTable php_pqconn_object_prophandlers;

/*
static void php_pqconn_del_eventhandler(php_pqconn_object_t *obj, const char *type_str, size_t type_len, ulong id TSRMLS_DC)
{
	zval **evhs;

	if (SUCCESS == zend_hash_find(&obj->intern->eventhandlers, type_str, type_len + 1, (void *) &evhs)) {
		zend_hash_index_del(Z_ARRVAL_PP(evhs), id);
	}
}
*/

static ulong php_pqconn_add_eventhandler(php_pqconn_object_t *obj, const char *type_str, size_t type_len, php_pq_callback_t *cb TSRMLS_DC)
{
	ulong h;
	HashTable *evhs;

	if (SUCCESS != zend_hash_find(&obj->intern->eventhandlers, type_str, type_len + 1, (void *) &evhs)) {
		HashTable evh;

		zend_hash_init(&evh, 1, NULL, (dtor_func_t) php_pq_callback_dtor, 0);
		zend_hash_add(&obj->intern->eventhandlers, type_str, type_len + 1, (void *) &evh, sizeof(evh), (void *) &evhs);
	}

	php_pq_callback_addref(cb);
	h = zend_hash_next_free_element(evhs);
	zend_hash_index_update(evhs, h, (void *) cb, sizeof(*cb), NULL);

	return h;
}

static void php_pqconn_object_free(void *o TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE conn(#%d) %p\n", obj->zv.handle, obj);
#endif
	if (obj->intern) {
		php_pq_callback_dtor(&obj->intern->onevent);
		php_resource_factory_handle_dtor(&obj->intern->factory, obj->intern->conn TSRMLS_CC);
		php_resource_factory_dtor(&obj->intern->factory);
		zend_hash_destroy(&obj->intern->listeners);
		zend_hash_destroy(&obj->intern->converters);
		zend_hash_destroy(&obj->intern->eventhandlers);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}


zend_object_value php_pqconn_create_object_ex(zend_class_entry *ce, php_pqconn_t *intern, php_pqconn_object_t **ptr TSRMLS_DC)
{
	php_pqconn_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqconn_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqconn_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqconn_object_handlers;

	return o->zv;
}

static zend_object_value php_pqconn_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqconn_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static void php_pqconn_object_read_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQstatus(obj->intern->conn));
}

static void php_pqconn_object_read_transaction_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQtransactionStatus(obj->intern->conn));
}

static void php_pqconn_object_read_error_message(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *error = PHP_PQerrorMessage(obj->intern->conn);

	if (error) {
		RETVAL_STRING(error, 1);
	} else {
		RETVAL_NULL();
	}
}

static int apply_notify_listener(void *p, void *arg TSRMLS_DC)
{
	php_pq_callback_t *listener = p;
	PGnotify *nfy = arg;
	zval *zpid, *zchannel, *zmessage;

	MAKE_STD_ZVAL(zpid);
	ZVAL_LONG(zpid, nfy->be_pid);
	MAKE_STD_ZVAL(zchannel);
	ZVAL_STRING(zchannel, nfy->relname, 1);
	MAKE_STD_ZVAL(zmessage);
	ZVAL_STRING(zmessage, nfy->extra, 1);

	zend_fcall_info_argn(&listener->fci TSRMLS_CC, 3, &zchannel, &zmessage, &zpid);
	zend_fcall_info_call(&listener->fci, &listener->fcc, NULL, NULL TSRMLS_CC);

	zval_ptr_dtor(&zchannel);
	zval_ptr_dtor(&zmessage);
	zval_ptr_dtor(&zpid);

	return ZEND_HASH_APPLY_KEEP;
}

static int apply_notify_listeners(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	HashTable *listeners = p;
	PGnotify *nfy = va_arg(argv, PGnotify *);

	if (0 == fnmatch(key->arKey, nfy->relname, 0)) {
		zend_hash_apply_with_argument(listeners, apply_notify_listener, nfy TSRMLS_CC);
	}

	return ZEND_HASH_APPLY_KEEP;
}

void php_pqconn_notify_listeners(php_pqconn_object_t *obj TSRMLS_DC)
{
	PGnotify *nfy;

	while ((nfy = PQnotifies(obj->intern->conn))) {
		zend_hash_apply_with_arguments(&obj->intern->listeners TSRMLS_CC, apply_notify_listeners, 1, nfy);
		PQfreemem(nfy);
	}
}

static void php_pqconn_object_read_busy(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(PQisBusy(obj->intern->conn));
}

static void php_pqconn_object_read_encoding(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_STRING(pg_encoding_to_char(PQclientEncoding(obj->intern->conn)), 1);
}

static void php_pqconn_object_write_encoding(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	zval *zenc = value;

	if (Z_TYPE_P(value) != IS_STRING) {
		if (Z_REFCOUNT_P(value) > 1) {
			zval *tmp;
			MAKE_STD_ZVAL(tmp);
			ZVAL_ZVAL(tmp, zenc, 1, 0);
			convert_to_string(tmp);
			zenc = tmp;
		} else {
			convert_to_string_ex(&zenc);
		}
	}

	if (0 > PQsetClientEncoding(obj->intern->conn, Z_STRVAL_P(zenc))) {
		php_error(E_NOTICE, "Unrecognized encoding '%s'", Z_STRVAL_P(zenc));
	}

	if (zenc != value) {
		zval_ptr_dtor(&zenc);
	}
}

static void php_pqconn_object_read_unbuffered(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->unbuffered);
}

static void php_pqconn_object_write_unbuffered(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	obj->intern->unbuffered = z_is_true(value);
}

static void php_pqconn_object_read_db(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *db = PQdb(obj->intern->conn);

	if (db) {
		RETVAL_STRING(db, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_user(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *user = PQuser(obj->intern->conn);

	if (user) {
		RETVAL_STRING(user, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_pass(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *pass = PQpass(obj->intern->conn);

	if (pass) {
		RETVAL_STRING(pass, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_host(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *host = PQhost(obj->intern->conn);

	if (host) {
		RETVAL_STRING(host, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_port(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *port = PQport(obj->intern->conn);

	if (port) {
		RETVAL_STRING(port, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

#if HAVE_PQCONNINFO
static void php_pqconn_object_read_params(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	PQconninfoOption *ptr, *params = PQconninfo(obj->intern->conn);

	array_init(return_value);

	if (params) {
		for (ptr = params; ptr->keyword; ++ptr) {
			if (ptr->val) {
				add_assoc_string(return_value, ptr->keyword, ptr->val, 1);
			} else {
				add_assoc_null(return_value, ptr->keyword);
			}
		}
		PQconninfoFree(params);
	}
}
#endif

static void php_pqconn_object_read_options(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *options = PQoptions(obj->intern->conn);

	if (options) {
		RETVAL_STRING(options, 1);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static int apply_read_event_handler_ex(void *p, void *arg TSRMLS_DC)
{
	HashTable *rv = arg;
	zval *zcb = php_pq_callback_to_zval(p);

	zend_hash_next_index_insert(rv, &zcb, sizeof(zval *), NULL);

	return ZEND_HASH_APPLY_KEEP;
}

static int apply_read_event_handlers(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	HashTable *evhs = p, *rv = va_arg(argv, HashTable *);
	zval *entry, **entry_ptr;

	MAKE_STD_ZVAL(entry);
	array_init_size(entry, zend_hash_num_elements(evhs));

	if (key->nKeyLength) {
		zend_hash_add(rv, key->arKey, key->nKeyLength, &entry, sizeof(zval *), (void *) &entry_ptr);
	} else {
		zend_hash_index_update(rv, key->h, &entry, sizeof(zval *), (void *) &entry_ptr);
	}

	zend_hash_apply_with_argument(evhs, apply_read_event_handler_ex, Z_ARRVAL_PP(entry_ptr) TSRMLS_CC);

	return ZEND_HASH_APPLY_KEEP;
}
static void php_pqconn_object_read_event_handlers(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	array_init(return_value);
	zend_hash_apply_with_arguments(&obj->intern->eventhandlers TSRMLS_CC, apply_read_event_handlers, 1, Z_ARRVAL_P(return_value) TSRMLS_CC);
}

static void php_pqconn_object_read_def_fetch_type(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(obj->intern->default_fetch_type);
}
static void php_pqconn_object_write_def_fetch_type(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	zval *zft = value;

	if (Z_TYPE_P(zft) != IS_LONG) {
		if (Z_REFCOUNT_P(zft) > 1) {
			zval *tmp;
			MAKE_STD_ZVAL(tmp);
			ZVAL_ZVAL(tmp, zft, 1, 0);
			convert_to_long(tmp);
			zft = tmp;
		} else {
			convert_to_long_ex(&zft);
		}
	}

	obj->intern->default_fetch_type = Z_LVAL_P(zft) & 0x3; /* two bits only */

	if (zft != value) {
		zval_ptr_dtor(&zft);
	}
}

static void php_pqconn_object_read_def_txn_isolation(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(obj->intern->default_txn_isolation);
}
static void php_pqconn_object_write_def_txn_isolation(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	zval *zti = value;

	if (Z_TYPE_P(zti) != IS_LONG) {
		if (Z_REFCOUNT_P(zti) > 1) {
			zval *tmp;
			MAKE_STD_ZVAL(tmp);
			ZVAL_ZVAL(tmp, zti, 1, 0);
			convert_to_long(tmp);
			zti = tmp;
		} else {
			convert_to_long_ex(&zti);
		}
	}

	obj->intern->default_txn_isolation = Z_LVAL_P(zti) & 0x3; /* two bits only */

	if (zti != value) {
		zval_ptr_dtor(&zti);
	}
}

static void php_pqconn_object_read_def_txn_readonly(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->default_txn_readonly);
}
static void php_pqconn_object_write_def_txn_readonly(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	obj->intern->default_txn_readonly = zend_is_true(value);
}

static void php_pqconn_object_read_def_txn_deferrable(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->default_txn_deferrable);
}
static void php_pqconn_object_write_def_txn_deferrable(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	obj->intern->default_txn_deferrable = zend_is_true(value);
}

static void php_pqconn_object_read_def_auto_conv(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(obj->intern->default_auto_convert);
}
static void php_pqconn_object_write_def_auto_conv(zval*object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	zval *zac = value;

	if (Z_TYPE_P(zac) != IS_LONG) {
		if (Z_REFCOUNT_P(zac) > 1) {
			zval *tmp;
			MAKE_STD_ZVAL(tmp);
			ZVAL_ZVAL(tmp, zac, 1, 0);
			convert_to_long(tmp);
			zac = tmp;
		} else {
			convert_to_long_ex(&zac);
		}
	}

	obj->intern->default_auto_convert = Z_LVAL_P(zac) & PHP_PQRES_CONV_ALL;

	if (zac != value) {
		zval_ptr_dtor(&zac);
	}
}

static STATUS php_pqconn_update_socket(zval *this_ptr, php_pqconn_object_t *obj TSRMLS_DC)
{
	zval *zsocket, zmember;
	php_stream *stream;
	STATUS retval;
	int socket;

	if (!obj) {
		obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	}

	INIT_PZVAL(&zmember);
	ZVAL_STRINGL(&zmember, "socket", sizeof("socket")-1, 0);
	MAKE_STD_ZVAL(zsocket);

	if ((CONNECTION_BAD != PQstatus(obj->intern->conn))
	&&	(-1 < (socket = PQsocket(obj->intern->conn)))
	&&	(stream = php_stream_fopen_from_fd(socket, "r+b", NULL))) {
		stream->flags |= PHP_STREAM_FLAG_NO_CLOSE;
		php_stream_to_zval(stream, zsocket);
		retval = SUCCESS;
	} else {
		ZVAL_NULL(zsocket);
		retval = FAILURE;
	}
	zend_get_std_object_handlers()->write_property(getThis(), &zmember, zsocket, NULL TSRMLS_CC);
	zval_ptr_dtor(&zsocket);

	return retval;
}

static void *php_pqconn_resource_factory_ctor(void *data, void *init_arg TSRMLS_DC)
{
	php_pqconn_resource_factory_data_t *o = init_arg;
	PGconn *conn = NULL;;

	if (o->flags & PHP_PQCONN_ASYNC) {
		conn = PQconnectStart(o->dsn);
	} else {
		conn = PQconnectdb(o->dsn);
	}

	if (conn) {
		PQregisterEventProc(conn, php_pqconn_event, "ext-pq", NULL);
	}

	return conn;
}

static void php_pqconn_resource_factory_dtor(void *opaque, void *handle TSRMLS_DC)
{
	php_pqconn_event_data_t *evdata = PQinstanceData(handle, php_pqconn_event);

	/* we don't care for anything, except free'ing evdata */
	if (evdata) {
		PQsetInstanceData(handle, php_pqconn_event, NULL);
		memset(evdata, 0, sizeof(*evdata));
		efree(evdata);
	}

	PQfinish(handle);
}

static php_resource_factory_ops_t php_pqconn_resource_factory_ops = {
	php_pqconn_resource_factory_ctor,
	NULL,
	php_pqconn_resource_factory_dtor
};

php_resource_factory_ops_t *php_pqconn_get_resource_factory_ops(void)
{
	return &php_pqconn_resource_factory_ops;
}

static void php_pqconn_wakeup(php_persistent_handle_factory_t *f, void **handle TSRMLS_DC)
{
	PGresult *res = PQexec(*handle, "");
	PHP_PQclear(res);

	if (CONNECTION_OK != PQstatus(*handle)) {
		PQreset(*handle);
	}
}

static inline PGresult *unlisten(PGconn *conn, const char *channel_str, size_t channel_len TSRMLS_DC)
{
	char *quoted_channel = PQescapeIdentifier(conn, channel_str, channel_len);
	PGresult *res = NULL;

	if (quoted_channel) {
		smart_str cmd = {0};

		smart_str_appends(&cmd, "UNLISTEN ");
		smart_str_appends(&cmd, quoted_channel);
		smart_str_0(&cmd);

		res = PQexec(conn, cmd.c);

		smart_str_free(&cmd);
		PQfreemem(quoted_channel);
	}

	return res;
}

static int apply_unlisten(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	php_pqconn_object_t *obj = va_arg(argv, php_pqconn_object_t *);
	PGresult *res = unlisten(obj->intern->conn, key->arKey, key->nKeyLength - 1 TSRMLS_CC);

	if (res) {
		PHP_PQclear(res);
	}

	return ZEND_HASH_APPLY_REMOVE;
}

static void php_pqconn_retire(php_persistent_handle_factory_t *f, void **handle TSRMLS_DC)
{
	php_pqconn_event_data_t *evdata = PQinstanceData(*handle, php_pqconn_event);
	PGcancel *cancel;
	PGresult *res;

	/* go away */
	PQsetInstanceData(*handle, php_pqconn_event, NULL);

	/* ignore notices */
	PQsetNoticeReceiver(*handle, php_pqconn_notice_ignore, NULL);

	/* cancel async queries */
	if (PQisBusy(*handle) && (cancel = PQgetCancel(*handle))) {
		char err[256] = {0};

		PQcancel(cancel, err, sizeof(err));
		PQfreeCancel(cancel);
	}
	/* clean up async results */
	while ((res = PQgetResult(*handle))) {
		PHP_PQclear(res);
	}

	/* clean up transaction & session */
	switch (PQtransactionStatus(*handle)) {
	case PQTRANS_IDLE:
		res = PQexec(*handle, "RESET ALL");
		break;
	default:
		res = PQexec(*handle, "ROLLBACK; RESET ALL");
		break;
	}

	if (res) {
		PHP_PQclear(res);
	}

	if (evdata) {
		/* clean up notify listeners */
		zend_hash_apply_with_arguments(&evdata->obj->intern->listeners TSRMLS_CC, apply_unlisten, 1, evdata->obj);

		/* release instance data */
		efree(evdata);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_construct, 0, 0, 1)
	ZEND_ARG_INFO(0, dsn)
	ZEND_ARG_INFO(0, async)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, __construct) {
	zend_error_handling zeh;
	char *dsn_str = "";
	int dsn_len = 0;
	long flags = 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sl", &dsn_str, &dsn_len, &flags);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			throw_exce(EX_BAD_METHODCALL TSRMLS_CC, "pq\\Connection already initialized");
		} else {
			php_pqconn_event_data_t *evdata =  php_pqconn_event_data_init(obj TSRMLS_CC);
			php_pqconn_resource_factory_data_t rfdata = {dsn_str, flags};

			obj->intern = ecalloc(1, sizeof(*obj->intern));

			obj->intern->default_auto_convert = PHP_PQRES_CONV_ALL;

			zend_hash_init(&obj->intern->listeners, 0, NULL, (dtor_func_t) zend_hash_destroy, 0);
			zend_hash_init(&obj->intern->converters, 0, NULL, ZVAL_PTR_DTOR, 0);
			zend_hash_init(&obj->intern->eventhandlers, 0, NULL, (dtor_func_t) zend_hash_destroy, 0);

			if (flags & PHP_PQCONN_PERSISTENT) {
				php_persistent_handle_factory_t *phf = php_persistent_handle_concede(NULL, ZEND_STRL("pq\\Connection"), dsn_str, dsn_len, php_pqconn_wakeup, php_pqconn_retire TSRMLS_CC);
				php_resource_factory_init(&obj->intern->factory, php_persistent_handle_get_resource_factory_ops(), phf, (void (*)(void*)) php_persistent_handle_abandon);
			} else {
				php_resource_factory_init(&obj->intern->factory, &php_pqconn_resource_factory_ops, NULL, NULL);
			}

			if (flags & PHP_PQCONN_ASYNC) {
				obj->intern->poller = (int (*)(PGconn*)) PQconnectPoll;
			}

			obj->intern->conn = php_resource_factory_handle_ctor(&obj->intern->factory, &rfdata TSRMLS_CC);

			PQsetInstanceData(obj->intern->conn, php_pqconn_event, evdata);
			PQsetNoticeReceiver(obj->intern->conn, php_pqconn_notice_recv, evdata);

			if (SUCCESS != php_pqconn_update_socket(getThis(), obj TSRMLS_CC)) {
				throw_exce(EX_CONNECTION_FAILED TSRMLS_CC, "Connection failed (%s)", PHP_PQerrorMessage(obj->intern->conn));
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_reset, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, reset) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PQreset(obj->intern->conn);

			if (CONNECTION_OK != PQstatus(obj->intern->conn)) {
				throw_exce(EX_CONNECTION_FAILED TSRMLS_CC, "Connection reset failed: (%s)", PHP_PQerrorMessage(obj->intern->conn));
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_reset_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, resetAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			if (!PQresetStart(obj->intern->conn)) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to start connection reset (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				obj->intern->poller = (int (*)(PGconn*)) PQresetPoll;
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_unlisten, 0, 0, 1)
	ZEND_ARG_INFO(0, channel)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, unlisten)
{
	zend_error_handling zeh;
	char *channel_str;
	int channel_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &channel_str, &channel_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else if (SUCCESS == zend_hash_del(&obj->intern->listeners, channel_str, channel_len + 1)) {
			PGresult *res = unlisten(obj->intern->conn, channel_str, channel_len TSRMLS_CC);

			if (res) {
				php_pqres_success(res TSRMLS_CC);
				PHP_PQclear(res);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_unlisten_async, 0, 0, 1)
	ZEND_ARG_INFO(0, channel)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, unlistenAsync) {
	zend_error_handling zeh;
	char *channel_str;
	int channel_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &channel_str, &channel_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *quoted_channel = PQescapeIdentifier(obj->intern->conn, channel_str, channel_len);

			if (!quoted_channel) {
				throw_exce(EX_ESCAPE TSRMLS_CC, "Failed to escape channel identifier (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				smart_str cmd = {0};

				smart_str_appends(&cmd, "UNLISTEN ");
				smart_str_appends(&cmd, quoted_channel);
				smart_str_0(&cmd);

				if (!PQsendQuery(obj->intern->conn, cmd.c)) {
					throw_exce(EX_IO TSRMLS_CC, "Failed to uninstall listener (%s)", PHP_PQerrorMessage(obj->intern->conn));
				} else {
					obj->intern->poller = PQconsumeInput;
					zend_hash_del(&obj->intern->listeners, channel_str, channel_len + 1);
				}

				smart_str_free(&cmd);
				PQfreemem(quoted_channel);
				php_pqconn_notify_listeners(obj TSRMLS_CC);
			}
		}
	}
}

static void php_pqconn_add_listener(php_pqconn_object_t *obj, const char *channel_str, size_t channel_len, php_pq_callback_t *listener TSRMLS_DC)
{
	HashTable ht, *existing_listeners;

	php_pq_callback_addref(listener);

	if (SUCCESS == zend_hash_find(&obj->intern->listeners, channel_str, channel_len + 1, (void *) &existing_listeners)) {
		zend_hash_next_index_insert(existing_listeners, (void *) listener, sizeof(*listener), NULL);
	} else {
		zend_hash_init(&ht, 1, NULL, (dtor_func_t) php_pq_callback_dtor, 0);
		zend_hash_next_index_insert(&ht, (void *) listener, sizeof(*listener), NULL);
		zend_hash_add(&obj->intern->listeners, channel_str, channel_len + 1, (void *) &ht, sizeof(HashTable), NULL);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_listen, 0, 0, 2)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, listen) {
	zend_error_handling zeh;
	char *channel_str = NULL;
	int channel_len = 0;
	php_pq_callback_t listener = {{0}};
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sf", &channel_str, &channel_len, &listener.fci, &listener.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *quoted_channel = PQescapeIdentifier(obj->intern->conn, channel_str, channel_len);

			if (!quoted_channel) {
				throw_exce(EX_ESCAPE TSRMLS_CC, "Failed to escape channel identifier (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				PGresult *res;
				smart_str cmd = {0};

				smart_str_appends(&cmd, "LISTEN ");
				smart_str_appends(&cmd, quoted_channel);
				smart_str_0(&cmd);

				res = PQexec(obj->intern->conn, cmd.c);

				smart_str_free(&cmd);
				PQfreemem(quoted_channel);

				if (!res) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to install listener (%s)", PHP_PQerrorMessage(obj->intern->conn));
				} else {
					if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
						obj->intern->poller = PQconsumeInput;
						php_pqconn_add_listener(obj, channel_str, channel_len, &listener TSRMLS_CC);
					}
					PHP_PQclear(res);
				}

				php_pqconn_notify_listeners(obj TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_listen_async, 0, 0, 0)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, listenAsync) {
	zend_error_handling zeh;
	char *channel_str = NULL;
	int channel_len = 0;
	php_pq_callback_t listener = {{0}};
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sf", &channel_str, &channel_len, &listener.fci, &listener.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *quoted_channel = PQescapeIdentifier(obj->intern->conn, channel_str, channel_len);

			if (!quoted_channel) {
				throw_exce(EX_ESCAPE TSRMLS_CC, "Failed to escape channel identifier (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				smart_str cmd = {0};

				smart_str_appends(&cmd, "LISTEN ");
				smart_str_appends(&cmd, quoted_channel);
				smart_str_0(&cmd);

				if (!PQsendQuery(obj->intern->conn, cmd.c)) {
					throw_exce(EX_IO TSRMLS_CC, "Failed to install listener (%s)", PHP_PQerrorMessage(obj->intern->conn));
				} else {
					obj->intern->poller = PQconsumeInput;
					php_pqconn_add_listener(obj, channel_str, channel_len, &listener TSRMLS_CC);
				}

				smart_str_free(&cmd);
				PQfreemem(quoted_channel);
				php_pqconn_notify_listeners(obj TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_notify, 0, 0, 2)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, notify) {
	zend_error_handling zeh;
	char *channel_str, *message_str;
	int channel_len, message_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &channel_str, &channel_len, &message_str, &message_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PGresult *res;
			char *params[2] = {channel_str, message_str};

			res = PQexecParams(obj->intern->conn, "select pg_notify($1, $2)", 2, NULL, (const char *const*) params, NULL, NULL, 0);

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to notify listeners (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				php_pqres_success(res TSRMLS_CC);
				PHP_PQclear(res);
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_notify_async, 0, 0, 2)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, notifyAsync) {
	zend_error_handling zeh;
	char *channel_str, *message_str;
	int channel_len, message_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &channel_str, &channel_len, &message_str, &message_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *params[2] = {channel_str, message_str};

			if (!PQsendQueryParams(obj->intern->conn, "select pg_notify($1, $2)", 2, NULL, (const char *const*) params, NULL, NULL, 0)) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to notify listeners (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				obj->intern->poller = PQconsumeInput;
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_poll, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, poll) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else if (!obj->intern->poller) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "No asynchronous operation active");
		} else {
			if (obj->intern->poller == PQconsumeInput) {
				RETVAL_LONG(obj->intern->poller(obj->intern->conn) * PGRES_POLLING_OK);
			} else {
				RETVAL_LONG(obj->intern->poller(obj->intern->conn));
			}
			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, exec) {
	zend_error_handling zeh;
	char *query_str;
	int query_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &query_str, &query_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PGresult *res = PQexec(obj->intern->conn, query_str);

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
				php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), &return_value TSRMLS_CC);
			} else {
				PHP_PQclear(res);
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_get_result, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, getResult) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PGresult *res = PQgetResult(obj->intern->conn);

			if (!res) {
				RETVAL_NULL();
			} else {
				php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), &return_value TSRMLS_CC);
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec_async, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, execAsync) {
	zend_error_handling zeh;
	php_pq_callback_t resolver = {{0}};
	char *query_str;
	int query_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f", &query_str, &query_len, &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else if (!PQsendQuery(obj->intern->conn, query_str)) {
			throw_exce(EX_IO TSRMLS_CC, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
#if HAVE_PQSETSINGLEROWMODE
		} else if (obj->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn)) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn));
#endif
		} else {
			php_pq_callback_recurse(&obj->intern->onevent, &resolver TSRMLS_CC);
			obj->intern->poller = PQconsumeInput;
			php_pqconn_notify_listeners(obj TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec_params, 0, 0, 2)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, params, 0)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, execParams) {
	zend_error_handling zeh;
	char *query_str;
	int query_len;
	zval *zparams;
	zval *ztypes = NULL;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa/|a/!", &query_str, &query_len, &zparams, &ztypes);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			PGresult *res;
			php_pq_params_t *params;

			params = php_pq_params_init(&obj->intern->converters, ztypes ? Z_ARRVAL_P(ztypes) : NULL, Z_ARRVAL_P(zparams) TSRMLS_CC);
			res = PQexecParams(obj->intern->conn, query_str, params->param.count, params->type.oids, (const char *const*) params->param.strings, NULL, NULL, 0);
			php_pq_params_free(&params);

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), &return_value TSRMLS_CC);
				} else {
					PHP_PQclear(res);
				}

				php_pqconn_notify_listeners(obj TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec_params_async, 0, 0, 2)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, params, 0)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, execParamsAsync) {
	zend_error_handling zeh;
	php_pq_callback_t resolver = {{0}};
	char *query_str;
	int query_len;
	zval *zparams;
	zval *ztypes = NULL;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa/|a/!f", &query_str, &query_len, &zparams, &ztypes, &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			int rc;
			php_pq_params_t *params;

			params = php_pq_params_init(&obj->intern->converters, ztypes ? Z_ARRVAL_P(ztypes) : NULL, Z_ARRVAL_P(zparams) TSRMLS_CC);
			rc = PQsendQueryParams(obj->intern->conn, query_str, params->param.count, params->type.oids, (const char *const*) params->param.strings, NULL, NULL, 0);
			php_pq_params_free(&params);

			if (!rc) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
#if HAVE_PQSETSINGLEROWMODE
			} else if (obj->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn)) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn));
#endif
			} else {
				php_pq_callback_recurse(&obj->intern->onevent, &resolver TSRMLS_CC);
				obj->intern->poller = PQconsumeInput;
				php_pqconn_notify_listeners(obj TSRMLS_CC);
			}
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

STATUS php_pqconn_prepare(zval *object, php_pqconn_object_t *obj, const char *name, const char *query, php_pq_params_t *params TSRMLS_DC)
{
	PGresult *res;
	STATUS rv;

	if (!obj) {
		obj = zend_object_store_get_object(object TSRMLS_CC);
	}

	res = PQprepare(obj->intern->conn, name, query, params->type.count, params->type.oids);

	if (!res) {
		rv = FAILURE;
		throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to prepare statement (%s)", PHP_PQerrorMessage(obj->intern->conn));
	} else {
		rv = php_pqres_success(res TSRMLS_CC);
		PHP_PQclear(res);
		php_pqconn_notify_listeners(obj TSRMLS_CC);
	}

	return rv;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_prepare, 0, 0, 2)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, prepare) {
	zend_error_handling zeh;
	zval *ztypes = NULL;
	char *name_str, *query_str;
	int name_len, *query_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|a/!", &name_str, &name_len, &query_str, &query_len, &ztypes);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			php_pq_params_t *params = php_pq_params_init(&obj->intern->converters, ztypes ? Z_ARRVAL_P(ztypes) : NULL, NULL TSRMLS_CC);

			if (SUCCESS != php_pqconn_prepare(getThis(), obj, name_str, query_str, params TSRMLS_CC)) {
				php_pq_params_free(&params);
			} else {
				php_pqstm_t *stm = ecalloc(1, sizeof(*stm));

				php_pq_object_addref(obj TSRMLS_CC);
				stm->conn = obj;
				stm->name = estrdup(name_str);
				stm->params = params;
				ZEND_INIT_SYMTABLE(&stm->bound);

				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqstm_create_object_ex(php_pqstm_class_entry, stm, NULL TSRMLS_CC);
			}
		}
	}
}

STATUS php_pqconn_prepare_async(zval *object, php_pqconn_object_t *obj, const char *name, const char *query, php_pq_params_t *params TSRMLS_DC)
{
	STATUS rv;

	if (!obj) {
		obj = zend_object_store_get_object(object TSRMLS_CC);
	}

	if (!PQsendPrepare(obj->intern->conn, name, query, params->type.count, params->type.oids)) {
		rv = FAILURE;
		throw_exce(EX_IO TSRMLS_CC, "Failed to prepare statement (%s)", PHP_PQerrorMessage(obj->intern->conn));
#if HAVE_PQSETSINGLEROWMODE
	} else if (obj->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn)) {
		rv = FAILURE;
		throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn));
#endif
	} else {
		rv = SUCCESS;
		obj->intern->poller = PQconsumeInput;
		php_pqconn_notify_listeners(obj TSRMLS_CC);
	}

	return rv;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_prepare_async, 0, 0, 2)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, prepareAsync) {
	zend_error_handling zeh;
	zval *ztypes = NULL;
	char *name_str, *query_str;
	int name_len, *query_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|a/!", &name_str, &name_len, &query_str, &query_len, &ztypes);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			php_pq_params_t *params = php_pq_params_init(&obj->intern->converters, ztypes ? Z_ARRVAL_P(ztypes) : NULL, NULL TSRMLS_CC);

			if (SUCCESS != php_pqconn_prepare_async(getThis(), obj, name_str, query_str, params TSRMLS_CC)) {
				php_pq_params_free(&params);
			} else {
				php_pqstm_t *stm = ecalloc(1, sizeof(*stm));

				php_pq_object_addref(obj TSRMLS_CC);
				stm->conn = obj;
				stm->name = estrdup(name_str);
				stm->params = params;
				ZEND_INIT_SYMTABLE(&stm->bound);

				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqstm_create_object_ex(php_pqstm_class_entry, stm, NULL TSRMLS_CC);
			}
		}
	}
}

STATUS php_pqconn_declare(zval *object, php_pqconn_object_t *obj, const char *decl TSRMLS_DC)
{
	PGresult *res;
	STATUS rv;

	if (!obj) {
		obj = zend_object_store_get_object(object TSRMLS_CC);
	}

	res = PQexec(obj->intern->conn, decl);

	if (!res) {
		rv = FAILURE;
		throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to declare cursor (%s)", PHP_PQerrorMessage(obj->intern->conn));
	} else {
		rv = php_pqres_success(res TSRMLS_CC);
		PHP_PQclear(res);
		php_pqconn_notify_listeners(obj TSRMLS_CC);
	}

	return rv;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_declare, 0, 0, 3)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, flags)
	ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, declare) {
	zend_error_handling zeh;
	char *name_str, *query_str;
	int name_len, query_len;
	long flags;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sls", &name_str, &name_len, &flags, &query_str, &query_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *decl = php_pqcur_declare_str(name_str, name_len, flags, query_str, query_len);

			if (SUCCESS != php_pqconn_declare(getThis(), obj, decl TSRMLS_CC)) {
				efree(decl);
			} else {
				php_pqcur_t *cur = ecalloc(1, sizeof(*cur));

				php_pq_object_addref(obj TSRMLS_CC);
				cur->conn = obj;
				cur->open = 1;
				cur->name = estrdup(name_str);
				cur->decl = decl;

				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqcur_create_object_ex(php_pqcur_class_entry, cur, NULL TSRMLS_CC);
			}
		}
	}
}

STATUS php_pqconn_declare_async(zval *object, php_pqconn_object_t *obj, const char *decl TSRMLS_DC)
{
	STATUS rv;

	if (!obj) {
		obj = zend_object_store_get_object(object TSRMLS_CC);
	}

	if (!PQsendQuery(obj->intern->conn, decl)) {
		rv = FAILURE;
		throw_exce(EX_IO TSRMLS_CC, "Failed to declare cursor (%s)", PHP_PQerrorMessage(obj->intern->conn));
#if HAVE_PQSETSINGLEROWMODE
	} else if (obj->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn)) {
		rv = FAILURE;
		throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn));
#endif
	} else {
		rv = SUCCESS;
		obj->intern->poller = PQconsumeInput;
		php_pqconn_notify_listeners(obj TSRMLS_CC);
	}

	return rv;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_declare_async, 0, 0, 2)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, flags)
	ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, declareAsync) {
	zend_error_handling zeh;
	char *name_str, *query_str;
	int name_len, query_len;
	long flags;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sls", &name_str, &name_len, &flags, &query_str, &query_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *decl = php_pqcur_declare_str(name_str, name_len, flags, query_str, query_len);

			if (SUCCESS != php_pqconn_declare_async(getThis(), obj, decl TSRMLS_CC)) {
				efree(decl);
			} else {
				php_pqcur_t *cur = ecalloc(1, sizeof(*cur));

				php_pq_object_addref(obj TSRMLS_CC);
				cur->conn = obj;
				cur->open = 1;
				cur->name = estrdup(name_str);
				cur->decl = decl;

				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqcur_create_object_ex(php_pqcur_class_entry, cur, NULL TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_quote, 0, 0, 1)
	ZEND_ARG_INFO(0, string)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, quote) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *quoted = PQescapeLiteral(obj->intern->conn, str, len);

			if (!quoted) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to quote string (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRING(quoted, 1);
				PQfreemem(quoted);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_quote_name, 0, 0, 1)
	ZEND_ARG_INFO(0, type)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, quoteName) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			char *quoted = PQescapeIdentifier(obj->intern->conn, str, len);

			if (!quoted) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to quote name (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRING(quoted, 1);
				PQfreemem(quoted);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_escape_bytea, 0, 0, 1)
	ZEND_ARG_INFO(0, bytea)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, escapeBytea) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			size_t escaped_len;
			char *escaped_str = (char *) PQescapeByteaConn(obj->intern->conn, (unsigned char *) str, len, &escaped_len);

			if (!escaped_str) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to escape bytea (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRINGL(escaped_str, escaped_len - 1, 1);
				PQfreemem(escaped_str);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_unescape_bytea, 0, 0, 1)
	ZEND_ARG_INFO(0, bytea)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, unescapeBytea) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			size_t unescaped_len;
			char *unescaped_str = (char *) PQunescapeBytea((unsigned char *)str, &unescaped_len);

			if (!unescaped_str) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to unescape bytea (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRINGL(unescaped_str, unescaped_len, 1);
				PQfreemem(unescaped_str);
			}
		}
	}
}

STATUS php_pqconn_start_transaction(zval *zconn, php_pqconn_object_t *conn_obj, long isolation, zend_bool readonly, zend_bool deferrable TSRMLS_DC)
{
	STATUS rv = FAILURE;

	if (!conn_obj) {
		conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);
	}

	if (!conn_obj->intern) {
		throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
	} else {
		PGresult *res;
		smart_str cmd = {0};
		const char *il = isolation_level(&isolation);

		smart_str_appends(&cmd, "START TRANSACTION ISOLATION LEVEL ");
		smart_str_appends(&cmd, il);
		smart_str_appends(&cmd, ", READ ");
		smart_str_appends(&cmd, readonly ? "ONLY" : "WRITE");
		smart_str_appends(&cmd, ",");
		smart_str_appends(&cmd, deferrable ? "" : " NOT");
		smart_str_appends(&cmd, " DEFERRABLE");
		smart_str_0(&cmd);

		res = PQexec(conn_obj->intern->conn, cmd.c);

		if (!res) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to start transaction (%s)", PHP_PQerrorMessage(conn_obj->intern->conn));
		} else {
			rv = php_pqres_success(res TSRMLS_CC);
			PHP_PQclear(res);
			php_pqconn_notify_listeners(conn_obj TSRMLS_CC);
		}

		smart_str_free(&cmd);
	}

	return rv;
}

STATUS php_pqconn_start_transaction_async(zval *zconn, php_pqconn_object_t *conn_obj, long isolation, zend_bool readonly, zend_bool deferrable TSRMLS_DC)
{
	STATUS rv = FAILURE;

	if (!conn_obj) {
		conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);
	}

	if (!conn_obj->intern) {
		throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
	} else {
		smart_str cmd = {0};
		const char *il = isolation_level(&isolation);

		smart_str_appends(&cmd, "START TRANSACTION ISOLATION LEVEL ");
		smart_str_appends(&cmd, il);
		smart_str_appends(&cmd, ", READ ");
		smart_str_appends(&cmd, readonly ? "ONLY" : "WRITE");
		smart_str_appends(&cmd, ",");
		smart_str_appends(&cmd, deferrable ? "" : "NOT ");
		smart_str_appends(&cmd, " DEFERRABLE");
		smart_str_0(&cmd);

		if (!PQsendQuery(conn_obj->intern->conn, cmd.c)) {
			throw_exce(EX_IO TSRMLS_CC, "Failed to start transaction (%s)", PHP_PQerrorMessage(conn_obj->intern->conn));
		} else {
			rv = SUCCESS;
			conn_obj->intern->poller = PQconsumeInput;
			php_pqconn_notify_listeners(conn_obj TSRMLS_CC);
		}

		smart_str_free(&cmd);
	}

	return rv;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_start_transaction, 0, 0, 0)
	ZEND_ARG_INFO(0, isolation)
	ZEND_ARG_INFO(0, readonly)
	ZEND_ARG_INFO(0, deferrable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, startTransaction) {
	zend_error_handling zeh;
	php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	long isolation = obj->intern ? obj->intern->default_txn_isolation : PHP_PQTXN_READ_COMMITTED;
	zend_bool readonly = obj->intern ? obj->intern->default_txn_readonly : 0;
	zend_bool deferrable = obj->intern ? obj->intern->default_txn_deferrable : 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lbb", &isolation, &readonly, &deferrable);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		rv = php_pqconn_start_transaction(getThis(), obj, isolation, readonly, deferrable TSRMLS_CC);

		if (SUCCESS == rv) {
			php_pqtxn_t *txn = ecalloc(1, sizeof(*txn));

			php_pq_object_addref(obj TSRMLS_CC);
			txn->conn = obj;
			txn->open = 1;
			txn->isolation = isolation;
			txn->readonly = readonly;
			txn->deferrable = deferrable;

			return_value->type = IS_OBJECT;
			return_value->value.obj = php_pqtxn_create_object_ex(php_pqtxn_class_entry, txn, NULL TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_start_transaction_async, 0, 0, 0)
	ZEND_ARG_INFO(0, isolation)
	ZEND_ARG_INFO(0, readonly)
	ZEND_ARG_INFO(0, deferrable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, startTransactionAsync) {
	zend_error_handling zeh;
	php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	long isolation = obj->intern ? obj->intern->default_txn_isolation : PHP_PQTXN_READ_COMMITTED;
	zend_bool readonly = obj->intern ? obj->intern->default_txn_readonly : 0;
	zend_bool deferrable = obj->intern ? obj->intern->default_txn_deferrable : 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lbb", &isolation, &readonly, &deferrable);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		rv = php_pqconn_start_transaction_async(getThis(), obj, isolation, readonly, deferrable TSRMLS_CC);

		if (SUCCESS == rv) {
			php_pqtxn_t *txn = ecalloc(1, sizeof(*txn));

			php_pq_object_addref(obj TSRMLS_CC);
			txn->conn = obj;
			txn->isolation = isolation;
			txn->readonly = readonly;
			txn->deferrable = deferrable;

			return_value->type = IS_OBJECT;
			return_value->value.obj = php_pqtxn_create_object_ex(php_pqtxn_class_entry, txn, NULL TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_trace, 0, 0, 0)
	ZEND_ARG_INFO(0, stdio_stream)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, trace) {
	zval *zstream = NULL;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|r!", &zstream)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			if (!zstream) {
				PQuntrace(obj->intern->conn);
				RETVAL_TRUE;
			} else {
				FILE *fp;
				php_stream *stream = NULL;

				php_stream_from_zval(stream, &zstream);

				if (SUCCESS != php_stream_cast(stream, PHP_STREAM_AS_STDIO, (void *) &fp, REPORT_ERRORS)) {
					RETVAL_FALSE;
				} else {
					stream->flags |= PHP_STREAM_FLAG_NO_CLOSE;
					PQtrace(obj->intern->conn, fp);
					RETVAL_TRUE;
				}
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_off, 0, 0, 1)
	ZEND_ARG_INFO(0, type)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, off) {
	zend_error_handling zeh;
	char *type_str;
	int type_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &type_str, &type_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			RETURN_BOOL(SUCCESS == zend_hash_del(&obj->intern->eventhandlers, type_str, type_len + 1));
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_on, 0, 0, 2)
	ZEND_ARG_INFO(0, type)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, on) {
	zend_error_handling zeh;
	char *type_str;
	int type_len;
	php_pq_callback_t cb = {{0}};
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sf", &type_str, &type_len, &cb.fci, &cb.fcc);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

			RETVAL_LONG(php_pqconn_add_eventhandler(obj, type_str, type_len, &cb TSRMLS_CC));
		}
	}
}

struct apply_set_converter_arg {
	HashTable *ht;
	zval **zconv;
	unsigned add:1;
};

static int apply_set_converter(void *p, void *a TSRMLS_DC)
{
	zval *tmp, **zoid = p;
	struct apply_set_converter_arg *arg = a;

	tmp = *zoid;
	Z_ADDREF_P(tmp);
	convert_to_long_ex(&tmp);
	if (arg->add) {
		Z_ADDREF_PP(arg->zconv);
		zend_hash_index_update(arg->ht, Z_LVAL_P(tmp), arg->zconv, sizeof(zval *), NULL);
	} else {
		zend_hash_index_del(arg->ht, Z_LVAL_P(tmp));
	}
	zval_ptr_dtor(&tmp);

	return ZEND_HASH_APPLY_KEEP;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_set_converter, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, converter, pq\\Converter, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, setConverter) {
	STATUS rv;
	zend_error_handling zeh;
	zval *zcnv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O", &zcnv, php_pqconv_class_entry);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			zval *tmp, *zoids = NULL;
			struct apply_set_converter_arg arg = {NULL};

			zend_call_method_with_0_params(&zcnv, NULL, NULL, "converttypes", &zoids);
			tmp = zoids;
			Z_ADDREF_P(tmp);
			convert_to_array_ex(&tmp);

			arg.ht = &obj->intern->converters;
			arg.zconv = &zcnv;
			arg.add = 1;

			zend_hash_apply_with_argument(Z_ARRVAL_P(tmp), apply_set_converter, &arg TSRMLS_CC);

			zval_ptr_dtor(&tmp);
			zval_ptr_dtor(&zoids);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_unset_converter, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, converter, pq\\Converter, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, unsetConverter) {
	STATUS rv;
	zend_error_handling zeh;
	zval *zcnv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O", &zcnv, php_pqconv_class_entry);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			zval *tmp, *zoids = NULL;
			struct apply_set_converter_arg arg = {NULL};

			zend_call_method_with_0_params(&zcnv, NULL, NULL, "converttypes", &zoids);
			tmp = zoids;
			Z_ADDREF_P(tmp);
			convert_to_array_ex(&tmp);

			arg.ht = &obj->intern->converters;
			arg.zconv = &zcnv;
			arg.add = 0;

			zend_hash_apply_with_argument(Z_ARRVAL_P(tmp), apply_set_converter, &arg TSRMLS_CC);

			zval_ptr_dtor(&tmp);
			zval_ptr_dtor(&zoids);
		}
	}
}

static zend_function_entry php_pqconn_methods[] = {
	PHP_ME(pqconn, __construct, ai_pqconn_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqconn, reset, ai_pqconn_reset, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, resetAsync, ai_pqconn_reset_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, poll, ai_pqconn_poll, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, exec, ai_pqconn_exec, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, execAsync, ai_pqconn_exec_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, execParams, ai_pqconn_exec_params, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, execParamsAsync, ai_pqconn_exec_params_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, prepare, ai_pqconn_prepare, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, prepareAsync, ai_pqconn_prepare_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, declare, ai_pqconn_declare, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, declareAsync, ai_pqconn_declare_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, unlisten, ai_pqconn_unlisten, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, unlistenAsync, ai_pqconn_unlisten_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, listen, ai_pqconn_listen, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, listenAsync, ai_pqconn_listen_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, notify, ai_pqconn_notify, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, notifyAsync, ai_pqconn_notify_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, getResult, ai_pqconn_get_result, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, quote, ai_pqconn_quote, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, quoteName, ai_pqconn_quote_name, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, escapeBytea, ai_pqconn_escape_bytea, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, unescapeBytea, ai_pqconn_unescape_bytea, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, startTransaction, ai_pqconn_start_transaction, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, startTransactionAsync, ai_pqconn_start_transaction_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, trace, ai_pqconn_trace, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, off, ai_pqconn_off, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, on, ai_pqconn_on, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, setConverter, ai_pqconn_set_converter, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, unsetConverter, ai_pqconn_unset_converter, ZEND_ACC_PUBLIC)
	{0}
};

PHP_MSHUTDOWN_FUNCTION(pqconn)
{
	zend_hash_destroy(&php_pqconn_object_prophandlers);
	return SUCCESS;
}

PHP_MINIT_FUNCTION(pqconn)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Connection", php_pqconn_methods);
	php_pqconn_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqconn_class_entry->create_object = php_pqconn_create_object;

	memcpy(&php_pqconn_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqconn_object_handlers.read_property = php_pq_object_read_prop;
	php_pqconn_object_handlers.write_property = php_pq_object_write_prop;
	php_pqconn_object_handlers.clone_obj = NULL;
	php_pqconn_object_handlers.get_property_ptr_ptr = NULL;
	php_pqconn_object_handlers.get_gc = NULL;
	php_pqconn_object_handlers.get_properties = php_pq_object_properties;
	php_pqconn_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqconn_object_prophandlers, 20, NULL, NULL, 1);

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("status"), CONNECTION_BAD, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_status;
	zend_hash_add(&php_pqconn_object_prophandlers, "status", sizeof("status"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("transactionStatus"), PQTRANS_UNKNOWN, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_transaction_status;
	zend_hash_add(&php_pqconn_object_prophandlers, "transactionStatus", sizeof("transactionStatus"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("socket"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = NULL; /* forward to std prophandler */
	zend_hash_add(&php_pqconn_object_prophandlers, "socket", sizeof("socket"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("errorMessage"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_error_message;
	zend_hash_add(&php_pqconn_object_prophandlers, "errorMessage", sizeof("errorMessage"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("busy"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_busy;
	zend_hash_add(&php_pqconn_object_prophandlers, "busy", sizeof("busy"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("encoding"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_encoding;
	ph.write = php_pqconn_object_write_encoding;
	zend_hash_add(&php_pqconn_object_prophandlers, "encoding", sizeof("encoding"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("unbuffered"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_unbuffered;
	ph.write = php_pqconn_object_write_unbuffered;
	zend_hash_add(&php_pqconn_object_prophandlers, "unbuffered", sizeof("unbuffered"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("db"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_db;
	zend_hash_add(&php_pqconn_object_prophandlers, "db", sizeof("db"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("user"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_user;
	zend_hash_add(&php_pqconn_object_prophandlers, "user", sizeof("user"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("pass"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_pass;
	zend_hash_add(&php_pqconn_object_prophandlers, "pass", sizeof("pass"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("host"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_host;
	zend_hash_add(&php_pqconn_object_prophandlers, "host", sizeof("host"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("port"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_port;
	zend_hash_add(&php_pqconn_object_prophandlers, "port", sizeof("port"), (void *) &ph, sizeof(ph), NULL);

#if HAVE_PQCONNINFO
	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("params"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_params;
	zend_hash_add(&php_pqconn_object_prophandlers, "params", sizeof("params"), (void *) &ph, sizeof(ph), NULL);
#endif

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("options"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_options;
	zend_hash_add(&php_pqconn_object_prophandlers, "options", sizeof("options"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("eventHandlers"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_event_handlers;
	zend_hash_add(&php_pqconn_object_prophandlers, "eventHandlers", sizeof("eventHandlers"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("defaultFetchType"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_def_fetch_type;
	ph.write = php_pqconn_object_write_def_fetch_type;
	zend_hash_add(&php_pqconn_object_prophandlers, "defaultFetchType", sizeof("defaultFetchType"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("defaultTransactionIsolation"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_def_txn_isolation;
	ph.write = php_pqconn_object_write_def_txn_isolation;
	zend_hash_add(&php_pqconn_object_prophandlers, "defaultTransactionIsolation", sizeof("defaultTransactionIsolation"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("defaultTransactionReadonly"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_def_txn_readonly;
	ph.write = php_pqconn_object_write_def_txn_readonly;
	zend_hash_add(&php_pqconn_object_prophandlers, "defaultTransactionReadonly", sizeof("defaultTransactionReadonly"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("defaultTransactionDeferrable"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_def_txn_deferrable;
	ph.write = php_pqconn_object_write_def_txn_deferrable;
	zend_hash_add(&php_pqconn_object_prophandlers, "defaultTransactionDeferrable", sizeof("defaultTransactionDeferrable"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("defaultAutoConvert"), PHP_PQRES_CONV_ALL, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_def_auto_conv;
	ph.write = php_pqconn_object_write_def_auto_conv;
	zend_hash_add(&php_pqconn_object_prophandlers, "defaultAutoConvert", sizeof("defaultAutoConvert"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

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

	zend_declare_class_constant_stringl(php_pqconn_class_entry, ZEND_STRL("EVENT_NOTICE"), ZEND_STRL("notice") TSRMLS_CC);
	zend_declare_class_constant_stringl(php_pqconn_class_entry, ZEND_STRL("EVENT_RESULT"), ZEND_STRL("result") TSRMLS_CC);
	zend_declare_class_constant_stringl(php_pqconn_class_entry, ZEND_STRL("EVENT_RESET"), ZEND_STRL("reset") TSRMLS_CC);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("ASYNC"), 0x1 TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("PERSISTENT"), 0x2 TSRMLS_CC);

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
