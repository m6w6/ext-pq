PHP_ARG_WITH(pq, for pq support,
[  --with-pq             Include pq support])

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
		AC_MSG_FAILURE(could not find include/libpq-events.h)
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
	PHP_CHECK_LIBRARY(pq, PQlibVersion, AC_DEFINE(HAVE_PQLIBVERSION, 1, Have PQlibVersion))

	PQ_SRC="src/php_pq.c"
	PHP_NEW_EXTENSION(pq, $PQ_SRC, $ext_shared)
    PHP_ADD_BUILD_DIR($ext_builddir/src, 1)
fi

