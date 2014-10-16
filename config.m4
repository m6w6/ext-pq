PHP_ARG_WITH(pq, [whether to enable libpq (PostgreSQL) support],
[  --with-pq[=DIR]           Include libpq support])

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
	
	ifdef([AC_PROG_EGREP], [
		AC_PROG_EGREP
	], [
		AC_CHECK_PROG(EGREP, egrep, egrep)
	])
	
	for PQ_DEF in PGRES_SINGLE_TUPLE PGRES_COPY_BOTH; do
		AC_MSG_CHECKING(for $PQ_DEF)
		if $EGREP -q $PQ_DEF $PQ_DIR/include/libpq-fe.h; then
			AC_DEFINE([$PQ_DEF], [1], [Have $PQ_DEF])
			AC_MSG_RESULT(yep)
		else
			AC_MSG_RESULT(nope)
		fi
	done 

	
	AC_DEFUN([PQ_CHECK_FUNC], [
		FAIL_HARD=$2
		
		PHP_CHECK_LIBRARY(pq, $1, [
			AC_DEFINE([HAVE_]translit($1,a-z,A-Z), 1, Have $1)
		], [
			if test -n "$FAIL_HARD"; then
				if "$FAIL_HARD"; then
					AC_MSG_ERROR(could not find $PQ_SYM in -lpq)
				fi
			fi
		], [
			-L$PQ_DIR/$PHP_LIBDIR
		])
	])
	
	PQ_CHECK_FUNC(PQregisterEventProc, true)
	PHP_ADD_LIBRARY_WITH_PATH(pq, $PQ_DIR/$PHP_LIBDIR, PQ_SHARED_LIBADD)
	PHP_SUBST(PQ_SHARED_LIBADD)
	
	PQ_CHECK_FUNC(PQlibVersion)
	PQ_CHECK_FUNC(PQconninfo)
	PQ_CHECK_FUNC(PQsetSingleRowMode)
	
	PQ_SRC="\
		src/php_pq_module.c\
		src/php_pq_misc.c\
		src/php_pq_callback.c\
		src/php_pq_object.c\
		src/php_pq_params.c\
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
		src/php_pqcur.c\
	"
	PHP_NEW_EXTENSION(pq, $PQ_SRC, $ext_shared)
	PHP_ADD_BUILD_DIR($ext_builddir/src)
	PHP_ADD_INCLUDE($ext_srcdir/src)
	PHP_ADD_EXTENSION_DEP(pq, raphf)
	
fi

