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

#include <libpq-events.h>
#include <libpq/libpq-fs.h>
#include <php.h>

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pq_object.h"
#include "php_pqexc.h"
#include "php_pqlob.h"

zend_class_entry *php_pqlob_class_entry;
static zend_object_handlers php_pqlob_object_handlers;
static HashTable php_pqlob_object_prophandlers;

static void php_pqlob_object_free(zend_object *o)
{
	php_pqlob_object_t *obj = PHP_PQ_OBJ(NULL, o);
#if DBG_GC
	fprintf(stderr, "FREE lob(#%d) %p (txn(#%d): %p)\n", obj->zo.handle, obj, obj->intern->txn->zo.handle, obj->intern->txn);
#endif
	if (obj->intern) {
		if (obj->intern->lofd) {
			lo_close(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd);
		}
		/* invalidate the stream */
		if (obj->intern->stream) {
			zend_list_delete(obj->intern->stream->res);
			obj->intern->stream = NULL;
		}
		php_pq_object_delref(obj->intern->txn);
		efree(obj->intern);
		obj->intern = NULL;
	}
	php_pq_object_dtor(o);
}

php_pqlob_object_t *php_pqlob_create_object_ex(zend_class_entry *ce, php_pqlob_t *intern)
{
	return php_pq_object_create(ce, intern, sizeof(php_pqlob_object_t),
			&php_pqlob_object_handlers, &php_pqlob_object_prophandlers);
}

static zend_object *php_pqlob_create_object(zend_class_entry *class_type)
{
	return &php_pqlob_create_object_ex(class_type, NULL)->zo;
}

static void php_pqlob_object_read_transaction(void *o, zval *return_value)
{
	php_pqlob_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->txn, return_value);
}

static void php_pqlob_object_gc_transaction(void *o, zval *return_value)
{
	php_pqlob_object_t *obj = o;
	zval ztxn;

	php_pq_object_to_zval_no_addref(obj->intern->txn, &ztxn);
	add_next_index_zval(return_value, &ztxn);
}

static void php_pqlob_object_read_oid(void *o, zval *return_value)
{
	php_pqlob_object_t *obj = o;

	RETVAL_LONG(obj->intern->loid);
}

static void php_pqlob_object_update_stream(php_pqlob_object_t *obj, zval *zstream);

static void php_pqlob_object_read_stream(void *o, zval *return_value)
{
	php_pqlob_object_t *obj = o;
	zval zstream;

	if (!obj->intern->stream) {
		php_pqlob_object_update_stream(obj, &zstream);
	} else {
		php_stream_to_zval(obj->intern->stream, &zstream);
	}

	RETVAL_ZVAL(&zstream, 1, 0);
}

static ssize_t php_pqlob_stream_write(php_stream *stream, const char *buffer, size_t length)
{
	php_pqlob_object_t *obj = stream->abstract;
	ssize_t written = 0;

	if (obj) {
		written = lo_write(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, buffer, length);

		if (written < 0) {
			php_error_docref(NULL, E_WARNING, "Failed to write to LOB with oid=%u (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
		}

		php_pqconn_notify_listeners(obj->intern->txn->intern->conn);
	}

	return written;
}

static ssize_t php_pqlob_stream_read(php_stream *stream, char *buffer, size_t length)
{
	php_pqlob_object_t *obj = stream->abstract;
	ssize_t read = 0;

	if (obj) {

		if (!buffer && !length) {
			if (lo_tell(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd) == lo_lseek(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, 0, SEEK_CUR)) {
				return EOF;
			}
		} else {
			read = lo_read(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, buffer, length);

			if (read < 0) {
				php_error_docref(NULL, E_WARNING, "Failed to read from LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			}
		}

		php_pqconn_notify_listeners(obj->intern->txn->intern->conn);
	}

	return read;
}

static ZEND_RESULT_CODE php_pqlob_stream_close(php_stream *stream, int close_handle)
{
	return SUCCESS;
}

static int php_pqlob_stream_flush(php_stream *stream)
{
	return SUCCESS;
}

static ZEND_RESULT_CODE php_pqlob_stream_seek(php_stream *stream, off_t offset, int whence, off_t *newoffset)
{
	ZEND_RESULT_CODE rv = FAILURE;
	php_pqlob_object_t *obj = stream->abstract;

	if (obj) {
		int position = lo_lseek(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, offset, whence);

		if (position < 0) {
			php_error_docref(NULL, E_WARNING, "Failed to seek offset in LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			rv = FAILURE;
		} else {
			*newoffset = position;
			rv = SUCCESS;
		}

		php_pqconn_notify_listeners(obj->intern->txn->intern->conn);
	}

	return rv;
}

static php_stream_ops php_pqlob_stream_ops = {
	/* stdio like functions - these are mandatory! */
	php_pqlob_stream_write,
	php_pqlob_stream_read,
	php_pqlob_stream_close,
	php_pqlob_stream_flush,

	"pq\\LOB stream",

	/* these are optional */
	php_pqlob_stream_seek,
	NULL, /* cast */
	NULL, /* stat */
	NULL, /* set_option */
};

static void php_pqlob_object_update_stream(php_pqlob_object_t *obj, zval *zstream)
{
	zval zobj, zmember;

	ZVAL_STRINGL(&zmember, "stream", sizeof("stream")-1);

	obj->intern->stream = php_stream_alloc(&php_pqlob_stream_ops, obj, NULL, "r+b");
	obj->intern->stream->flags |= PHP_STREAM_FLAG_NO_FCLOSE;
	php_stream_to_zval(obj->intern->stream, zstream);

#if PHP_VERSION_ID >= 80000
	zend_get_std_object_handlers()->write_property(&obj->zo, Z_STR(zmember), zstream, NULL);
#else
	ZVAL_OBJ(&zobj, &obj->zo);
	zend_get_std_object_handlers()->write_property(&zobj, &zmember, zstream, NULL);
#endif
	zval_ptr_dtor(&zmember);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, transaction, pq\\Transaction, 0)
	ZEND_ARG_INFO(0, oid)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, __construct) {
	zend_error_handling zeh;
	zval *ztxn;
	zend_long mode = INV_WRITE|INV_READ, loid = InvalidOid;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "O|ll", &ztxn, php_pqtxn_class_entry, &loid, &mode);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);
		php_pqtxn_object_t *txn_obj = PHP_PQ_OBJ(ztxn, NULL);

		if (obj->intern) {
			throw_exce(EX_BAD_METHODCALL, "pq\\LOB already initialized");
		} else if (!txn_obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\Transaction not initialized");
		} else if (!txn_obj->intern->open) {
			throw_exce(EX_RUNTIME, "pq\\Transation already closed");
		} else {
			if (loid == InvalidOid) {
				loid = lo_creat(txn_obj->intern->conn->intern->conn, mode);
			}

			if (loid == InvalidOid) {
				throw_exce(EX_RUNTIME, "Failed to create large object with mode '%s' (%s)", php_pq_strmode(mode), PHP_PQerrorMessage(txn_obj->intern->conn->intern->conn));
			} else {
				int lofd = lo_open(txn_obj->intern->conn->intern->conn, loid, mode);

				if (lofd < 0) {
					throw_exce(EX_RUNTIME, "Failed to open large object with oid=%u with mode '%s' (%s)", loid, php_pq_strmode(mode), PHP_PQerrorMessage(txn_obj->intern->conn->intern->conn));
				} else {
					obj->intern = ecalloc(1, sizeof(*obj->intern));
					obj->intern->lofd = lofd;
					obj->intern->loid = loid;
					php_pq_object_addref(txn_obj);
					obj->intern->txn = txn_obj;
				}
			}

			php_pqconn_notify_listeners(txn_obj->intern->conn);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_write, 0, 0, 1)
	ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, write) {
	zend_error_handling zeh;
	char *data_str;
	size_t data_len;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "s", &data_str, &data_len);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\LOB not initialized");
		} else {
			int written = lo_write(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, data_str, data_len);

			if (written < 0) {
				throw_exce(EX_RUNTIME, "Failed to write to LOB with oid=%u (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(written);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_read, 0, 0, 0)
	ZEND_ARG_INFO(0, length)
	ZEND_ARG_INFO(1, read)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, read) {
	zend_error_handling zeh;
	zend_long length = 0x1000;
	zval *zread = NULL;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|lz!", &length, &zread);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\LOB not initialized");
		} else {
			zend_string *buffer = zend_string_alloc(length, 0);
			int read = lo_read(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, &buffer->val[0], length);

			if (read < 0) {
				zend_string_release(buffer);
				throw_exce(EX_RUNTIME, "Failed to read from LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				if (zread) {
					ZVAL_DEREF(zread);
					zval_dtor(zread);
					ZVAL_LONG(zread, read);
				}
				buffer->val[buffer->len = read] = '\0';
				RETVAL_STR(buffer);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_seek, 0, 0, 1)
	ZEND_ARG_INFO(0, offset)
	ZEND_ARG_INFO(0, whence)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, seek) {
	zend_error_handling zeh;
	zend_long offset, whence = SEEK_SET;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "l|l", &offset, &whence);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\LOB not initialized");
		} else {
			int position = lo_lseek(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, offset, whence);

			if (position < 0) {
				throw_exce(EX_RUNTIME, "Failed to seek offset in LOB with oid=%d (%s)", obj->intern->loid,	PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(position);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_tell, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, tell) {
	zend_error_handling zeh;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\LOB not initialized");
		} else {
			int position = lo_tell(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd);

			if (position < 0) {
				throw_exce(EX_RUNTIME, "Failed to tell offset in LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(position);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_truncate, 0, 0, 0)
	ZEND_ARG_INFO(0, length)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, truncate) {
	zend_error_handling zeh;
	zend_long length = 0;
	ZEND_RESULT_CODE rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh);
	rv = zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &length);
	zend_restore_error_handling(&zeh);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = PHP_PQ_OBJ(getThis(), NULL);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED, "pq\\LOB not initialized");
		} else {
			int rc = lo_truncate(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, length);

			if (rc != 0) {
				throw_exce(EX_RUNTIME, "Failed to truncate LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn);
		}
	}
}

static zend_function_entry php_pqlob_methods[] = {
	PHP_ME(pqlob, __construct, ai_pqlob_construct, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, write, ai_pqlob_write, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, read, ai_pqlob_read, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, seek, ai_pqlob_seek, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, tell, ai_pqlob_tell, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, truncate, ai_pqlob_truncate, ZEND_ACC_PUBLIC)
	{0}
};

PHP_MSHUTDOWN_FUNCTION(pqlob)
{
	zend_hash_destroy(&php_pqlob_object_prophandlers);
	return SUCCESS;
}

PHP_MINIT_FUNCTION(pqlob)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "LOB", php_pqlob_methods);
	php_pqlob_class_entry = zend_register_internal_class_ex(&ce, NULL);
	php_pqlob_class_entry->create_object = php_pqlob_create_object;

	memcpy(&php_pqlob_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqlob_object_handlers.offset = XtOffsetOf(php_pqlob_object_t, zo);
	php_pqlob_object_handlers.free_obj = php_pqlob_object_free;
	php_pqlob_object_handlers.read_property = php_pq_object_read_prop;
	php_pqlob_object_handlers.write_property = php_pq_object_write_prop;
	php_pqlob_object_handlers.clone_obj = NULL;
	php_pqlob_object_handlers.get_property_ptr_ptr = php_pq_object_get_prop_ptr_null;
	php_pqlob_object_handlers.get_gc = php_pq_object_get_gc;
	php_pqlob_object_handlers.get_properties = php_pq_object_properties;
	php_pqlob_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqlob_object_prophandlers, 3, NULL, php_pq_object_prophandler_dtor, 1);

	zend_declare_property_null(php_pqlob_class_entry, ZEND_STRL("transaction"), ZEND_ACC_PUBLIC);
	ph.read = php_pqlob_object_read_transaction;
	ph.gc = php_pqlob_object_gc_transaction;
	zend_hash_str_add_mem(&php_pqlob_object_prophandlers, "transaction", sizeof("transaction")-1, (void *) &ph, sizeof(ph));
	ph.gc = NULL;

	zend_declare_property_long(php_pqlob_class_entry, ZEND_STRL("oid"), InvalidOid, ZEND_ACC_PUBLIC);
	ph.read = php_pqlob_object_read_oid;
	zend_hash_str_add_mem(&php_pqlob_object_prophandlers, "oid", sizeof("oid")-1, (void *) &ph, sizeof(ph));

	zend_declare_property_null(php_pqlob_class_entry, ZEND_STRL("stream"), ZEND_ACC_PUBLIC);
	ph.read = php_pqlob_object_read_stream;
	zend_hash_str_add_mem(&php_pqlob_object_prophandlers, "stream", sizeof("stream")-1, (void *) &ph, sizeof(ph));

	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("INVALID_OID"), InvalidOid);
	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("R"), INV_READ);
	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("W"), INV_WRITE);
	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("RW"), INV_READ|INV_WRITE);

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
