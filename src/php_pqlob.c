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

static void php_pqlob_object_free(void *o TSRMLS_DC)
{
	php_pqlob_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE lob(#%d) %p (txn(#%d): %p)\n", obj->zv.handle, obj, obj->intern->txn->zv.handle, obj->intern->txn);
#endif
	if (obj->intern) {
		if (obj->intern->lofd) {
			lo_close(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd);
		}
		/* invalidate the stream */
		if (obj->intern->stream) {
			zend_list_delete(obj->intern->stream);
			obj->intern->stream = 0;
		}
		php_pq_object_delref(obj->intern->txn TSRMLS_CC);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

zend_object_value php_pqlob_create_object_ex(zend_class_entry *ce, php_pqlob_t *intern, php_pqlob_object_t **ptr TSRMLS_DC)
{
	php_pqlob_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqlob_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqlob_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqlob_object_handlers;

	return o->zv;
}

static zend_object_value php_pqlob_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqlob_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static void php_pqlob_object_read_transaction(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqlob_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->txn, &return_value TSRMLS_CC);
}

static void php_pqlob_object_read_oid(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqlob_object_t *obj = o;

	RETVAL_LONG(obj->intern->loid);
}

static void php_pqlob_object_update_stream(zval *this_ptr, php_pqlob_object_t *obj, zval **zstream TSRMLS_DC);

static void php_pqlob_object_read_stream(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqlob_object_t *obj = o;

	if (!obj->intern->stream) {
		zval *zstream;

		php_pqlob_object_update_stream(object, obj, &zstream TSRMLS_CC);
		RETVAL_ZVAL(zstream, 1, 1);
	} else {
		RETVAL_RESOURCE(obj->intern->stream);
		zend_list_addref(obj->intern->stream);
	}
}

static size_t php_pqlob_stream_write(php_stream *stream, const char *buffer, size_t length TSRMLS_DC)
{
	php_pqlob_object_t *obj = stream->abstract;
	int written = 0;

	if (obj) {
		written = lo_write(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, buffer, length);

		if (written < 0) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to write to LOB with oid=%u (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
		}

		php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
	}

	return written;
}

static size_t php_pqlob_stream_read(php_stream *stream, char *buffer, size_t length TSRMLS_DC)
{
	php_pqlob_object_t *obj = stream->abstract;
	int read = 0;

	if (obj) {

		if (!buffer && !length) {
			if (lo_tell(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd) == lo_lseek(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, 0, SEEK_CUR)) {
				return EOF;
			}
		} else {
			read = lo_read(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, buffer, length);

			if (read < 0) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to read from LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			}
		}

		php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
	}

	return read;
}

static STATUS php_pqlob_stream_close(php_stream *stream, int close_handle TSRMLS_DC)
{
	return SUCCESS;
}

static int php_pqlob_stream_flush(php_stream *stream TSRMLS_DC)
{
	return SUCCESS;
}

static STATUS php_pqlob_stream_seek(php_stream *stream, off_t offset, int whence, off_t *newoffset TSRMLS_DC)
{
	STATUS rv = FAILURE;
	php_pqlob_object_t *obj = stream->abstract;

	if (obj) {
		int position = lo_lseek(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, offset, whence);

		if (position < 0) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to seek offset in LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			rv = FAILURE;
		} else {
			*newoffset = position;
			rv = SUCCESS;
		}

		php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
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

static void php_pqlob_object_update_stream(zval *this_ptr, php_pqlob_object_t *obj, zval **zstream_ptr TSRMLS_DC)
{
	zval *zstream, zmember;
	php_stream *stream;

	INIT_PZVAL(&zmember);
	ZVAL_STRINGL(&zmember, "stream", sizeof("stream")-1, 0);

	MAKE_STD_ZVAL(zstream);
	if (!obj) {
		obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	}
	stream = php_stream_alloc(&php_pqlob_stream_ops, obj, NULL, "r+b");
	stream->flags |= PHP_STREAM_FLAG_NO_FCLOSE;
	zend_list_addref(obj->intern->stream = stream->rsrc_id);
	php_stream_to_zval(stream, zstream);

	zend_get_std_object_handlers()->write_property(getThis(), &zmember, zstream, NULL TSRMLS_CC);

	if (zstream_ptr) {
		*zstream_ptr = zstream;
	} else {
		zval_ptr_dtor(&zstream);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, transaction, pq\\Transaction, 0)
	ZEND_ARG_INFO(0, oid)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, __construct) {
	zend_error_handling zeh;
	zval *ztxn;
	long mode = INV_WRITE|INV_READ, loid = InvalidOid;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|ll", &ztxn, php_pqtxn_class_entry, &loid, &mode);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *txn_obj = zend_object_store_get_object(ztxn TSRMLS_CC);

		if (!txn_obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!txn_obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transation already closed");
		} else {
			if (loid == InvalidOid) {
				loid = lo_creat(txn_obj->intern->conn->intern->conn, mode);
			}

			if (loid == InvalidOid) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to create large object with mode '%s' (%s)", strmode(mode), PHP_PQerrorMessage(txn_obj->intern->conn->intern->conn));
			} else {
				int lofd = lo_open(txn_obj->intern->conn->intern->conn, loid, mode);

				if (lofd < 0) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to open large object with oid=%u with mode '%s' (%s)", loid, strmode(mode), PHP_PQerrorMessage(txn_obj->intern->conn->intern->conn));
				} else {
					php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

					obj->intern = ecalloc(1, sizeof(*obj->intern));
					obj->intern->lofd = lofd;
					obj->intern->loid = loid;
					php_pq_object_addref(txn_obj TSRMLS_CC);
					obj->intern->txn = txn_obj;
				}
			}

			php_pqconn_notify_listeners(txn_obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_write, 0, 0, 1)
	ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, write) {
	zend_error_handling zeh;
	char *data_str;
	int data_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &data_str, &data_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\LOB not initialized");
		} else {
			int written = lo_write(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, data_str, data_len);

			if (written < 0) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to write to LOB with oid=%u (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(written);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_read, 0, 0, 0)
	ZEND_ARG_INFO(0, length)
	ZEND_ARG_INFO(1, read)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, read) {
	zend_error_handling zeh;
	long length = 0x1000;
	zval *zread = NULL;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lz!", &length, &zread);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\LOB not initialized");
		} else {
			char *buffer = emalloc(length + 1);
			int read = lo_read(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, buffer, length);

			if (read < 0) {
				efree(buffer);
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to read from LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				if (zread) {
					zval_dtor(zread);
					ZVAL_LONG(zread, read);
				}
				buffer[read] = '\0';
				RETVAL_STRINGL(buffer, read, 0);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_seek, 0, 0, 1)
	ZEND_ARG_INFO(0, offset)
	ZEND_ARG_INFO(0, whence)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, seek) {
	zend_error_handling zeh;
	long offset, whence = SEEK_SET;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|l", &offset, &whence);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\LOB not initialized");
		} else {
			int position = lo_lseek(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, offset, whence);

			if (position < 0) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to seek offset in LOB with oid=%d (%s)", obj->intern->loid,	PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(position);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_tell, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, tell) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\LOB not initialized");
		} else {
			int position = lo_tell(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd);

			if (position < 0) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to tell offset in LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(position);
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqlob_truncate, 0, 0, 0)
	ZEND_ARG_INFO(0, length)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqlob, truncate) {
	zend_error_handling zeh;
	long length = 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &length);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqlob_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\LOB not initialized");
		} else {
			int rc = lo_truncate(obj->intern->txn->intern->conn->intern->conn, obj->intern->lofd, length);

			if (rc != 0) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to truncate LOB with oid=%d (%s)", obj->intern->loid, PHP_PQerrorMessage(obj->intern->txn->intern->conn->intern->conn));
			}

			php_pqconn_notify_listeners(obj->intern->txn->intern->conn TSRMLS_CC);
		}
	}
}

static zend_function_entry php_pqlob_methods[] = {
	PHP_ME(pqlob, __construct, ai_pqlob_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqlob, write, ai_pqlob_write, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, read, ai_pqlob_read, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, seek, ai_pqlob_seek, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, tell, ai_pqlob_tell, ZEND_ACC_PUBLIC)
	PHP_ME(pqlob, truncate, ai_pqlob_truncate, ZEND_ACC_PUBLIC)
	{0}
};

PHP_MINIT_FUNCTION(pqlob)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "LOB", php_pqlob_methods);
	php_pqlob_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqlob_class_entry->create_object = php_pqlob_create_object;

	memcpy(&php_pqlob_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqlob_object_handlers.read_property = php_pq_object_read_prop;
	php_pqlob_object_handlers.write_property = php_pq_object_write_prop;
	php_pqlob_object_handlers.clone_obj = NULL;
	php_pqlob_object_handlers.get_property_ptr_ptr = NULL;
	php_pqlob_object_handlers.get_gc = NULL;
	php_pqlob_object_handlers.get_properties = php_pq_object_properties;
	php_pqlob_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqlob_object_prophandlers, 3, NULL, NULL, 1);

	zend_declare_property_null(php_pqlob_class_entry, ZEND_STRL("transaction"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqlob_object_read_transaction;
	zend_hash_add(&php_pqlob_object_prophandlers, "transaction", sizeof("transaction"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqlob_class_entry, ZEND_STRL("oid"), InvalidOid, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqlob_object_read_oid;
	zend_hash_add(&php_pqlob_object_prophandlers, "oid", sizeof("oid"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqlob_class_entry, ZEND_STRL("stream"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqlob_object_read_stream;
	zend_hash_add(&php_pqlob_object_prophandlers, "stream", sizeof("stream"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("INVALID_OID"), InvalidOid TSRMLS_CC);
	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("R"), INV_READ TSRMLS_CC);
	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("W"), INV_WRITE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqlob_class_entry, ZEND_STRL("RW"), INV_READ|INV_WRITE TSRMLS_CC);

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
