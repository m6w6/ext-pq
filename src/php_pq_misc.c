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

#include <libpq/libpq-fs.h>

#include "php_pq.h"
#include "php_pq_misc.h"

char *rtrim(char *e)
{
	size_t l = strlen(e);

	while (l-- > 0 && e[l] == '\n') {
		e[l] = '\0';
	}
	return e;
}

const char *strmode(long mode)
{
	switch (mode & (INV_READ|INV_WRITE)) {
	case INV_READ|INV_WRITE:
		return "rw";
	case INV_READ:
		return "r";
	case INV_WRITE:
		return "w";
	default:
		return "-";
	}
}

int compare_index(const void *lptr, const void *rptr TSRMLS_DC)
{
	const Bucket *l = *(const Bucket **) lptr;
	const Bucket *r = *(const Bucket **) rptr;

	if (l->h < r->h) {
		return -1;
	}
	if (l->h > r->h) {
		return 1;
	}
	return 0;
}

static int apply_to_oid(void *p, void *arg TSRMLS_DC)
{
	Oid **types = arg;
	zval **ztype = p;

	if (Z_TYPE_PP(ztype) != IS_LONG) {
		convert_to_long_ex(ztype);
	}

	**types = Z_LVAL_PP(ztype);
	++*types;

	if (*ztype != *(zval **)p) {
		zval_ptr_dtor(ztype);
	}
	return ZEND_HASH_APPLY_KEEP;
}

static int apply_to_param(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	char ***params;
	HashTable *zdtor;
	zval **zparam = p;

	params = (char ***) va_arg(argv, char ***);
	zdtor = (HashTable *) va_arg(argv, HashTable *);

	if (Z_TYPE_PP(zparam) == IS_NULL) {
		**params = NULL;
		++*params;
	} else {
		if (Z_TYPE_PP(zparam) != IS_STRING) {
			convert_to_string_ex(zparam);
		}

		**params = Z_STRVAL_PP(zparam);
		++*params;

		if (*zparam != *(zval **)p) {
			zend_hash_next_index_insert(zdtor, zparam, sizeof(zval *), NULL);
		}
	}
	return ZEND_HASH_APPLY_KEEP;
}

int php_pq_types_to_array(HashTable *ht, Oid **types TSRMLS_DC)
{
	int count = zend_hash_num_elements(ht);

	*types = NULL;

	if (count) {
		Oid *tmp;

		/* +1 for when less types than params are specified */
		*types = tmp = ecalloc(count + 1, sizeof(**types));
		zend_hash_apply_with_argument(ht, apply_to_oid, &tmp TSRMLS_CC);
	}

	return count;
}

int php_pq_params_to_array(HashTable *ht, char ***params, HashTable *zdtor TSRMLS_DC)
{
	int count = zend_hash_num_elements(ht);

	*params = NULL;

	if (count) {
		char **tmp;

		*params = tmp = ecalloc(count, sizeof(char *));
		zend_hash_apply_with_arguments(ht TSRMLS_CC, apply_to_param, 2, &tmp, zdtor);
	}

	return count;
}

/*
Oid *php_pq_ntypes_to_array(zend_bool fill, int argc, ...)
{
	int i;
	Oid *oids = ecalloc(argc + 1, sizeof(*oids));
	va_list argv;

	va_start(argv, argc);
	for (i = 0; i < argc; ++i) {
		if (!fill || !i) {
			oids[i] = va_arg(argv, Oid);
		} else {
			oids[i] = oids[0];
		}
	}
	va_end(argv);

	return oids;
}
*/

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
