PHP_ARG_WITH(pq, [whether to enable libpq (PostgreSQL) support],
[  --with-pq[=DIR]           Include libpq support])

if test "$PHP_PQ" != "no"; then

	SEARCH_PATH="/usr/local /usr /opt"
	if test "$PHP_PQ" != "yes"; then
		SEARCH_PATH="$PHP_PQ $SEARCH_PATH"
	fi

	AC_MSG_CHECKING(for pg_config)
	for i in $SEARCH_PATH; do
		if test -x "$i/bin/pg_config"; then
			PG_CONFIG="$i/bin/pg_config"
			break
		fi
	done

	if test -z "$PG_CONFIG"; then
		AC_PATH_PROG(PG_CONFIG, pg_config, no)
	fi

	AC_MSG_RESULT($PG_CONFIG)

	if test "$PG_CONFIG" = "no"; then
		AC_MSG_ERROR(could not find a usable pg_config in $SEARCH_PATH)
	else
		if test "$PHP_PQ" != "yes" -a "$PHP_PQ/bin/pg_config" != "$PG_CONFIG"; then
			AC_MSG_WARN(Found pg_config is not in $PHP_PQ)
		fi

		AC_MSG_CHECKING(for PostgreSQL version)
		PQ_VERSION=$($PG_CONFIG --version | $SED 's/PostgreSQL //')

		if test -z "$PQ_VERSION"; then
			AC_MSG_RESULT(not found)
			AC_MSG_ERROR(\`$PG_CONFIG --version\` did not provide any meaningful output, please reinstall postgresql/libpq)
		else
			AC_MSG_RESULT($PQ_VERSION)
			AC_DEFINE_UNQUOTED(PHP_PQ_LIBVERSION, "$PQ_VERSION", [ ])
		fi

		PQ_INCDIR=$($PG_CONFIG --includedir)
		PQ_LIBDIR=$($PG_CONFIG --libdir)
	fi

	PHP_ADD_INCLUDE($PQ_INCDIR)

	ifdef([AC_PROG_EGREP], [
		AC_PROG_EGREP
	], [
		AC_CHECK_PROG(EGREP, egrep, egrep)
	])

	dnl
	dnl PQ_CHECK_CONST(name)
	dnl
	AC_DEFUN([PQ_CHECK_CONST], [
		AC_MSG_CHECKING(for $1)
		if $EGREP -q $1 $PQ_INCDIR/libpq-fe.h; then
			AC_DEFINE(HAVE_$1, 1, [Have $1])
			AC_MSG_RESULT(yep)
		else
			AC_MSG_RESULT(nope)
		fi
	])

	PQ_CHECK_CONST(PGRES_SINGLE_TUPLE)
	PQ_CHECK_CONST(PGRES_COPY_BOTH)

	PQ_CHECK_CONST(CONNECTION_CHECK_WRITABLE)
	PQ_CHECK_CONST(CONNECTION_CONSUME)
	PQ_CHECK_CONST(CONNECTION_GSS_STARTUP)

	dnl
	dnl PQ_CHECK_FUNC(sym, fail-hard)
	dnl
	AC_DEFUN([PQ_CHECK_FUNC], [
		PQ_SYM=$1
		FAIL_HARD=$2
		save_LIBS="$LIBS"
		LIBS=
		PHP_CHECK_LIBRARY(pq, $1, [
			AC_DEFINE([HAVE_]translit($1,a-z,A-Z), 1, Have $1)
		], [
			if test -n "$FAIL_HARD"; then
				if $FAIL_HARD; then
					AC_MSG_ERROR(could not find $PQ_SYM in -lpq -L$PQ_LIBDIR)
				fi
			fi
		], [
			-L$PQ_LIBDIR
		])
		LIBS="$save_LIBS"
	])

	PQ_CHECK_FUNC(PQregisterEventProc, true)
	PHP_ADD_LIBRARY_WITH_PATH(pq, $PQ_LIBDIR, PQ_SHARED_LIBADD)
	PHP_SUBST(PQ_SHARED_LIBADD)

	PQ_CHECK_FUNC(PQlibVersion)
	PQ_CHECK_FUNC(PQprotocolVersion)
	PQ_CHECK_FUNC(PQserverVersion)
	PQ_CHECK_FUNC(PQconninfo)
	PQ_CHECK_FUNC(PQsetSingleRowMode)

	dnl
	dnl PQ_HAVE_PHP_EXT(name[, code-if-yes[, code-if-not]])
	dnl
	AC_DEFUN([PQ_HAVE_PHP_EXT], [
		extname=$1
		haveext=$[PHP_]translit($1,a-z_-,A-Z__)
		AC_MSG_CHECKING([for ext/$extname support])
		if test "$haveext" != "no" && test "x$haveext" != "x"; then
			[PHP_PQ_HAVE_EXT_]translit($1,a-z_-,A-Z__)=1
			AC_MSG_RESULT([yes])
			$2
		elif test -x "$PHP_EXECUTABLE"; then
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
fi
