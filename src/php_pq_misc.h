/*
    +--------------------------------------------------------------------+
    | PECL :: pq                                                         |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted provided that the conditions mentioned |
    | in the accompanying LICENSE file are met.                          |
    +--------------------------------------------------------------------+
    | Copyright (c) 2013, Michael Wallner <mike@php.net>                |
    +--------------------------------------------------------------------+
*/


#ifndef PHP_PQ_ERROR_H
#define PHP_PQ_ERROR_H

#include <libpq-fe.h>

typedef int STATUS; /* SUCCESS/FAILURE */

/* trim LF from EOL */
char *rtrim(char *e);

/* R, W, RW */
const char *strmode(long mode);

/* compare array index */
int compare_index(const void *lptr, const void *rptr TSRMLS_DC);

#define PHP_PQerrorMessage(c) rtrim(PQerrorMessage((c)))
#define PHP_PQresultErrorMessage(r) rtrim(PQresultErrorMessage((r)))

int php_pq_types_to_array(HashTable *ht, Oid **types TSRMLS_DC);
int php_pq_params_to_array(HashTable *ht, char ***params, HashTable *zdtor TSRMLS_DC);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
