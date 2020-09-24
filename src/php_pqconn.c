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
#include <Zend/zend_smart_str.h>

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

static void php_pq_callback_hash_dtor(zval *p)
{
	php_pq_callback_dtor(Z_PTR_P(p));
	efree(Z_PTR_P(p));
}

/*
static void php_pqconn_del_eventhandler(php_pqconn_object_t *obj, const char *type_str, size_t type_len, unsigned long id)
{
	zval **evhs;

	if (SUCCESS == zend_hash_find(&obj->intern->eventhandlers, type_str, type_len + 1, (void *) &evhs)) {
		zend_hash_index_del(Z_ARRVAL_PP(evhs), id);
	}
}
*/

static zend_long php_pqconn_add_eventhandler(php_pqconn_object_t *obj, const char *type_str, size_t type_len, php_pq_callback_t *cb)
{
	zend_long h;
	zval *zevhs;

	if (!(zevhs = zend_hash_str_find(&obj->intern->eventhandlers, type_str, type_len))) {
		HashTable *evhs;
		zval tmp;

		ALLOC_HASHTABLE(evhs);
		zend_hash_init(evhs, 1, NULL, php_pq_callback_hash_dtor, 0);

		ZVAL_ARR(&tmp, evhs);
		zevhs = zend_hash_str_add(&obj->intern->eventhandlers, type_str, type_len, &tmp);
	}

	php_pq_callback_addref(cb);
	h = zend_hash_next_free_element(Z_ARRVAL_P(zevhs));
	zend_hash_index_update_mem(Z_ARRVAL_P(zevhs), h, (void *) cb, sizeof(*cb));

	return h;
}

static void php_pqconn_object_free(zend_object *o)
{
	php_pqconn_object_t *obj = PHP_PQ_OBJ(NULL, o);
#if DBG_GC
	fprintf(stderr, "FREE conn(#%d) %p\n", obj->zo.handle, obj);
#endif
	if (obj->intern) {
		php_pq_callback_dtor(&obj->intern->onevent);
		php_resource_factory_handle_dtor(&obj->intern->factory, obj->intern->conn);
		php_resource_factory_dtor(&obj->intern->factory);
		zend_hash_destroy(&obj->intern->listeners);
		zend_hash_destroy(&obj->intern->statements);
		zend_hash_destroy(&obj->intern->converters);
		zend_hash_destroy(&obj->intern->eventhandlers);
		efree(obj->intern);
		obj->intern = NULL;
	}
	php_pq_object_dtor(o);
}


php_pqconn_object_t *php_pqconn_create_object_ex(zend_class_entry *ce, php_pqconn_t *intern)
{
	return php_pq_object_create(ce, intern, sizeof(php_pqconn_object_t),
			&php_pqconn_object_handlers, &php_pqconn_object_prophandlers);
}

static zend_object *php_pqconn_create_object(zend_class_entry *class_type)
{
	return &php_pqconn_create_object_ex(class_type, NULL)->zo;
}

static void php_pqconn_object_read_status(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQstatus(obj->intern->conn));
}

static void php_pqconn_object_read_transaction_status(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQtransactionStatus(obj->intern->conn));
}

static void php_pqconn_object_read_error_message(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	char *error = PHP_PQerrorMessage(obj->intern->conn);

	if (error) {
		RETVAL_STRING(error);
	} else {
		RETVAL_NULL();
	}
}

static int apply_notify_listener(zval *p, void *arg)
{
	php_pq_callback_t *listener = Z_PTR_P(p);
	PGnotify *nfy = arg;
	zval zpid, zchannel, zmessage;

	ZVAL_LONG(&zpid, nfy->be_pid);
	ZVAL_STRING(&zchannel, nfy->relname);
	ZVAL_STRING(&zmessage, nfy->extra);

	zend_fcall_info_argn(&listener->fci, 3, &zchannel, &zmessage, &zpid);
	zend_fcall_info_call(&listener->fci, &listener->fcc, NULL, NULL);
	zend_fcall_info_args_clear(&listener->fci, 0);

	zval_ptr_dtor(&zchannel);
	zval_ptr_dtor(&zmessage);
	zval_ptr_dtor(&zpid);

	return ZEND_HASH_APPLY_KEEP;
}

static int apply_notify_listeners(zval *p, int argc, va_list argv, zend_hash_key *key)
{
	HashTable *listeners = Z_ARRVAL_P(p);
	PGnotify *nfy = va_arg(argv, PGnotify *);

	if (0 == fnmatch(key->key->val, nfy->relname, 0)) {
		zend_hash_apply_with_argument(listeners, apply_notify_listener, nfy);
	}

	return ZEND_HASH_APPLY_KEEP;
}

void php_pqconn_notify_listeners(php_pqconn_object_t *obj)
{
	PGnotify *nfy;

	while ((nfy = PQnotifies(obj->intern->conn))) {
		zend_hash_apply_with_arguments(&obj->intern->listeners, apply_notify_listeners, 1, nfy);
		PQfreemem(nfy);
	}
}

static void php_pqconn_object_read_busy(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(PQisBusy(obj->intern->conn));
}

static void php_pqconn_object_read_encoding(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_STRING(pg_encoding_to_char(PQclientEncoding(obj->intern->conn)));
}

static void php_pqconn_object_write_encoding(void *o, zval *value)
{
	php_pqconn_object_t *obj = o;
	zend_string *zenc = zval_get_string(value);

	if (0 > PQsetClientEncoding(obj->intern->conn, zenc->val)) {
		php_error(E_NOTICE, "Unrecognized encoding '%s'", zenc->val);
	}

	zend_string_release(zenc);
}

static void php_pqconn_object_read_unbuffered(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->unbuffered);
}

static void php_pqconn_object_write_unbuffered(void *o, zval *value)
{
	php_pqconn_object_t *obj = o;

	obj->intern->unbuffered = z_is_true(value);
}

static void php_pqconn_object_read_nonblocking(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(PQisnonblocking(obj->intern->conn));
}

static void php_pqconn_object_write_nonblocking(void *o, zval *value)
{
	php_pqconn_object_t *obj = o;

	PQsetnonblocking(obj->intern->conn, z_is_true(value));
}

static void php_pqconn_object_read_db(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	char *db = PQdb(obj->intern->conn);

	if (db) {
		RETVAL_STRING(db);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_user(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	char *user = PQuser(obj->intern->conn);

	if (user) {
		RETVAL_STRING(user);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_pass(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	char *pass = PQpass(obj->intern->conn);

	if (pass) {
		RETVAL_STRING(pass);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_host(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	char *host = PQhost(obj->intern->conn);

	if (host) {
		RETVAL_STRING(host);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static void php_pqconn_object_read_port(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	char *port = PQport(obj->intern->conn);

	if (port) {
		RETVAL_STRING(port);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

#if HAVE_PQCONNINFO
static void php_pqconn_object_read_params(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	PQconninfoOption *ptr, *params = PQconninfo(obj->intern->conn);

	array_init(return_value);

	if (params) {
		for (ptr = params; ptr->keyword; ++ptr) {
			if (ptr->val) {
				add_assoc_string(return_value, ptr->keyword, ptr->val);
			} else {
				add_assoc_null(return_value, ptr->keyword);
			}
		}
		PQconninfoFree(params);
	}
}
#endif

static void php_pqconn_object_read_options(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	char *options = PQoptions(obj->intern->conn);

	if (options) {
		RETVAL_STRING(options);
	} else {
		RETVAL_EMPTY_STRING();
	}
}

static int apply_read_callback_ex(zval *p, void *arg)
{
	HashTable *rv = arg;
	zval zcb;

	zend_hash_next_index_insert(rv, php_pq_callback_to_zval(Z_PTR_P(p), &zcb));

	return ZEND_HASH_APPLY_KEEP;
}

static int apply_read_callbacks(zval *p, int argc, va_list argv, zend_hash_key *key)
{
	HashTable *evhs = Z_ARRVAL_P(p), *rv = va_arg(argv, HashTable *);
	zval entry, *entry_ptr;

	array_init_size(&entry, zend_hash_num_elements(evhs));

	if (key->key->len) {
		entry_ptr = zend_hash_add(rv, key->key, &entry);
	} else {
		entry_ptr = zend_hash_index_update(rv, key->h, &entry);
	}

	zend_hash_apply_with_argument(evhs, apply_read_callback_ex, Z_ARRVAL_P(entry_ptr));

	return ZEND_HASH_APPLY_KEEP;
}
static void php_pqconn_object_read_event_handlers(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	array_init(return_value);
	zend_hash_apply_with_arguments(&obj->intern->eventhandlers, apply_read_callbacks, 1, Z_ARRVAL_P(return_value));
}

static void php_pqconn_object_gc_event_handlers(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	zval *evhs;

	ZEND_HASH_FOREACH_VAL(&obj->intern->eventhandlers, evhs)
	{
		zval *evh;

		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(evhs), evh)
		{
			zval zcb;

			add_next_index_zval(return_value, php_pq_callback_to_zval_no_addref(Z_PTR_P(evh), &zcb));
		}
		ZEND_HASH_FOREACH_END();
	}
	ZEND_HASH_FOREACH_END();
}

static void php_pqconn_object_read_listeners(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	array_init(return_value);
	zend_hash_apply_with_arguments(&obj->intern->listeners, apply_read_callbacks, 1, Z_ARRVAL_P(return_value));
}

static void php_pqconn_object_gc_listeners(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	zval *listeners;

	ZEND_HASH_FOREACH_VAL(&obj->intern->listeners, listeners)
	{
		zval *listener;

		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(listeners), listener)
		{
			zval zcb;

			add_next_index_zval(return_value, php_pq_callback_to_zval_no_addref(Z_PTR_P(listener), &zcb));
		}
		ZEND_HASH_FOREACH_END();
	}
	ZEND_HASH_FOREACH_END();
}

static void php_pqconn_object_read_converters(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	array_init(return_value);
	zend_hash_copy(Z_ARRVAL_P(return_value), &obj->intern->converters, zval_add_ref);
}

static void php_pqconn_object_gc_converters(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;
	zval *converter;

	ZEND_HASH_FOREACH_VAL(&obj->intern->converters, converter)
	{
		add_next_index_zval(return_value, converter);
	}
	ZEND_HASH_FOREACH_END();
}

static void php_pqconn_object_read_def_fetch_type(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(obj->intern->default_fetch_type);
}
static void php_pqconn_object_write_def_fetch_type(void *o, zval *value)
{
	php_pqconn_object_t *obj = o;

	obj->intern->default_fetch_type = zval_get_long(value) & 0x3; /* two bits only */
}

static void php_pqconn_object_read_def_txn_isolation(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(obj->intern->default_txn_isolation);
}
static void php_pqconn_object_write_def_txn_isolation(void *o, zval *value)
{
	php_pqconn_object_t *obj = o;

	obj->intern->default_txn_isolation = zval_get_long(value) & 0x3; /* two bits only */
}

static void php_pqconn_object_read_def_txn_readonly(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->default_txn_readonly);
}
static void php_pqconn_object_write_def_txn_readonly(void *o, zval *value)
{
	php_pqconn_object_t *obj = o;

	obj->intern->default_txn_readonly = z_is_true(value);
}

static void php_pqconn_object_read_def_txn_deferrable(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->default_txn_deferrable);
}
static void php_pqconn_object_write_def_txn_deferrable(void *o, zval *value)
{
	php_pqconn_object_t *obj = o;

	obj->intern->default_txn_deferrable = zend_is_true(value);
}

static void php_pqconn_object_read_def_auto_conv(void *o, zval *return_value)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(obj->intern->default_auto_convert);
}
static void php_pqconn_object_write_def_auto_conv(void *o, zval *value)
{
	php_pqconn_object_t *obj = o;

	obj->intern->default_auto_convert = zval_get_long(value) & PHP_PQRES_CONV_ALL;
}

static ZEND_RESULT_CODE php_pqconn_update_socket(zval *zobj, php_pqconn_object_t *obj)
{
	zval zsocket, zmember;
	php_stream *stream;
	ZEND_RESULT_CODE retval;
	int socket;

	if (!obj) {
		obj = PHP_PQ_OBJ(zobj, NULL);
	}

	ZVAL_STRINGL(&zmember, "socket", sizeof("socket")-1);

	if ((CONNECTION_BAD != PQstatus(obj->intern->conn))
	&&	(-1 < (socket = PQsocket(obj->intern->conn)))
	&&	(stream = php_stream_fopen_from_fd(socket, "r+b", NULL))) {
		stream->flags |= PHP_STREAM_FLAG_NO_CLOSE;
		php_stream_to_zval(stream, &zsocket);
		retval = SUCCESS;
	} else {
		ZVAL_NULL(&zsocket);
		retval = FAILURE;
	}
#if PHP_VERSION_ID >= 80000
	zend_std_write_property(Z_OBJ_P(zobj), Z_STR(zmember), &zsocket, NULL);
#else
	zend_std_write_property(zobj, &zmember, &zsocket, NULL);
#endif
	zval_ptr_dtor(&zsocket);
	zval_ptr_dtor(&zmember);

	return retval;
}

static void *php_pqconn_resource_factory_ctor(void *data, void *init_arg)
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

static void php_pqconn_resource_factory_dtor(void *opaque, void *handle)
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

static void php_pqconn_wakeup(php_persistent_handle_factory_t *f, void **handle)
{
	PGresult *res = PQexec(*handle, "");
	php_pqres_clear(res);

	if (CONNECTION_OK != PQstatus(*handle)) {
		PQreset(*handle);
	}
}

static inline PGresult *unlisten(PGconn *conn, const char *channel_str, size_t channel_len)
{
	char *quoted_channel = PQescapeIdentifier(conn, channel_str, channel_len);
	PGresult *res = NULL;

	if (quoted_channel) {
		smart_str cmd = {0};

		smart_str_appends(&cmd, "UNLISTEN ");
		smart_str_appends(&cmd, quoted_channel);
		smart_str_0(&cmd);

		res = PQexec(conn, smart_str_v(&cmd));

		smart_str_free(&cmd);
		PQfreemem(quoted_channel);
	}

	return res;
}

static int apply_unlisten(zval *p, int argc, va_list argv, zend_hash_key *key)
{
	php_pqconn_object_t *obj = va_arg(argv, php_pqconn_object_t *);
	PGresult *res = unlisten(obj->intern->conn, key->key->val, key->key->len);

	if (res) {
		php_pqres_clear(res);
	}

	return ZEND_HASH_APPLY_REMOVE;
}

static void php_pqconn_retire(php_persistent_handle_factory_t *f, void **handle)
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
		php_pqres_clear(res);
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
		php_pqres_clear(res);
	}

	if (evdata) {
		/* clean up notify listeners */
		zend_hash_apply_with_arguments(&evdata->obj->intern->listeners, apply_unlisten, 1, evdata->obj);

		/* release instance data */
		efree(evdata);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_construct, 0, 0, 1)
	ZEND_ARG_INFO(0, dsn)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, __construct) {
	zend_error_handling zeh;
	char *dsn_str = "";
	size_t dsn_len = 0;
	zend_long flags = 0;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|sl", &dsn_str, &dsn_len, &flags);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (obj->intern) {
			throw_exce(EX_BAD_METHODCALL, "pq\\Connection already initialized");
		} else {
			php_pqconn_event_data_t *evdata =  php_pqconn_event_data_init(obj);
			php_pqconn_resource_factory_data_t rfdata = {dsn_str, flags};

			obj->intern = ecalloc(1, sizeof(*obj->intern));

			obj->intern->default_auto_convert = PHP_PQRES_CONV_ALL;

			zend_hash_init(&obj->intern->listeners, 0, NULL, ZVAL_PTR_DTOR, 0);
			zend_hash_init(&obj->intern->statements, 0, NULL, NULL, 0);
			zend_hash_init(&obj->intern->converters, 0, NULL, ZVAL_PTR_DTOR, 0);
			zend_hash_init(&obj->intern->eventhandlers, 0, NULL, ZVAL_PTR_DTOR, 0);

			if (flags & PHP_PQCONN_PERSISTENT) {
				zend_string *dsn = zend_string_init(dsn_str, dsn_len, 0);
				php_persistent_handle_factory_t *phf = php_persistent_handle_concede(NULL, PHP_PQ_G->connection.name, dsn, php_pqconn_wakeup, php_pqconn_retire);
				php_persistent_handle_resource_factory_init(&obj->intern->factory, phf);
				zend_string_release(dsn);
			} else {
				php_resource_factory_init(&obj->intern->factory, &php_pqconn_resource_factory_ops, NULL, NULL);
			}

			if (flags & PHP_PQCONN_ASYNC) {
				obj->intern->poller = (int (*)(PGconn*)) PQconnectPoll;
			}

			obj->intern->conn = php_resource_factory_handle_ctor(&obj->intern->factory, &rfdata);

			PQsetInstanceData(obj->intern->conn, php_pqconn_event, evdata);
			PQsetNoticeReceiver(obj->intern->conn, php_pqconn_notice_recv, evdata);

			if (SUCCESS != php_pqconn_update_socket(getThis(), obj)) {
				throw_exce(EX_CONNECTION_FAILED, "Connection failed (%s)", PHP_PQerrorMessage(obj->intern->conn));
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_reset, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, reset) {
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			PQreset(obj->intern->conn);

			if (CONNECTION_OK != PQstatus(obj->intern->conn)) {
				throw_exce(EX_CONNECTION_FAILED, "Connection reset failed: (%s)", PHP_PQerrorMessage(obj->intern->conn));
			}

			php_pqconn_notify_listeners(obj);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_reset_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, resetAsync) {
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			if (!PQresetStart(obj->intern->conn)) {
				throw_exce(EX_IO, "Failed to start connection reset (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				obj->intern->poller = (int (*)(PGconn*)) PQresetPoll;
			}

			php_pqconn_notify_listeners(obj);
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
	size_t channel_len;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "s", &channel_str, &channel_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else if (SUCCESS == zend_hash_str_del(&obj->intern->listeners, channel_str, channel_len)) {
			PGresult *res = unlisten(obj->intern->conn, channel_str, channel_len);

			if (res) {
				php_pqres_success(res);
				php_pqres_clear(res);
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
	size_t channel_len;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "s", &channel_str, &channel_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			char *quoted_channel = PQescapeIdentifier(obj->intern->conn, channel_str, channel_len);

			if (!quoted_channel) {
				throw_exce(EX_ESCAPE, "Failed to escape channel identifier (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				smart_str cmd = {0};

				smart_str_appends(&cmd, "UNLISTEN ");
				smart_str_appends(&cmd, quoted_channel);
				smart_str_0(&cmd);

				if (!PQsendQuery(obj->intern->conn, smart_str_v(&cmd))) {
					throw_exce(EX_IO, "Failed to uninstall listener (%s)", PHP_PQerrorMessage(obj->intern->conn));
				} else {
					obj->intern->poller = PQconsumeInput;
					zend_hash_str_del(&obj->intern->listeners, channel_str, channel_len);
				}

				smart_str_free(&cmd);
				PQfreemem(quoted_channel);
				php_pqconn_notify_listeners(obj);
			}
		}
	}
}

static void php_pqconn_add_listener(php_pqconn_object_t *obj, const char *channel_str, size_t channel_len, php_pq_callback_t *listener)
{
	zval *existing;

	php_pq_callback_addref(listener);

	if ((existing = zend_hash_str_find(&obj->intern->listeners, channel_str, channel_len))) {
		zend_hash_next_index_insert_mem(Z_ARRVAL_P(existing), (void *) listener, sizeof(*listener));
	} else {
		zval tmp;
		HashTable *ht;

		ALLOC_HASHTABLE(ht);
		zend_hash_init(ht, 0, NULL, php_pq_callback_hash_dtor, 0);
		zend_hash_next_index_insert_mem(ht, (void *) listener, sizeof(*listener));

		ZVAL_ARR(&tmp, ht);
		zend_hash_str_add(&obj->intern->listeners, channel_str, channel_len, &tmp);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_listen, 0, 0, 2)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, listen) {
	zend_error_handling zeh;
	char *channel_str = NULL;
	size_t channel_len = 0;
	php_pq_callback_t listener = PHP_PQ_CALLBACK_INIT;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "sf", &channel_str, &channel_len, &listener.fci, &listener.fcc);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			char *quoted_channel = PQescapeIdentifier(obj->intern->conn, channel_str, channel_len);

			if (!quoted_channel) {
				throw_exce(EX_ESCAPE, "Failed to escape channel identifier (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				PGresult *res;
				smart_str cmd = {0};

				smart_str_appends(&cmd, "LISTEN ");
				smart_str_appends(&cmd, quoted_channel);
				smart_str_0(&cmd);

				res = php_pq_exec(obj->intern->conn, smart_str_v(&cmd));

				smart_str_free(&cmd);
				PQfreemem(quoted_channel);

				if (!res) {
					throw_exce(EX_RUNTIME, "Failed to install listener (%s)", PHP_PQerrorMessage(obj->intern->conn));
				} else {
					if (SUCCESS == php_pqres_success(res)) {
						obj->intern->poller = PQconsumeInput;
						php_pqconn_add_listener(obj, channel_str, channel_len, &listener);
					}
					php_pqres_clear(res);
				}

				php_pqconn_notify_listeners(obj);
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
	size_t channel_len = 0;
	php_pq_callback_t listener = PHP_PQ_CALLBACK_INIT;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "sf", &channel_str, &channel_len, &listener.fci, &listener.fcc);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			char *quoted_channel = PQescapeIdentifier(obj->intern->conn, channel_str, channel_len);

			if (!quoted_channel) {
				throw_exce(EX_ESCAPE, "Failed to escape channel identifier (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				smart_str cmd = {0};

				smart_str_appends(&cmd, "LISTEN ");
				smart_str_appends(&cmd, quoted_channel);
				smart_str_0(&cmd);

				if (!PQsendQuery(obj->intern->conn, smart_str_v(&cmd))) {
					throw_exce(EX_IO, "Failed to install listener (%s)", PHP_PQerrorMessage(obj->intern->conn));
				} else {
					obj->intern->poller = PQconsumeInput;
					php_pqconn_add_listener(obj, channel_str, channel_len, &listener);
				}

				smart_str_free(&cmd);
				PQfreemem(quoted_channel);
				php_pqconn_notify_listeners(obj);
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
	size_t channel_len, message_len;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &channel_str, &channel_len, &message_str, &message_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			PGresult *res;
			char *params[2] = {channel_str, message_str};

			res = PQexecParams(obj->intern->conn, "select pg_notify($1, $2)", 2, NULL, (const char *const*) params, NULL, NULL, 0);

			if (!res) {
				throw_exce(EX_RUNTIME, "Failed to notify listeners (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				php_pqres_success(res);
				php_pqres_clear(res);
			}

			php_pqconn_notify_listeners(obj);
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
	size_t channel_len, message_len;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &channel_str, &channel_len, &message_str, &message_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			char *params[2] = {channel_str, message_str};

			if (!PQsendQueryParams(obj->intern->conn, "select pg_notify($1, $2)", 2, NULL, (const char *const*) params, NULL, NULL, 0)) {
				throw_exce(EX_IO, "Failed to notify listeners (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				obj->intern->poller = PQconsumeInput;
			}

			php_pqconn_notify_listeners(obj);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_poll, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, poll) {
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else if (!obj->intern->poller) {
			throw_exce(EX_RUNTIME, "No asynchronous operation active");
		} else {
			if (obj->intern->poller == PQconsumeInput) {
				RETVAL_LONG(obj->intern->poller(obj->intern->conn) * PGRES_POLLING_OK);
			} else {
				RETVAL_LONG(obj->intern->poller(obj->intern->conn));
			}
			php_pqconn_notify_listeners(obj);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_flush, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, flush) {
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else if (!obj->intern->poller) {
			throw_exce(EX_RUNTIME, "No asynchronous operation active");
		} else {
			switch (PQflush(obj->intern->conn)) {
			case -1:
			default:
				throw_exce(EX_RUNTIME, "Failed to flush connection: %s", PHP_PQerrorMessage(obj->intern->conn));
				break;
			case 0:
				RETVAL_TRUE;
				break;
			case 1:
				RETVAL_FALSE;
				break;
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, exec) {
	zend_error_handling zeh;
	char *query_str;
	size_t query_len;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "s", &query_str, &query_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			PGresult *res = php_pq_exec(obj->intern->conn, query_str);

			if (!res) {
				throw_exce(EX_RUNTIME, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else if (SUCCESS == php_pqres_success(res)) {
				php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), return_value);
			} else {
				php_pqres_clear(res);
			}

			php_pqconn_notify_listeners(obj);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_get_result, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, getResult) {
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			PGresult *res = PQgetResult(obj->intern->conn);
			php_pq_object_t *res_obj;

			if (res && (res_obj = PQresultInstanceData(res, php_pqconn_event))) {
				php_pq_object_to_zval_no_addref(res_obj, return_value);
			} else {
				RETVAL_NULL();
			}

			php_pqconn_notify_listeners(obj);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec_async, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, execAsync) {
	zend_error_handling zeh;
	php_pq_callback_t resolver = PHP_PQ_CALLBACK_INIT;
	char *query_str;
	size_t query_len;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "s|f", &query_str, &query_len, &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else if (!PQsendQuery(obj->intern->conn, query_str)) {
			throw_exce(EX_IO, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
#if HAVE_PQSETSINGLEROWMODE
		} else if (obj->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn)) {
			throw_exce(EX_RUNTIME, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn));
#endif
		} else {
			php_pq_callback_recurse(&obj->intern->onevent, &resolver);
			obj->intern->poller = PQconsumeInput;
			php_pqconn_notify_listeners(obj);
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
	size_t query_len;
	zval *zparams;
	zval *ztypes = NULL;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "sa/|a/!", &query_str, &query_len, &zparams, &ztypes);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			PGresult *res;
			php_pq_params_t *params;

			params = php_pq_params_init(&obj->intern->converters, ztypes ? Z_ARRVAL_P(ztypes) : NULL, Z_ARRVAL_P(zparams));
			res = PQexecParams(obj->intern->conn, query_str, params->param.count, params->type.oids, (const char *const*) params->param.strings, NULL, NULL, 0);
			php_pq_params_free(&params);

			if (!res) {
				throw_exce(EX_RUNTIME, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res)) {
					php_pq_object_to_zval_no_addref(PQresultInstanceData(res, php_pqconn_event), return_value);
				} else {
					php_pqres_clear(res);
				}

				php_pqconn_notify_listeners(obj);
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
	php_pq_callback_t resolver = PHP_PQ_CALLBACK_INIT;
	char *query_str;
	size_t query_len;
	zval *zparams;
	zval *ztypes = NULL;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "sa/|a/!f", &query_str, &query_len, &zparams, &ztypes, &resolver.fci, &resolver.fcc);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			int rc;
			php_pq_params_t *params;

			params = php_pq_params_init(&obj->intern->converters, ztypes ? Z_ARRVAL_P(ztypes) : NULL, Z_ARRVAL_P(zparams));
			rc = PQsendQueryParams(obj->intern->conn, query_str, params->param.count, params->type.oids, (const char *const*) params->param.strings, NULL, NULL, 0);
			php_pq_params_free(&params);

			if (!rc) {
				throw_exce(EX_IO, "Failed to execute query (%s)", PHP_PQerrorMessage(obj->intern->conn));
#if HAVE_PQSETSINGLEROWMODE
			} else if (obj->intern->unbuffered && !PQsetSingleRowMode(obj->intern->conn)) {
				throw_exce(EX_RUNTIME, "Failed to enable unbuffered mode (%s)", PHP_PQerrorMessage(obj->intern->conn));
#endif
			} else {
				php_pq_callback_recurse(&obj->intern->onevent, &resolver);
				obj->intern->poller = PQconsumeInput;
				php_pqconn_notify_listeners(obj);
			}
		}
	}
	zend_restore_error_handling(&zeh);
}

ZEND_RESULT_CODE php_pqconn_prepare(zval *object, php_pqconn_object_t *obj, const char *name, const char *query, php_pq_params_t *params)
{
	PGresult *res;
	ZEND_RESULT_CODE rv;

	if (!obj) {
		obj = PHP_PQ_OBJ(object, NULL);
	}

	res = php_pq_prepare(obj->intern->conn, name, query, params->type.count, params->type.oids);

	if (!res) {
		rv = FAILURE;
		throw_exce(EX_RUNTIME, "Failed to prepare statement (%s)", PHP_PQerrorMessage(obj->intern->conn));
	} else {
		rv = php_pqres_success(res);
		php_pqres_clear(res);
		php_pqconn_notify_listeners(obj);
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
	size_t name_len, *query_len;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "ss|a/!", &name_str, &name_len, &query_str, &query_len, &ztypes);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			php_pq_params_t *params = php_pq_params_init(&obj->intern->converters, ztypes ? Z_ARRVAL_P(ztypes) : NULL, NULL);

			if (SUCCESS != php_pqconn_prepare(getThis(), obj, name_str, query_str, params)) {
				php_pq_params_free(&params);
			} else {
				php_pqstm_t *stm = php_pqstm_init(obj, name_str, query_str, params);

				RETVAL_OBJ(&php_pqstm_create_object_ex(php_pqstm_class_entry, stm)->zo);
			}
		}
	}
}

ZEND_RESULT_CODE php_pqconn_prepare_async(zval *object, php_pqconn_object_t *obj, const char *name, const char *query, php_pq_params_t *params)
{
	ZEND_RESULT_CODE rv;

	if (!obj) {
		obj = PHP_PQ_OBJ(object, NULL);
	}

	if (!PQsendPrepare(obj->intern->conn, name, query, params->type.count, params->type.oids)) {
		rv = FAILURE;
		throw_exce(EX_IO, "Failed to prepare statement (%s)", PHP_PQerrorMessage(obj->intern->conn));
	} else {
		rv = SUCCESS;
		obj->intern->poller = PQconsumeInput;
		php_pqconn_notify_listeners(obj);
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
	size_t name_len, *query_len;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "ss|a/!", &name_str, &name_len, &query_str, &query_len, &ztypes);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			php_pq_params_t *params = php_pq_params_init(&obj->intern->converters, ztypes ? Z_ARRVAL_P(ztypes) : NULL, NULL);

			if (SUCCESS != php_pqconn_prepare_async(getThis(), obj, name_str, query_str, params)) {
				php_pq_params_free(&params);
			} else {
				php_pqstm_t *stm = php_pqstm_init(obj, name_str, query_str, params);

				RETVAL_OBJ(&php_pqstm_create_object_ex(php_pqstm_class_entry, stm)->zo);
			}
		}
	}
}

ZEND_RESULT_CODE php_pqconn_declare(zval *object, php_pqconn_object_t *obj, const char *decl)
{
	PGresult *res;
	ZEND_RESULT_CODE rv;

	if (!obj) {
		obj = PHP_PQ_OBJ(object, NULL);
	}

	res = php_pq_exec(obj->intern->conn, decl);

	if (!res) {
		rv = FAILURE;
		throw_exce(EX_RUNTIME, "Failed to declare cursor (%s)", PHP_PQerrorMessage(obj->intern->conn));
	} else {
		rv = php_pqres_success(res);
		php_pqres_clear(res);
		php_pqconn_notify_listeners(obj);
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
	size_t name_len, query_len;
	zend_long flags;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "sls", &name_str, &name_len, &flags, &query_str, &query_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			int query_offset;
			char *decl = php_pqcur_declare_str(name_str, name_len, flags, query_str, query_len, &query_offset);

			if (SUCCESS != php_pqconn_declare(getThis(), obj, decl)) {
				efree(decl);
			} else {
				php_pqcur_t *cur = php_pqcur_init(obj, name_str, decl, query_offset, flags);

				RETVAL_OBJ(&php_pqcur_create_object_ex(php_pqcur_class_entry, cur)->zo);
			}
		}
	}
}

ZEND_RESULT_CODE php_pqconn_declare_async(zval *object, php_pqconn_object_t *obj, const char *decl)
{
	ZEND_RESULT_CODE rv;

	if (!obj) {
		obj = PHP_PQ_OBJ(object, NULL);
	}

	if (!PQsendQuery(obj->intern->conn, decl)) {
		rv = FAILURE;
		throw_exce(EX_IO, "Failed to declare cursor (%s)", PHP_PQerrorMessage(obj->intern->conn));
	} else {
		rv = SUCCESS;
		obj->intern->poller = PQconsumeInput;
		php_pqconn_notify_listeners(obj);
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
	size_t name_len, query_len;
	zend_long flags;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "sls", &name_str, &name_len, &flags, &query_str, &query_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			int query_offset;
			char *decl = php_pqcur_declare_str(name_str, name_len, flags, query_str, query_len, &query_offset);

			if (SUCCESS != php_pqconn_declare_async(getThis(), obj, decl)) {
				efree(decl);
			} else {
				php_pqcur_t *cur = php_pqcur_init(obj, name_str, decl, query_offset, flags);

				RETVAL_OBJ(&php_pqcur_create_object_ex(php_pqcur_class_entry, cur)->zo);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_quote, 0, 0, 1)
	ZEND_ARG_INFO(0, string)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, quote) {
	char *str;
	size_t len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS(), "s", &str, &len)) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			char *quoted = PQescapeLiteral(obj->intern->conn, str, len);

			if (!quoted) {
				php_error_docref(NULL, E_WARNING, "Failed to quote string (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRING(quoted);
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
	size_t len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS(), "s", &str, &len)) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			char *quoted = PQescapeIdentifier(obj->intern->conn, str, len);

			if (!quoted) {
				php_error_docref(NULL, E_WARNING, "Failed to quote name (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRING(quoted);
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
	size_t len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS(), "s", &str, &len)) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			size_t escaped_len;
			char *escaped_str = (char *) PQescapeByteaConn(obj->intern->conn, (unsigned char *) str, len, &escaped_len);

			if (!escaped_str) {
				php_error_docref(NULL, E_WARNING, "Failed to escape bytea (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRINGL(escaped_str, escaped_len - 1);
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
	size_t len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS(), "s", &str, &len)) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			size_t unescaped_len;
			char *unescaped_str = (char *) PQunescapeBytea((unsigned char *)str, &unescaped_len);

			if (!unescaped_str) {
				php_error_docref(NULL, E_WARNING, "Failed to unescape bytea (%s)", PHP_PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			} else {
				RETVAL_STRINGL(unescaped_str, unescaped_len);
				PQfreemem(unescaped_str);
			}
		}
	}
}

ZEND_RESULT_CODE php_pqconn_start_transaction(zval *zconn, php_pqconn_object_t *conn_obj, long isolation, zend_bool readonly, zend_bool deferrable)
{
	ZEND_RESULT_CODE rv = FAILURE;

	if (!conn_obj) {
		conn_obj = PHP_PQ_OBJ(zconn, NULL);
	}

	if (!conn_obj->intern) {
		throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
	} else {
		PGresult *res;
		smart_str cmd = {0};
		const char *il = php_pq_isolation_level(&isolation);

		smart_str_appends(&cmd, "START TRANSACTION ISOLATION LEVEL ");
		smart_str_appends(&cmd, il);
		smart_str_appends(&cmd, ", READ ");
		smart_str_appends(&cmd, readonly ? "ONLY" : "WRITE");
		smart_str_appends(&cmd, ",");
		smart_str_appends(&cmd, deferrable ? "" : " NOT");
		smart_str_appends(&cmd, " DEFERRABLE");
		smart_str_0(&cmd);

		res = php_pq_exec(conn_obj->intern->conn, smart_str_v(&cmd));

		if (!res) {
			throw_exce(EX_RUNTIME, "Failed to start transaction (%s)", PHP_PQerrorMessage(conn_obj->intern->conn));
		} else {
			rv = php_pqres_success(res);
			php_pqres_clear(res);
			php_pqconn_notify_listeners(conn_obj);
		}

		smart_str_free(&cmd);
	}

	return rv;
}

ZEND_RESULT_CODE php_pqconn_start_transaction_async(zval *zconn, php_pqconn_object_t *conn_obj, long isolation, zend_bool readonly, zend_bool deferrable)
{
	ZEND_RESULT_CODE rv = FAILURE;

	if (!conn_obj) {
		conn_obj = PHP_PQ_OBJ(zconn, NULL);
	}

	if (!conn_obj->intern) {
		throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
	} else {
		smart_str cmd = {0};
		const char *il = php_pq_isolation_level(&isolation);

		smart_str_appends(&cmd, "START TRANSACTION ISOLATION LEVEL ");
		smart_str_appends(&cmd, il);
		smart_str_appends(&cmd, ", READ ");
		smart_str_appends(&cmd, readonly ? "ONLY" : "WRITE");
		smart_str_appends(&cmd, ",");
		smart_str_appends(&cmd, deferrable ? "" : "NOT ");
		smart_str_appends(&cmd, " DEFERRABLE");
		smart_str_0(&cmd);

		if (!PQsendQuery(conn_obj->intern->conn, smart_str_v(&cmd))) {
			throw_exce(EX_IO, "Failed to start transaction (%s)", PHP_PQerrorMessage(conn_obj->intern->conn));
		} else {
			rv = SUCCESS;
			conn_obj->intern->poller = PQconsumeInput;
			php_pqconn_notify_listeners(conn_obj);
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
	php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);
	zend_long isolation = obj->intern ? obj->intern->default_txn_isolation : PHP_PQTXN_READ_COMMITTED;
	zend_bool readonly = obj->intern ? obj->intern->default_txn_readonly : 0;
	zend_bool deferrable = obj->intern ? obj->intern->default_txn_deferrable : 0;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|lbb", &isolation, &readonly, &deferrable);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		rv = php_pqconn_start_transaction(getThis(), obj, isolation, readonly, deferrable);

		if (SUCCESS == rv) {
			php_pqtxn_t *txn = ecalloc(1, sizeof(*txn));

			php_pq_object_addref(obj);
			txn->conn = obj;
			txn->open = 1;
			txn->isolation = isolation;
			txn->readonly = readonly;
			txn->deferrable = deferrable;

			RETVAL_OBJ(&php_pqtxn_create_object_ex(php_pqtxn_class_entry, txn)->zo);
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
	php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);
	zend_long isolation = obj->intern ? obj->intern->default_txn_isolation : PHP_PQTXN_READ_COMMITTED;
	zend_bool readonly = obj->intern ? obj->intern->default_txn_readonly : 0;
	zend_bool deferrable = obj->intern ? obj->intern->default_txn_deferrable : 0;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|lbb", &isolation, &readonly, &deferrable);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		rv = php_pqconn_start_transaction_async(getThis(), obj, isolation, readonly, deferrable);

		if (SUCCESS == rv) {
			php_pqtxn_t *txn = ecalloc(1, sizeof(*txn));

			php_pq_object_addref(obj);
			txn->conn = obj;
			txn->open = 1;
			txn->isolation = isolation;
			txn->readonly = readonly;
			txn->deferrable = deferrable;

			RETVAL_OBJ(&php_pqtxn_create_object_ex(php_pqtxn_class_entry, txn)->zo);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_trace, 0, 0, 0)
	ZEND_ARG_INFO(0, stdio_stream)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, trace) {
	zval *zstream = NULL;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS(), "|r!", &zstream)) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			if (!zstream) {
				PQuntrace(obj->intern->conn);
				RETVAL_TRUE;
			} else {
				FILE *fp;
				php_stream *stream = NULL;

				php_stream_from_zval(stream, zstream);

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
	zend_string *type;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "S", &type);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			RETURN_BOOL(SUCCESS == zend_hash_del(&obj->intern->eventhandlers, type));
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
	size_t type_len;
	php_pq_callback_t cb = PHP_PQ_CALLBACK_INIT;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "sf", &type_str, &type_len, &cb.fci, &cb.fcc);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			RETVAL_LONG(php_pqconn_add_eventhandler(obj, type_str, type_len, &cb));
		}
	}
}

struct apply_set_converter_arg {
	HashTable *ht;
	zval *zconv;
	unsigned add:1;
};

static int apply_set_converter(zval *zoid, void *a)
{
	zend_long oid = zval_get_long(zoid);
	struct apply_set_converter_arg *arg = a;

	if (arg->add) {
		Z_ADDREF_P(arg->zconv);
		zend_hash_index_update(arg->ht, oid, arg->zconv);
	} else {
		zend_hash_index_del(arg->ht, oid);
	}

	return ZEND_HASH_APPLY_KEEP;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_set_converter, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, converter, pq\\Converter, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, setConverter) {
	ZEND_RESULT_CODE rv;
	zend_error_handling zeh;
	zval *zcnv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "O", &zcnv, php_pqconv_class_entry);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			zval tmp, zoids;
			struct apply_set_converter_arg arg = {NULL};

			ZVAL_NULL(&zoids);
			php_pq_call_method(zcnv, "converttypes", 0, &zoids);
			ZVAL_DUP(&tmp, &zoids);
			convert_to_array(&tmp);

			arg.ht = &obj->intern->converters;
			arg.zconv = zcnv;
			arg.add = 1;

			zend_hash_apply_with_argument(Z_ARRVAL(tmp), apply_set_converter, &arg);

			zval_ptr_dtor(&tmp);
			zval_ptr_dtor(&zoids);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_unset_converter, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, converter, pq\\Converter, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, unsetConverter) {
	ZEND_RESULT_CODE rv;
	zend_error_handling zeh;
	zval *zcnv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "O", &zcnv, php_pqconv_class_entry);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqconn_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Connection not initialized");
		} else {
			zval tmp, zoids;
			struct apply_set_converter_arg arg = {NULL};

			ZVAL_NULL(&zoids);
			php_pq_call_method(zcnv, "converttypes", 0, &zoids);
			ZVAL_DUP(&tmp, &zoids);
			convert_to_array(&tmp);

			arg.ht = &obj->intern->converters;
			arg.zconv = zcnv;
			arg.add = 0;

			zend_hash_apply_with_argument(Z_ARRVAL(tmp), apply_set_converter, &arg);

			zval_ptr_dtor(&tmp);
			zval_ptr_dtor(&zoids);
		}
	}
}

static zend_function_entry php_pqconn_methods[] = {
	PHP_ME(pqconn, __construct, ai_pqconn_construct, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, reset, ai_pqconn_reset, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, resetAsync, ai_pqconn_reset_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, poll, ai_pqconn_poll, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, flush, ai_pqconn_flush, ZEND_ACC_PUBLIC)
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
	php_persistent_handle_cleanup(PHP_PQ_G->connection.name, NULL);
	zend_string_release(PHP_PQ_G->connection.name);
	zend_hash_destroy(&php_pqconn_object_prophandlers);
	return SUCCESS;
}

PHP_MINIT_FUNCTION(pqconn)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Connection", php_pqconn_methods);
	php_pqconn_class_entry = zend_register_internal_class_ex(&ce, NULL);
	php_pqconn_class_entry->create_object = php_pqconn_create_object;

	memcpy(&php_pqconn_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqconn_object_handlers.offset = XtOffsetOf(php_pqconn_object_t, zo);
	php_pqconn_object_handlers.free_obj = php_pqconn_object_free;
	php_pqconn_object_handlers.read_property = php_pq_object_read_prop;
	php_pqconn_object_handlers.write_property = php_pq_object_write_prop;
	php_pqconn_object_handlers.clone_obj = NULL;
	php_pqconn_object_handlers.get_property_ptr_ptr = php_pq_object_get_prop_ptr_null;
	php_pqconn_object_handlers.get_gc = php_pq_object_get_gc;
	php_pqconn_object_handlers.get_properties = php_pq_object_properties;
	php_pqconn_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqconn_object_prophandlers, 23, NULL, php_pq_object_prophandler_dtor, 1);

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("status"), CONNECTION_BAD, ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_status;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "status", sizeof("status")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("transactionStatus"), PQTRANS_UNKNOWN, ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_transaction_status;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "transactionStatus", sizeof("transactionStatus")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("socket"), ZEND_ACC_PUBLIC);
	ph.read = NULL; /* forward to std prophandler */
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "socket", sizeof("socket")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("errorMessage"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_error_message;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "errorMessage", sizeof("errorMessage")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("busy"), 0, ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_busy;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "busy", sizeof("busy")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("encoding"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_encoding;
	ph.write = php_pqconn_object_write_encoding;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "encoding", sizeof("encoding")-1, (void *) &ph, sizeof(ph));
	ph.write = NULL;

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("unbuffered"), 0, ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_unbuffered;
	ph.write = php_pqconn_object_write_unbuffered;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "unbuffered", sizeof("unbuffered")-1, (void *) &ph, sizeof(ph));
	ph.write = NULL;

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("nonblocking"), 0, ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_nonblocking;
	ph.write = php_pqconn_object_write_nonblocking;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "nonblocking", sizeof("nonblocking")-1, (void *) &ph, sizeof(ph));
	ph.write = NULL;

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("db"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_db;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "db", sizeof("db")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("user"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_user;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "user", sizeof("user")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("pass"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_pass;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "pass", sizeof("pass")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("host"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_host;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "host", sizeof("host")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("port"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_port;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "port", sizeof("port")-1, (void *) &ph, sizeof(ph));

#if HAVE_PQCONNINFO
	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("params"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_params;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "params", sizeof("params")-1, (void *) &ph, sizeof(ph));
#endif

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("options"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_options;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "options", sizeof("options")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("eventHandlers"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_event_handlers;
	ph.gc = php_pqconn_object_gc_event_handlers;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "eventHandlers", sizeof("eventHandlers")-1, (void *) &ph, sizeof(ph));
	ph.gc = NULL;

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("listeners"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_listeners;
	ph.gc = php_pqconn_object_gc_listeners;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "listeners", sizeof("listeners")-1, (void *) &ph, sizeof(ph));
	ph.gc = NULL;

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("converters"), ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_converters;
	ph.gc = php_pqconn_object_gc_converters;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "converters", sizeof("converters")-1, (void *) &ph, sizeof(ph));
	ph.gc = NULL;

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("defaultFetchType"), 0, ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_def_fetch_type;
	ph.write = php_pqconn_object_write_def_fetch_type;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "defaultFetchType", sizeof("defaultFetchType")-1, (void *) &ph, sizeof(ph));
	ph.write = NULL;

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("defaultTransactionIsolation"), 0, ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_def_txn_isolation;
	ph.write = php_pqconn_object_write_def_txn_isolation;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "defaultTransactionIsolation", sizeof("defaultTransactionIsolation")-1, (void *) &ph, sizeof(ph));
	ph.write = NULL;

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("defaultTransactionReadonly"), 0, ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_def_txn_readonly;
	ph.write = php_pqconn_object_write_def_txn_readonly;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "defaultTransactionReadonly", sizeof("defaultTransactionReadonly")-1, (void *) &ph, sizeof(ph));
	ph.write = NULL;

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("defaultTransactionDeferrable"), 0, ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_def_txn_deferrable;
	ph.write = php_pqconn_object_write_def_txn_deferrable;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "defaultTransactionDeferrable", sizeof("defaultTransactionDeferrable")-1, (void *) &ph, sizeof(ph));
	ph.write = NULL;

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("defaultAutoConvert"), PHP_PQRES_CONV_ALL, ZEND_ACC_PUBLIC);
	ph.read = php_pqconn_object_read_def_auto_conv;
	ph.write = php_pqconn_object_write_def_auto_conv;
	zend_hash_str_add_mem(&php_pqconn_object_prophandlers, "defaultAutoConvert", sizeof("defaultAutoConvert")-1, (void *) &ph, sizeof(ph));
	ph.write = NULL;

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("OK"), CONNECTION_OK);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("BAD"), CONNECTION_BAD);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("STARTED"), CONNECTION_STARTED);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("MADE"), CONNECTION_MADE);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("AWAITING_RESPONSE"), CONNECTION_AWAITING_RESPONSE);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("AUTH_OK"), CONNECTION_AUTH_OK);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("SSL_STARTUP"), CONNECTION_SSL_STARTUP);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("SETENV"), CONNECTION_SETENV);
#ifdef HAVE_CONNECTION_CHECK_WRITABLE
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("CHECK_WRITABLE"), CONNECTION_CHECK_WRITABLE);
#endif
#ifdef HAVE_CONNECTION_CONSUME
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("CONSUME"), CONNECTION_CONSUME);
#endif
#ifdef HAVE_CONNECTION_GSS_STARTUP
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("GSS_STARTUP"), CONNECTION_GSS_STARTUP);
#endif

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_IDLE"), PQTRANS_IDLE);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_ACTIVE"), PQTRANS_ACTIVE);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_INTRANS"), PQTRANS_INTRANS);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_INERROR"), PQTRANS_INERROR);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_UNKNOWN"), PQTRANS_UNKNOWN);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_FAILED"), PGRES_POLLING_FAILED);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_READING"), PGRES_POLLING_READING);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_WRITING"), PGRES_POLLING_WRITING);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_OK"), PGRES_POLLING_OK);

	zend_declare_class_constant_stringl(php_pqconn_class_entry, ZEND_STRL("EVENT_NOTICE"), ZEND_STRL("notice"));
	zend_declare_class_constant_stringl(php_pqconn_class_entry, ZEND_STRL("EVENT_RESULT"), ZEND_STRL("result"));
	zend_declare_class_constant_stringl(php_pqconn_class_entry, ZEND_STRL("EVENT_RESET"), ZEND_STRL("reset"));

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("ASYNC"), 0x1);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("PERSISTENT"), 0x2);

	PHP_PQ_G->connection.name = zend_string_init(ZEND_STRL("pq\\Connection"), 1);

	return php_persistent_handle_provide(PHP_PQ_G->connection.name, php_pqconn_get_resource_factory_ops(), NULL, NULL);
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
