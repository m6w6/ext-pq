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

#ifndef PHP_PQ_PARAMS_H
#define PHP_PQ_PARAMS_H

typedef struct php_pq_params {
	struct {
		HashTable conv;
		unsigned count;
		Oid *oids;
	} type;
	struct {
		HashTable dtor;
		unsigned count;
		char **strings;
	} param;
#ifdef ZTS
	void ***ts;
#endif
} php_pq_params_t;

php_pq_params_t *php_pq_params_init(HashTable *conv, HashTable *oids, HashTable *params TSRMLS_DC);
void php_pq_params_free(php_pq_params_t **p);
unsigned php_pq_params_set_params(php_pq_params_t *p, HashTable *params);
unsigned php_pq_params_set_type_oids(php_pq_params_t *p, HashTable *oids);
unsigned php_pq_params_add_type_oid(php_pq_params_t *p, Oid type);
unsigned php_pq_params_add_param(php_pq_params_t *p, zval *param);
void php_pq_params_set_type_conv(php_pq_params_t *p, HashTable *conv);


#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
