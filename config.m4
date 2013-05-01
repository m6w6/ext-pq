PHP_ARG_WITH(pq, [whether to enable libpq (PostgreSQL) support],
[  --with-pq[=DIR]           Include libpq support])
PHP_ARG_WITH(pq-postgresql, [where to find PostgreSQL server headers],
[  --with-pq-postgresql[=DIR]  PQ: Define some standard type OIDs from catalog/pg_type.h], $PHP_PQ)

if test "$PHP_PQ" != "no"; then
	SEARCH_PATH="/usr/local /usr /opt"
	if test "$PHP_PQ" != "yes"; then
		SEARCH_PATH="$PHP_PQ $SEARCH_PATH"
	fi
	for i in $SEARCH_PATH; do
		AC_MSG_CHECKING(for $i/include/libpq-events.h)
		if test -f "$i/include/libpq-events.h"; then
			PQ_DIR=$i
			AC_MSG_RESULT(yep)
			break
		fi
		AC_MSG_RESULT(nope)
	done

	if test -z "$PQ_DIR"; then
		AC_MSG_ERROR(could not find include/libpq-events.h)
	fi
	PHP_ADD_INCLUDE($PQ_DIR/include)

	PQ_SYM=PQregisterEventProc
	PHP_CHECK_LIBRARY(pq, $PQ_SYM, [
		PHP_ADD_LIBRARY_WITH_PATH(pq, $PQ_DIR/$PHP_LIBDIR, PQ_SHARED_LIBADD)
		PHP_SUBST(PQ_SHARED_LIBADD)
	],[
		AC_MSG_ERROR(could not find $PQ_SYM in -lpq)
	],[
		-L$PQ_DIR/$PHP_LIBDIR
	])
	PHP_CHECK_LIBRARY(pq, PQlibVersion, [AC_DEFINE(HAVE_PQLIBVERSION, 1, Have PQlibVersion)])
	
	PQ_SRC="\
		src/php_pq_module.c\
		src/php_pq_misc.c\
		src/php_pq_callback.c\
		src/php_pq_object.c\
		src/php_pqcancel.c\
		src/php_pqconn.c\
		src/php_pqconn_event.c\
		src/php_pqcopy.c\
		src/php_pqexc.c\
		src/php_pqlob.c\
		src/php_pqres.c\
		src/php_pqstm.c\
		src/php_pqtxn.c\
		src/php_pqtypes.c\
	"
	PHP_NEW_EXTENSION(pq, $PQ_SRC, $ext_shared)
	PHP_ADD_BUILD_DIR($ext_builddir/src)
	PHP_ADD_INCLUDE($ext_srcdir/src)
	PHP_ADD_EXTENSION_DEP(pq, raphf)
	
	if test "$PHP_PQ_POSTGRESQL" != "no"; then
		if test "$PHP_PQ_POSTGRESQL" != "yes"; then
			SEARCH_PATH="$PHP_PQ_POSTGRESQL $SEARCH_PATH"
		fi
		for i in $SEARCH_PATH; do
			CATALOG="$i/include/postgresql/server/catalog/pg_type.h"
			AC_MSG_CHECKING(for $CATALOG)
			if test -f "$CATALOG"; then
				AC_MSG_RESULT(yep)
				AC_PROG_AWK()
				$AWK -f $ext_srcdir/php_pq_type.awk < "$CATALOG" > $ext_srcdir/php_pq_type.h
				AC_DEFINE(HAVE_PHP_PQ_TYPE_H, 1, Have PostgreSQL type OID defs)
				break
			fi
			AC_MSG_RESULT(nope)
		done
	fi
fi

