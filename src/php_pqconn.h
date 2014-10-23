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


#ifndef PHP_PQCONN_H
#define PHP_PQCONN_H

#define PHP_PQCONN_ASYNC      0x01
#define PHP_PQCONN_PERSISTENT 0x02

#include <ext/raphf/php_raphf.h>
#include "php_pq_callback.h"
#include "php_pq_params.h"

typedef struct php_pqconn {
	PGconn *conn;
	int (*poller)(PGconn *);
	php_resource_factory_t factory;
	HashTable listeners;
	HashTable converters;
	HashTable eventhandlers;
	php_pq_callback_t onevent;
	unsigned unbuffered:1;
	unsigned default_fetch_type:2;
	unsigned default_txn_isolation:2;
	unsigned default_txn_readonly:1;
	unsigned default_txn_deferrable:1;
	unsigned default_auto_convert:16;
} php_pqconn_t;

typedef struct php_pqconn_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqconn_t *intern;
} php_pqconn_object_t;

typedef struct php_pqconn_resource_factory_data {
	char *dsn;
	long flags;
} php_pqconn_resource_factory_data_t;

extern php_resource_factory_ops_t *php_pqconn_get_resource_factory_ops(void);

extern zend_class_entry *php_pqconn_class_entry;
extern zend_object_value php_pqconn_create_object_ex(zend_class_entry *ce, php_pqconn_t *intern, php_pqconn_object_t **ptr TSRMLS_DC);
extern void php_pqconn_notify_listeners(php_pqconn_object_t *obj TSRMLS_DC);
extern STATUS php_pqconn_prepare(zval *object, php_pqconn_object_t *obj, const char *name, const char *query, php_pq_params_t *params TSRMLS_DC);
extern STATUS php_pqconn_prepare_async(zval *object, php_pqconn_object_t *obj, const char *name, const char *query, php_pq_params_t *params TSRMLS_DC);
extern STATUS php_pqconn_start_transaction(zval *zconn, php_pqconn_object_t *conn_obj, long isolation, zend_bool readonly, zend_bool deferrable TSRMLS_DC);
extern STATUS php_pqconn_start_transaction_async(zval *zconn, php_pqconn_object_t *conn_obj, long isolation, zend_bool readonly, zend_bool deferrable TSRMLS_DC);
extern STATUS php_pqconn_declare(zval *object, php_pqconn_object_t *obj, const char *decl TSRMLS_DC);
extern STATUS php_pqconn_declare_async(zval *object, php_pqconn_object_t *obj, const char *decl TSRMLS_DC);

extern PHP_MINIT_FUNCTION(pqconn);
extern PHP_MSHUTDOWN_FUNCTION(pqconn);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
