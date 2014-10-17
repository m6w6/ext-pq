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

	
	dnl
	dnl PQ_CHECK_FUNC(sym, fail-hard)
	dnl
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
	
	dnl
	dnl PQ_HAVE_PHP_EXT(name[, code-if-yes[, code-if-not]])
	dnl
	AC_DEFUN([PQ_HAVE_PHP_EXT], [
		extname=$1
		haveext=$[PHP_]translit($1,a-z_-,A-Z__)
		AC_MSG_CHECKING([for ext/$extname support])
		if test -x "$PHP_EXECUTABLE"; then
			grepext=`$PHP_EXECUTABLE -m | $EGREP ^$extname\$`
			if test "$grepext" = "$extname"; then
				[PHP_PQ_HAVE_EXT_]translit($1,a-z_-,A-Z__)=1
				AC_MSG_RESULT([yes])
				$2
			else
				[PHP_PQ_HAVE_EXT_]translit($1,a-z_-,A-Z__)=
				AC_MSG_RESULT([no])
				$3
			fi
		elif test "$haveext" != "no" && test "x$haveext" != "x"; then
			[PHP_PQ_HAVE_EXT_]translit($1,a-z_-,A-Z__)=1
			AC_MSG_RESULT([yes])
			$2
		else
			[PHP_PQ_HAVE_EXT_]translit($1,a-z_-,A-Z__)=
			AC_MSG_RESULT([no])
			$3
		fi
	])
	
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
	
	PQ_HAVE_PHP_EXT([raphf], [
		AC_MSG_CHECKING([for php_raphf.h])
		PQ_EXT_RAPHF_INCDIR=
		for i in `echo $INCLUDES | $SED -e's/-I//g'` $abs_srcdir ../raphf; do
			if test -d $i; then
				if test -f $i/php_raphf.h; then
					PQ_EXT_RAPHF_INCDIR=$i
					break
				elif test -f $i/ext/raphf/php_raphf.h; then
					PQ_EXT_RAPHF_INCDIR=$i/ext/raphf
					break
				fi
			fi
		done
		if test "x$PQ_EXT_RAPHF_INCDIR" = "x"; then
			AC_MSG_ERROR([not found])
		else
			AC_MSG_RESULT([$PQ_EXT_RAPHF_INCDIR])
			AC_DEFINE([PHP_PQ_HAVE_PHP_RAPHF_H], [1], [Have ext/raphf support])
			PHP_ADD_INCLUDE([$PQ_EXT_RAPHF_INCDIR])
		fi
	], [
		AC_MSG_ERROR([Please install pecl/raphf and activate extension=raphf.$SHLIB_DL_SUFFIX_NAME in your php.ini])
	])
	PHP_ADD_EXTENSION_DEP(pq, raphf, true)
	PQ_HAVE_PHP_EXT([json], [
		AC_MSG_CHECKING([for php_json.h])
		PQ_EXT_JSON_INCDIR=
		for i in `echo $INCLUDES | $SED -e's/-I//g'` $abs_srcdir ../json ../jsonc ../jsond; do
			if test -d $i; then
				if test -f $i/php_json.h; then
					PQ_EXT_JSON_INCDIR=$i
					break
				elif test -f $i/ext/json/php_json.h; then
					PQ_EXT_JSON_INCDIR=$i/ext/json
					break
				fi
			fi
		done
		if test "x$PQ_EXT_JSON_INCDIR" = "x"; then
			AC_MSG_ERROR([not found])
		else
			AC_MSG_RESULT([$PQ_EXT_JSON_INCDIR])
			AC_DEFINE([PHP_PQ_HAVE_PHP_JSON_H], [1], [Have ext/json support])
			PHP_ADD_INCLUDE([$PQ_EXT_JSON_INCDIR])
		fi
	])
fi

