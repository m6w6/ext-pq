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
#include <ext/standard/php_smart_str.h>

#include <libpq-events.h>
#include <libpq/libpq-fs.h>

#include "php_pq.h"
#include "php_pq_misc.h"
#include "php_pq_object.h"
#include "php_pqexc.h"
#include "php_pqres.h"
#include "php_pqlob.h"
#include "php_pqtxn.h"

zend_class_entry *php_pqtxn_class_entry;
static zend_object_handlers php_pqtxn_object_handlers;
static HashTable php_pqtxn_object_prophandlers;

const char *isolation_level(long *isolation)
{
	switch (*isolation) {
	case PHP_PQTXN_SERIALIZABLE:
		return "SERIALIZABLE";
	case PHP_PQTXN_REPEATABLE_READ:
		return "REPEATABLE READ";
	default:
		*isolation = PHP_PQTXN_READ_COMMITTED;
		/* no break */
	case PHP_PQTXN_READ_COMMITTED:
		return "READ COMMITTED";
	}
}

static void php_pqtxn_object_free(void *o TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
#if DBG_GC
	fprintf(stderr, "FREE txn(#%d) %p (conn(#%d): %p)\n", obj->zv.handle, obj, obj->intern->conn->zv.handle, obj->intern->conn);
#endif
	if (obj->intern) {
		if (obj->intern->open) {
			PGresult *res = PQexec(obj->intern->conn->intern->conn, "ROLLBACK");

			if (res) {
				PHP_PQclear(res);
			}
		}
		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

zend_object_value php_pqtxn_create_object_ex(zend_class_entry *ce, php_pqtxn_t *intern, php_pqtxn_object_t **ptr TSRMLS_DC)
{
	php_pqtxn_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqtxn_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqtxn_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqtxn_object_handlers;

	return o->zv;
}

static zend_object_value php_pqtxn_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqtxn_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static void php_pqtxn_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static void php_pqtxn_object_read_isolation(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	RETVAL_LONG(obj->intern->isolation);
}

static void php_pqtxn_object_read_readonly(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->readonly);
}

static void php_pqtxn_object_read_deferrable(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->deferrable);
}

static void php_pqtxn_object_write_isolation(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
	php_pqtxn_isolation_t orig = obj->intern->isolation;
	zval *zisolation = value;
	PGresult *res;

	if (Z_TYPE_P(zisolation) != IS_LONG) {
		if (Z_REFCOUNT_P(value) > 1) {
			zval *tmp;
			MAKE_STD_ZVAL(tmp);
			ZVAL_ZVAL(tmp, zisolation, 1, 0);
			convert_to_long(tmp);
			zisolation = tmp;
		} else {
			convert_to_long_ex(&zisolation);
		}
	}

	switch ((obj->intern->isolation = Z_LVAL_P(zisolation))) {
	case PHP_PQTXN_READ_COMMITTED:
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION ISOLATION LEVEL READ COMMITED");
		break;
	case PHP_PQTXN_REPEATABLE_READ:
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
		break;
	case PHP_PQTXN_SERIALIZABLE:
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");
		break;
	default:
		obj->intern->isolation = orig;
		res = NULL;
		break;
	}

	if (zisolation != value) {
		zval_ptr_dtor(&zisolation);
	}

	if (res) {
		php_pqres_success(res TSRMLS_CC);
		PHP_PQclear(res);
	}
}

static void php_pqtxn_object_write_readonly(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
	PGresult *res;

	if ((obj->intern->readonly = zend_is_true(value))) {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION READ ONLY");
	} else {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION READ WRITE");
	}

	if (res) {
		php_pqres_success(res TSRMLS_CC);
		PHP_PQclear(res);
	}
}

static void php_pqtxn_object_write_deferrable(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
	PGresult *res;

	if ((obj->intern->deferrable = zend_is_true(value))) {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION DEFERRABLE");
	} else {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION NOT DEFERRABLE");
	}

	if (res) {
		php_pqres_success(res TSRMLS_CC);
		PHP_PQclear(res);
	}
}


ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
	ZEND_ARG_INFO(0, async)
	ZEND_ARG_INFO(0, isolation)
	ZEND_ARG_INFO(0, readonly)
	ZEND_ARG_INFO(0, deferrable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, __construct) {
	zend_error_handling zeh;
	zval *zconn;
	long isolation = PHP_PQTXN_READ_COMMITTED;
	zend_bool async = 0, readonly = 0, deferrable = 0;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|blbb", &zconn, php_pqconn_class_entry, &async, &isolation, &readonly, &deferrable);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (!conn_obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Connection not initialized");
		} else {
			if (async) {
				rv = php_pqconn_start_transaction_async(zconn, conn_obj, isolation, readonly, deferrable TSRMLS_CC);
			} else {
				rv = php_pqconn_start_transaction(zconn, conn_obj, isolation, readonly, deferrable TSRMLS_CC);
			}

			if (SUCCESS == rv) {
				php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

				obj->intern = ecalloc(1, sizeof(*obj->intern));

				php_pq_object_addref(conn_obj TSRMLS_CC);
				obj->intern->conn = conn_obj;
				obj->intern->open = 1;
				obj->intern->isolation = isolation;
				obj->intern->readonly = readonly;
				obj->intern->deferrable = deferrable;
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_savepoint, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, savepoint) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction already closed");
		} else {
			PGresult *res;
			smart_str cmd = {0};

			smart_str_appends(&cmd, "SAVEPOINT \"");
			smart_str_append_unsigned(&cmd, ++obj->intern->savepoint);
			smart_str_appends(&cmd, "\"");
			smart_str_0(&cmd);

			res = PQexec(obj->intern->conn->intern->conn, cmd.c);

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to create %s (%s)", cmd.c, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				php_pqres_success(res TSRMLS_CC);
				PHP_PQclear(res);
			}

			smart_str_free(&cmd);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_savepoint_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, savepointAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction already closed");
		} else {
			smart_str cmd = {0};

			smart_str_appends(&cmd, "SAVEPOINT \"");
			smart_str_append_unsigned(&cmd, ++obj->intern->savepoint);
			smart_str_appends(&cmd, "\"");
			smart_str_0(&cmd);

			if (!PQsendQuery(obj->intern->conn->intern->conn, cmd.c)) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to create %s (%s)", cmd.c, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}

			smart_str_free(&cmd);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_commit, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, commit) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transacation not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transacation already closed");
		} else {
			PGresult *res;
			smart_str cmd = {0};

			if (!obj->intern->savepoint) {
				res = PQexec(obj->intern->conn->intern->conn, "COMMIT");
			} else {
				smart_str_appends(&cmd, "RELEASE SAVEPOINT \"");
				smart_str_append_unsigned(&cmd, obj->intern->savepoint--);
				smart_str_appends(&cmd, "\"");
				smart_str_0(&cmd);

				res = PQexec(obj->intern->conn->intern->conn, cmd.c);
			}

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to %s (%s)", cmd.c ? cmd.c : "commit transaction", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					if (!cmd.c) {
						obj->intern->open = 0;
					}
				}
				PHP_PQclear(res);
			}

			smart_str_free(&cmd);
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_commit_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, commitAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction already closed");
		} else {
			int rc;
			smart_str cmd = {0};

			if (!obj->intern->savepoint) {
				rc = PQsendQuery(obj->intern->conn->intern->conn, "COMMIT");
			} else {
				smart_str_appends(&cmd, "RELEASE SAVEPOINT \"");
				smart_str_append_unsigned(&cmd, obj->intern->savepoint--);
				smart_str_appends(&cmd, "\"");
				smart_str_0(&cmd);

				rc = PQsendQuery(obj->intern->conn->intern->conn, cmd.c);
			}

			if (!rc) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to %s (%s)", cmd.c ? cmd.c : "commmit transaction", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (!cmd.c) {
					obj->intern->open = 0;
				}
				obj->intern->conn->intern->poller = PQconsumeInput;
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			}

			smart_str_free(&cmd);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_rollback, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, rollback) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction already closed");
		} else {
			PGresult *res;
			smart_str cmd = {0};

			if (!obj->intern->savepoint) {
				res = PQexec(obj->intern->conn->intern->conn, "ROLLBACK");
			} else {
				smart_str_appends(&cmd, "ROLLBACK TO SAVEPOINT \"");
				smart_str_append_unsigned(&cmd, obj->intern->savepoint--);
				smart_str_appends(&cmd, "\"");
				smart_str_0(&cmd);

				res = PQexec(obj->intern->conn->intern->conn, cmd.c);
			}

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to %s (%s)", cmd.c ? cmd.c : "rollback transaction", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					if (!cmd.c) {
						obj->intern->open = 0;
					}
				}
				PHP_PQclear(res);
			}

			smart_str_free(&cmd);
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_rollback_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, rollbackAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!obj->intern->open) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction already closed");
		} else {
			int rc;
			smart_str cmd = {0};

			if (!obj->intern->savepoint) {
				rc = PQsendQuery(obj->intern->conn->intern->conn, "ROLLBACK");
			} else {
				smart_str_appends(&cmd, "ROLLBACK TO SAVEPOINT \"");
				smart_str_append_unsigned(&cmd, obj->intern->savepoint--);
				smart_str_appends(&cmd, "\"");
				smart_str_0(&cmd);

				rc = PQsendQuery(obj->intern->conn->intern->conn, cmd.c);
			}

			if (!rc) {
				throw_exce(EX_IO TSRMLS_CC, "Failed to %s (%s)", cmd.c ? cmd.c : "rollback transaction", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (!cmd.c) {
					obj->intern->open = 0;
				}
				obj->intern->conn->intern->poller = PQconsumeInput;
			}

			smart_str_free(&cmd);
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_export_snapshot, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, exportSnapshot) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			PGresult *res = PQexec(obj->intern->conn->intern->conn, "SELECT pg_export_snapshot()");

			if (!res) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to export transaction snapshot (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					RETVAL_STRING(PQgetvalue(res, 0, 0), 1);
				}

				PHP_PQclear(res);
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_export_snapshot_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, exportSnapshotAsync) {
	zend_error_handling zeh;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters_none();
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (!PQsendQuery(obj->intern->conn->intern->conn, "SELECT pg_export_snapshot()")) {
			throw_exce(EX_IO TSRMLS_CC, "Failed to export transaction snapshot (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
		} else {
			obj->intern->conn->intern->poller = PQconsumeInput;
			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_import_snapshot, 0, 0, 1)
	ZEND_ARG_INFO(0, snapshot_id)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, importSnapshot) {
	zend_error_handling zeh;
	char *snapshot_str;
	int snapshot_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &snapshot_str, &snapshot_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (obj->intern->isolation < PHP_PQTXN_REPEATABLE_READ) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction must have at least isolation level REPEATABLE READ to be able to import a snapshot");
		} else {
			char *sid = PQescapeLiteral(obj->intern->conn->intern->conn, snapshot_str, snapshot_len);

			if (!sid) {
				throw_exce(EX_ESCAPE TSRMLS_CC, "Failed to quote snapshot identifier (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				PGresult *res;
				smart_str cmd = {0};

				smart_str_appends(&cmd, "SET TRANSACTION SNAPSHOT ");
				smart_str_appends(&cmd, sid);
				smart_str_0(&cmd);

				res = PQexec(obj->intern->conn->intern->conn, cmd.c);

				if (!res) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to import transaction snapshot (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else {
					php_pqres_success(res TSRMLS_CC);
					PHP_PQclear(res);
				}

				smart_str_free(&cmd);
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_import_snapshot_async, 0, 0, 1)
	ZEND_ARG_INFO(0, snapshot_id)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, importSnapshotAsync) {
	zend_error_handling zeh;
	char *snapshot_str;
	int snapshot_len;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &snapshot_str, &snapshot_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else if (obj->intern->isolation < PHP_PQTXN_REPEATABLE_READ) {
			throw_exce(EX_RUNTIME TSRMLS_CC, "pq\\Transaction must have at least isolation level REPEATABLE READ to be able to import a snapshot");
		} else {
			char *sid = PQescapeLiteral(obj->intern->conn->intern->conn, snapshot_str, snapshot_len);

			if (!sid) {
				throw_exce(EX_ESCAPE TSRMLS_CC, "Failed to quote snapshot identifier (%s)", PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				smart_str cmd = {0};

				smart_str_appends(&cmd, "SET TRANSACTION SNAPSHOT ");
				smart_str_appends(&cmd, sid);
				smart_str_0(&cmd);

				if (!PQsendQuery(obj->intern->conn->intern->conn, cmd.c)) {
					throw_exce(EX_IO TSRMLS_CC, "Failed to %s (%s)", cmd.c, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else {
					obj->intern->conn->intern->poller = PQconsumeInput;
				}

				smart_str_free(&cmd);
				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
			}
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_open_lob, 0, 0, 1)
	ZEND_ARG_INFO(0, oid)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, openLOB) {
	zend_error_handling zeh;
	long mode = INV_WRITE|INV_READ, loid;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|l", &loid, &mode);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			int lofd = lo_open(obj->intern->conn->intern->conn, loid, mode);

			if (lofd < 0) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to open large object with oid=%u with mode '%s' (%s)", loid, strmode(mode), PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				php_pqlob_t *lob = ecalloc(1, sizeof(*lob));

				lob->lofd = lofd;
				lob->loid = loid;
				php_pq_object_addref(obj TSRMLS_CC);
				lob->txn = obj;

				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqlob_create_object_ex(php_pqlob_class_entry, lob, NULL TSRMLS_CC);
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_create_lob, 0, 0, 0)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, createLOB) {
	zend_error_handling zeh;
	long mode = INV_WRITE|INV_READ;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &mode);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			Oid loid = lo_creat(obj->intern->conn->intern->conn, mode);

			if (loid == InvalidOid) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to create large object with mode '%s' (%s)", strmode(mode), PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				int lofd = lo_open(obj->intern->conn->intern->conn, loid, mode);

				if (lofd < 0) {
					throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to open large object with oid=%u with mode '%s': %s", loid, strmode(mode), PHP_PQerrorMessage(obj->intern->conn->intern->conn));
				} else {
					php_pqlob_t *lob = ecalloc(1, sizeof(*lob));

					lob->lofd = lofd;
					lob->loid = loid;
					php_pq_object_addref(obj TSRMLS_CC);
					lob->txn = obj;

					return_value->type = IS_OBJECT;
					return_value->value.obj = php_pqlob_create_object_ex(php_pqlob_class_entry, lob, NULL TSRMLS_CC);
				}
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_unlink_lob, 0, 0, 1)
	ZEND_ARG_INFO(0, oid)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, unlinkLOB) {
	zend_error_handling zeh;
	long loid;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &loid);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (SUCCESS == rv) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			int rc = lo_unlink(obj->intern->conn->intern->conn, loid);

			if (rc != 1) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to unlink LOB (oid=%u): %s", loid, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_import_lob, 0, 0, 1)
	ZEND_ARG_INFO(0, local_path)
	ZEND_ARG_INFO(0, oid)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, importLOB) {
	zend_error_handling zeh;
	char *path_str;
	int path_len;
	long oid = InvalidOid;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "p|l", &path_str, &path_len, &oid);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (rv == SUCCESS) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			if (oid == InvalidOid) {
				oid = lo_import(obj->intern->conn->intern->conn, path_str);
			} else {
				oid = lo_import_with_oid(obj->intern->conn->intern->conn, path_str, oid);
			}

			if (oid == InvalidOid) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to import LOB from '%s' (%s)", path_str, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			} else {
				RETVAL_LONG(oid);
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_export_lob, 0, 0, 2)
	ZEND_ARG_INFO(0, oid)
	ZEND_ARG_INFO(0, local_path)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, exportLOB) {
	zend_error_handling zeh;
	char *path_str;
	int path_len;
	long oid;
	STATUS rv;

	zend_replace_error_handling(EH_THROW, exce(EX_INVALID_ARGUMENT), &zeh TSRMLS_CC);
	rv = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "lp", &oid, &path_str, &path_len);
	zend_restore_error_handling(&zeh TSRMLS_CC);

	if (rv == SUCCESS) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (!obj->intern) {
			throw_exce(EX_UNINITIALIZED TSRMLS_CC, "pq\\Transaction not initialized");
		} else {
			int rc = lo_export(obj->intern->conn->intern->conn, oid, path_str);

			if (rc == -1) {
				throw_exce(EX_RUNTIME TSRMLS_CC, "Failed to export LOB (oid=%u) to '%s' (%s)", oid, path_str, PHP_PQerrorMessage(obj->intern->conn->intern->conn));
			}

			php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);
		}
	}
}

static zend_function_entry php_pqtxn_methods[] = {
	PHP_ME(pqtxn, __construct, ai_pqtxn_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqtxn, commit, ai_pqtxn_commit, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, rollback, ai_pqtxn_rollback, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, commitAsync, ai_pqtxn_commit_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, rollbackAsync, ai_pqtxn_rollback_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, savepoint, ai_pqtxn_savepoint, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, savepointAsync, ai_pqtxn_savepoint_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, exportSnapshot, ai_pqtxn_export_snapshot, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, exportSnapshotAsync, ai_pqtxn_export_snapshot_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, importSnapshot, ai_pqtxn_import_snapshot, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, importSnapshotAsync, ai_pqtxn_import_snapshot_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, openLOB, ai_pqtxn_open_lob, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, createLOB, ai_pqtxn_create_lob, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, unlinkLOB, ai_pqtxn_unlink_lob, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, importLOB, ai_pqtxn_import_lob, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, exportLOB, ai_pqtxn_export_lob, ZEND_ACC_PUBLIC)
	{0}
};

PHP_MSHUTDOWN_FUNCTION(pqtxn)
{
	zend_hash_destroy(&php_pqtxn_object_prophandlers);
	return SUCCESS;
}

PHP_MINIT_FUNCTION(pqtxn)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Transaction", php_pqtxn_methods);
	php_pqtxn_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqtxn_class_entry->create_object = php_pqtxn_create_object;

	memcpy(&php_pqtxn_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqtxn_object_handlers.read_property = php_pq_object_read_prop;
	php_pqtxn_object_handlers.write_property = php_pq_object_write_prop;
	php_pqtxn_object_handlers.clone_obj = NULL;
	php_pqtxn_object_handlers.get_property_ptr_ptr = NULL;
	php_pqtxn_object_handlers.get_gc = NULL;
	php_pqtxn_object_handlers.get_properties = php_pq_object_properties;
	php_pqtxn_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqtxn_object_prophandlers, 4, NULL, NULL, 1);

	zend_declare_property_null(php_pqtxn_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_connection;
	zend_hash_add(&php_pqtxn_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqtxn_class_entry, ZEND_STRL("isolation"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_isolation;
	ph.write = php_pqtxn_object_write_isolation;
	zend_hash_add(&php_pqtxn_object_prophandlers, "isolation", sizeof("isolation"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_bool(php_pqtxn_class_entry, ZEND_STRL("readonly"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_readonly;
	ph.write = php_pqtxn_object_write_readonly;
	zend_hash_add(&php_pqtxn_object_prophandlers, "readonly", sizeof("readonly"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_bool(php_pqtxn_class_entry, ZEND_STRL("deferrable"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_deferrable;
	ph.write = php_pqtxn_object_write_deferrable;
	zend_hash_add(&php_pqtxn_object_prophandlers, "deferrable", sizeof("deferrable"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_class_constant_long(php_pqtxn_class_entry, ZEND_STRL("READ_COMMITTED"), PHP_PQTXN_READ_COMMITTED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqtxn_class_entry, ZEND_STRL("REPEATABLE_READ"), PHP_PQTXN_REPEATABLE_READ TSRMLS_CC);
	zend_declare_class_constant_long(php_pqtxn_class_entry, ZEND_STRL("SERIALIZABLE"), PHP_PQTXN_SERIALIZABLE TSRMLS_CC);

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
