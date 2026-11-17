 /*
  * fb.c
  * A module to access the Firebird database from Ruby.
  * Fork of interbase.c to fb.c by Brent Rowland.
  * All changes, improvements and associated bugs Copyright (C) 2006 Brent Rowland and Target Training International.
  * License to all changes, improvements and bugs is granted under the same terms as the original and/or
  * the Ruby license, whichever is most applicable.
  * Based on interbase.c
  *
  *                               Copyright (C) 1999 by NaCl inc.
  *                               Copyright (C) 1997,1998 by RIOS Corporation
  *
  * Permission to use, copy, modify, and distribute this software and its
  * documentation for any purpose and without fee is hereby granted, provided
  * that the above copyright notice appear in all copies.
  * RIOS Corporation makes no representations about the suitability of
  * this software for any purpose.  It is provided "as is" without express
  * or implied warranty.  By use of this software the user agrees to
  * indemnify and hold harmless RIOS Corporation from any  claims or
  * liability for loss arising out of such use.
  */

#include "ruby.h"

#ifdef HAVE_RUBY_REGEX_H
#  include "ruby/re.h"
#else
#  include "re.h"
#endif

// this sucks. but for some reason these moved around between 1.8 and 1.9
#ifdef ONIGURUMA_H
#define IGNORECASE ONIG_OPTION_IGNORECASE
#else
#define IGNORECASE RE_OPTION_IGNORECASE
#endif

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ibase.h>
#include <float.h>
#include <time.h>
#include <stdbool.h>

#if defined(HAVE_FB_CANCEL_OPERATION) && defined(HAVE_RB_THREAD_CALL_WITHOUT_GVL)
#  include <ruby/thread.h>
#endif

/* Firebird 4+ batch API (IBatch) ------------------------------------------
 * Permite ejecutar un array de tuplas con un solo round trip en lugar de un
 * isc_dsql_execute2 por registro. Solo se compila si extconf.rb la encuentra
 * (headers + simbolos en libfbclient); si no, fb.c usa el loop de siempre. */
#if defined(HAVE_FIREBIRD_FB_C_API_H) && defined(HAVE_FB_GET_MASTER_INTERFACE) && \
    defined(HAVE_FB_GET_STATEMENT_INTERFACE) && defined(HAVE_FB_GET_TRANSACTION_INTERFACE)
#  define FB_HAVE_BATCH_API 1
#  include <firebird/fb_c_api.h>
/* fb_c_api.h declara los tipos como "struct IXxx" (esta pensado para C++):
   estos typedefs hacen que los nombres cortos y sus macros sean C valido. */
typedef struct IMaster IMaster;
typedef struct IStatus IStatus;
typedef struct IUtil IUtil;
typedef struct IStatement IStatement;
typedef struct ITransaction ITransaction;
typedef struct IMessageMetadata IMessageMetadata;
typedef struct IBatch IBatch;
typedef struct IBatchCompletionState IBatchCompletionState;
typedef struct IXpbBuilder IXpbBuilder;
#endif

/* SQL type constants for Firebird 4.0/5.0+ not present in older ibase.h */
#ifndef SQL_TIMESTAMP_TZ
#define SQL_TIMESTAMP_TZ_EX              32748
#define SQL_TIME_TZ_EX                   32750
#define SQL_INT128                       32752
#define SQL_TIMESTAMP_TZ                 32754
#define SQL_TIME_TZ                      32756
#define SQL_DEC16                        32760
#define SQL_DEC34                        32762
#define SQL_BOOLEAN                      32764
#define SQL_NULL                         32766
#endif

#define	SQLDA_COLSINIT	50
#define	SQLCODE_NOMORE	100
#define	TPBBUFF_ALLOC	64
#define	CMND_DELIMIT	" \t\n\r\f"
#define	LIST_DELIMIT	", \t\n\r\f"
#define	META_NAME_MAX	31

/* Tamano maximo de un segmento de blob.
   isc_put_segment recibe la longitud en un unsigned short (65535 seria el
   tope de la biblioteca), pero los servidores Firebird 2.5 y anteriores solo
   manejan segmentos de 32765 bytes: 32765 es el mayor valor que funciona con
   cualquier version, y de todos modos son 8 veces menos segmentos (y menos
   isc_put_segment/isc_get_segment, cada uno con su ida y vuelta) que los 4096
   de antes. */
#define	FB_BLOB_SEGMENT_SIZE	32765

/* Statement type */
#define	STATEMENT_DDL	1
#define	STATEMENT_DML	0

/* Execute process flag */
#define	EXECF_EXECDML	0
#define	EXECF_SETPARM	1

static VALUE rb_mFb;
static VALUE rb_cFbDatabase;
static VALUE rb_cFbConnection;
static VALUE rb_cFbCursor;
static VALUE rb_cFbSqlType;
/* static VALUE rb_cFbGlobal; */
static VALUE rb_eFbError;
static VALUE rb_sFbField;
static VALUE rb_sFbIndex;
static VALUE rb_sFbColumn;
static VALUE rb_cDate;

static ID id_matches;
static ID id_downcase_bang;
static VALUE re_lowercase;
static ID id_rstrip_bang;
static ID id_sub_bang;
static ID id_force_encoding;

/* static char isc_info_stmt[] = { isc_info_sql_stmt_type }; */
/* static char isc_info_buff[16]; */
static char isc_tpb_0[] = {
    isc_tpb_version1,		isc_tpb_write,
    isc_tpb_concurrency,	isc_tpb_nowait
};

/* structs */

/* Entrada de cache de sentencias preparadas (ver fb_stmt_cache_*) */
struct FbStmtCache {
	char *sql;
	isc_stmt_handle stmt;
	XSQLDA *i_sqlda;
	XSQLDA *o_sqlda;
	long statement_type;
	int in_use;
	int invalid;
	VALUE owner;	/* cursor en cuyo campo stmt vive el handle (ver fb_stmt_cache_destroy) */
	struct FbStmtCache *next;
};

/* DB handle and TR parameter block list structure */
typedef struct
{
	isc_db_handle	*dbb_ptr ;
	long		 tpb_len ;
	char		*tpb_ptr ;
} ISC_TEB ; /* transaction existence block */

/* InterBase varchar structure */
typedef	struct
{
	short vary_length;
	char  vary_string[1];
} VARY;

struct FbConnection {
	isc_db_handle db;		/* DB handle */
	isc_tr_handle transact; /* transaction handle */
	VALUE cursor;
	VALUE cached_cursors; /* anclaje GC de cursores con sentencia cacheada */
	unsigned short dialect;
	unsigned short db_dialect;
	short downcase_names;
	VALUE encoding;
	long command_timeout_ms;	/* 0 = sin limite */
	struct FbStmtCache *stmt_cache;
	int dropped;
	ISC_STATUS isc_status[20];
	/* struct FbConnection *next; */
};

/* static struct FbConnection *fb_connection_list; */

struct FbCursor {
	int open;
	int eof;
	isc_tr_handle auto_transact;
	isc_stmt_handle stmt;
	XSQLDA *i_sqlda;
	XSQLDA *o_sqlda;
	char *i_buffer;
	long  i_buffer_size;
	char *o_buffer;
	long  o_buffer_size;
	VALUE fields_ary;
	VALUE fields_hash;
	VALUE connection;
	struct FbStmtCache *cache;	/* entrada de cache (o NULL si es dueno) */
	int exec_procedure;
	int fetched;
};

typedef struct trans_opts
{
	const char *option1;
	const char *option2;
	char  optval;
	short position;
	struct trans_opts *sub_opts;
} trans_opts;

/* global utilities */

#define	FB_ALIGN(n, b)	((n + b - 1) & ~(b - 1))
#define	UPPER(c)	(((c) >= 'a' && (c)<= 'z') ? (c) - 'a' + 'A' : (c))
#define	FREE(p)		if (p)	{ xfree(p); p = 0; }
#define	SETNULL(p)	if (p && strlen(p) == 0)	{ p = 0; }
 // #define HERE(s) printf("%s\n", s)
#define HERE(s)

static long calculate_buffsize(XSQLDA *sqlda)
{
	XSQLVAR *var;
	long cols;
	short dtp;
	long offset = 0;
	long alignment;
	long length;
	long count;

	cols = sqlda->sqld;
	var = sqlda->sqlvar;
	for (count = 0; count < cols; var++,count++) {
		length = alignment = var->sqllen;
		dtp = var->sqltype & ~1;

		if (dtp == SQL_TEXT) {
			alignment = 1;
		} else if (dtp == SQL_VARYING) {
			length += sizeof(short);
			alignment = sizeof(short);
		}

		offset = FB_ALIGN(offset, alignment);
		offset += length;
		offset = FB_ALIGN(offset, sizeof(short));
		offset += sizeof(short);
	}

	return offset + sizeof(short);
}

#if (FB_API_VER >= 20)
static VALUE fb_error_msg(const ISC_STATUS *isc_status)
{
	char msg[1024];
	VALUE result = rb_str_new(NULL, 0);
	while (fb_interpret(msg, 1024, &isc_status))
	{
		result = rb_str_cat(result, msg, strlen(msg));
		result = rb_str_cat(result, "\n", strlen("\n"));
	}
	return result;
}
#else
static VALUE fb_error_msg(ISC_STATUS *isc_status)
{
	char msg[1024];
	VALUE result = rb_str_new(NULL, 0);
	while (isc_interprete(msg, &isc_status))
	{
		result = rb_str_cat(result, msg, strlen(msg));
		result = rb_str_cat(result, "\n", strlen("\n"));
	}
	return result;
}
#endif

struct time_object {
	struct timeval tv;
	struct tm tm;
	int gmt;
	int tm_got;
};

#define GetTimeval(obj, tobj) \
    Data_Get_Struct(obj, struct time_object, tobj)

static VALUE fb_mktime(struct tm *tm, const char *which)
{
#if defined(_WIN32)
	if (tm->tm_year + 1900 < 1970)
	{
		tm->tm_year = 70;
		tm->tm_mon = 0;
		tm->tm_mday = 1;
		tm->tm_hour = 0;
		tm->tm_min = 0;
		tm->tm_sec = 0;
	}
#endif
#if defined(_LP64) || defined(__LP64__) || defined(__arch64__)
// No need to floor time on 64-bit Unix.
#else
	if (tm->tm_year + 1900 < 1902)
	{
		tm->tm_year = 2;
		tm->tm_mon = 0;
		tm->tm_mday = 1;
		tm->tm_hour = 0;
		tm->tm_min = 0;
		tm->tm_sec = 0;
	}
#endif
	return rb_funcall(
		rb_cTime, rb_intern(which), 6,
		INT2FIX(tm->tm_year + 1900), INT2FIX(tm->tm_mon + 1), INT2FIX(tm->tm_mday),
		INT2FIX(tm->tm_hour), INT2FIX(tm->tm_min), INT2FIX(tm->tm_sec));
}

static VALUE fb_mkdate(struct tm *tm)
{
	return rb_funcall(
		rb_cDate, rb_intern("civil"), 3,
		INT2FIX(1900 + tm->tm_year), INT2FIX(tm->tm_mon + 1), INT2FIX(tm->tm_mday));
}

static int responds_like_date(VALUE obj)
{
	return rb_respond_to(obj, rb_intern("year")) &&
		rb_respond_to(obj, rb_intern("month")) &&
		rb_respond_to(obj, rb_intern("day"));
}
static void tm_from_date(struct tm *tm, VALUE date)
{
	VALUE year, month, day;

	if (!responds_like_date(date)) {
		VALUE s = rb_funcall(date, rb_intern("to_s"), 0);
		date = rb_funcall(rb_cDate, rb_intern("parse"), 1, s);
	}
	year = rb_funcall(date, rb_intern("year"), 0);
	month = rb_funcall(date, rb_intern("month"), 0);
	day = rb_funcall(date, rb_intern("day"), 0);
	memset(tm, 0, sizeof(struct tm));
	tm->tm_year = FIX2INT(year) - 1900;
	tm->tm_mon = FIX2INT(month) - 1;
	tm->tm_mday = FIX2INT(day);
}

static void tm_from_timestamp(struct tm *tm, VALUE obj)
{
#ifdef TypedData_Get_Struct
	VALUE year, month, day, hour, min, sec;
#else
	struct time_object *tobj;
#endif

	if (!rb_obj_is_kind_of(obj, rb_cTime))
	{
		if (rb_respond_to(obj, rb_intern("to_str")))
		{
			VALUE s = rb_funcall(obj, rb_intern("to_str"), 0);
			obj = rb_funcall(rb_cTime, rb_intern("parse"), 1, s);
		}
		else
		{
			rb_raise(rb_eTypeError, "Expecting Time object or string.");
		}
	}

#ifdef TypedData_Get_Struct
	year = rb_funcall(obj, rb_intern("year"), 0);
	month = rb_funcall(obj, rb_intern("month"), 0);
	day = rb_funcall(obj, rb_intern("day"), 0);
	hour = rb_funcall(obj, rb_intern("hour"), 0);
	min = rb_funcall(obj, rb_intern("min"), 0);
	sec = rb_funcall(obj, rb_intern("sec"), 0);
	memset(tm, 0, sizeof(struct tm));
	tm->tm_year = FIX2INT(year) - 1900;
	tm->tm_mon = FIX2INT(month) - 1;
	tm->tm_mday = FIX2INT(day);
	tm->tm_hour = FIX2INT(hour);
	tm->tm_min = FIX2INT(min);
	tm->tm_sec = FIX2INT(sec);
#else
	GetTimeval(obj, tobj);
	*tm = tobj->tm;
#endif
}

static VALUE object_to_fixnum(VALUE object)
{
	if (TYPE(object) != T_FIXNUM && TYPE(object) != T_BIGNUM)
	{
		if (TYPE(object) == T_FLOAT || !strcmp(rb_class2name(CLASS_OF(object)), "BigDecimal"))
			object = rb_funcall(object, rb_intern("round"), 0);
		else if (TYPE(object) == T_STRING)
			object = rb_funcall(rb_funcall(rb_mKernel, rb_intern("BigDecimal"), 1, object), rb_intern("round"), 0);
		else if (!strcmp(rb_class2name(CLASS_OF(object)), "Time"))
			rb_raise(rb_eTypeError, "Time value not allowed as Integer");
		else if (rb_respond_to(object, rb_intern("to_i")))
			object = rb_funcall(object, rb_intern("to_i"), 0);
		else
			rb_raise(rb_eTypeError, "Value doesn't respond to 'to_i' for conversion");
	}
	return (object);
}

static VALUE double_from_obj(VALUE obj)
{
	if (TYPE(obj) == T_STRING)
	{
		obj = rb_funcall(obj, rb_intern("to_f"), 0);
	}
	return obj;
}

/* INT128 (Firebird 4+): se maneja como Integer de Ruby (bignum) almacenado en
   16 bytes little-endian en complemento a dos. Sin esto, los valores fuera del
   rango de int64 daban RangeError al escribir y se truncaban al leer. */
static VALUE fb_int128_modulus;	/* 2**128 */
static VALUE fb_int128_lomask;	/* 2**64 - 1 */

static void fb_int128_write(char *dest, VALUE v)
{
	VALUE mod = rb_funcall(v, rb_intern("%"), 1, fb_int128_modulus);	/* complemento a dos */
	VALUE lo = rb_funcall(mod, rb_intern("&"), 1, fb_int128_lomask);
	VALUE hi = rb_funcall(mod, rb_intern(">>"), 1, INT2FIX(64));
	unsigned long long lo64 = NUM2ULL(lo);
	unsigned long long hi64 = NUM2ULL(hi);

	memcpy(dest, &lo64, sizeof(lo64));
	memcpy(dest + sizeof(lo64), &hi64, sizeof(hi64));
}

static VALUE fb_int128_read(const char *src)
{
	unsigned long long lo64, hi64;
	VALUE v;

	memcpy(&lo64, src, sizeof(lo64));
	memcpy(&hi64, src + sizeof(lo64), sizeof(hi64));

	v = rb_funcall(ULL2NUM(hi64), rb_intern("<<"), 1, INT2FIX(64));
	v = rb_funcall(v, rb_intern("+"), 1, ULL2NUM(lo64));
	if (hi64 & 0x8000000000000000ULL)	/* bit de signo: negativo */
		v = rb_funcall(v, rb_intern("-"), 1, fb_int128_modulus);
	return v;
}

static VALUE fb_sql_type_from_code(int code, int subtype)
{
	const char *sql_type = NULL;
	switch(code) {
		case SQL_TEXT:
		case blr_text:
			sql_type = "CHAR";
			break;
		case SQL_VARYING:
		case blr_varying:
			sql_type = "VARCHAR";
			break;
		case SQL_SHORT:
		case blr_short:
			switch (subtype) {
				case 0:		sql_type = "SMALLINT";	break;
				case 1:		sql_type = "NUMERIC";	break;
				case 2:		sql_type = "DECIMAL";	break;
			}
			break;
		case SQL_LONG:
		case blr_long:
			switch (subtype) {
				case 0:		sql_type = "INTEGER";	break;
				case 1:		sql_type = "NUMERIC";	break;
				case 2:		sql_type = "DECIMAL";	break;
			}
			break;
		case SQL_FLOAT:
		case blr_float:
			sql_type = "FLOAT";
			break;
		case SQL_DOUBLE:
		case blr_double:
			switch (subtype) {
				case 0:		sql_type = "DOUBLE PRECISION"; break;
				case 1:		sql_type = "NUMERIC";	break;
				case 2:		sql_type = "DECIMAL";	break;
			}
			break;
		case SQL_D_FLOAT:
		case blr_d_float:
			sql_type = "DOUBLE PRECISION";
			break;
		case SQL_TIMESTAMP:
		case blr_timestamp:
			sql_type = "TIMESTAMP";
			break;
		case SQL_BLOB:
		case blr_blob:
			sql_type = "BLOB";
			break;
		case SQL_ARRAY:
			sql_type = "ARRAY";
			break;
#if (FB_API_VER >= 30)
		case SQL_BOOLEAN:
		case blr_boolean:
			sql_type = "BOOLEAN";
			break;
#endif
		case SQL_QUAD:
		case blr_quad:
			sql_type = "DECIMAL";
			break;
		case SQL_TIMESTAMP_TZ:
		case SQL_TIMESTAMP_TZ_EX:
			sql_type = "TIMESTAMP WITH TIME ZONE";
			break;
		case SQL_TIME_TZ:
		case SQL_TIME_TZ_EX:
			sql_type = "TIME WITH TIME ZONE";
			break;
		case SQL_DEC16:
			sql_type = "DECFLOAT(16)";
			break;
		case SQL_DEC34:
			sql_type = "DECFLOAT(34)";
			break;
		case SQL_TYPE_TIME:
		case blr_sql_time:
			sql_type = "TIME";
			break;
		case SQL_TYPE_DATE:
		case blr_sql_date:
			sql_type = "DATE";
			break;
		case SQL_INT64:
		case blr_int64:
			switch (subtype) {
				case 0:		sql_type = "BIGINT";	break;
				case 1:		sql_type = "NUMERIC";	break;
				case 2:		sql_type = "DECIMAL";	break;
			}
			break;
#if (FB_API_VER >= 40)
		case SQL_INT128:
			sql_type = "INT128";
			break;
#endif
		default:
			printf("Unknown: %d, %d\n", code, subtype);
			sql_type = "UNKNOWN";
			break;
	}
	return rb_str_new2(sql_type);
}

/* call-seq:
 *   from_code(code, subtype) -> String
 *
 * Returns the SQL type, such as VARCHAR or INTEGER for a given type code and subtype.
 */
static VALUE sql_type_from_code(VALUE self, VALUE code, VALUE subtype)
{
	return fb_sql_type_from_code(NUM2INT(code), NUM2INT(subtype));
}

static void fb_error_check(ISC_STATUS *isc_status)
{
	if (isc_status[0] == 1 && isc_status[1]) {
		char buf[1024];
		VALUE exc, msg, msg1, msg2;
		short code = isc_sqlcode(isc_status);

		isc_sql_interprete(code, buf, 1024);
		msg1 = rb_str_new2(buf);
		msg2 = fb_error_msg(isc_status);
		msg = rb_str_cat(msg1, "\n", strlen("\n"));
		msg = rb_str_concat(msg, msg2);

		exc = rb_exc_new3(rb_eFbError, msg);
		rb_iv_set(exc, "error_code", INT2FIX(code));
		rb_exc_raise(exc);
	}
}

static void fb_error_check_warn(ISC_STATUS *isc_status)
{
	short code = isc_sqlcode(isc_status);
	if (code != 0) {
		char buf[1024];
		isc_sql_interprete(code, buf, 1024);
		rb_warning("%s(%d)", buf, code);
	}
}

static void fb_stmt_cache_invalidate(struct FbConnection *fb_connection);

/* Operaciones que pueden bloquear (execute/fetch) se ejecutan sin el GVL para
   que otro hilo de Ruby pueda llamar a Connection#cancel (o Ctrl-C interrumpa):
   en ese caso la "unblock function" pide la cancelacion a Firebird para que la
   llamada retorne. */
#ifdef HAVE_FB_CANCEL_OPERATION
struct fb_blocking_op {
	ISC_STATUS *status;
	isc_db_handle *db;
	isc_tr_handle *tr;
	isc_stmt_handle *stmt;
	XSQLDA *in;
	XSQLDA *out;
	int fetch;
	int rc;
};

static void *fb_blocking_run(void *arg)
{
	struct fb_blocking_op *op = (struct fb_blocking_op *) arg;
	if (op->fetch)
		op->rc = (int) isc_dsql_fetch(op->status, op->stmt, 1, op->out);
	else
		op->rc = (int) isc_dsql_execute2(op->status, op->tr, op->stmt, SQLDA_VERSION1, op->in, op->out);
	return NULL;
}

static void fb_blocking_cancel(void *arg)
{
	struct fb_blocking_op *op = (struct fb_blocking_op *) arg;
	ISC_STATUS status[20];
	memset(status, 0, sizeof(status));
	fb_cancel_operation(status, op->db, fb_cancel_raise);
}

static int fb_blocking_call(struct FbConnection *fb_connection, isc_stmt_handle *stmt,
							XSQLDA *in, XSQLDA *out, int fetch)
{
	struct fb_blocking_op op;

	op.status = fb_connection->isc_status;
	op.db = &fb_connection->db;
	op.tr = &fb_connection->transact;
	op.stmt = stmt;
	op.in = in;
	op.out = out;
	op.fetch = fetch;
	op.rc = 0;
#ifdef HAVE_RB_THREAD_CALL_WITHOUT_GVL
	rb_thread_call_without_gvl(fb_blocking_run, &op, fb_blocking_cancel, &op);
#else
	fb_blocking_run(&op);
#endif
	return op.rc;
}
#endif /* HAVE_FB_CANCEL_OPERATION */

static int fb_stmt_execute2(struct FbConnection *fb_connection, isc_stmt_handle *stmt,
							XSQLDA *in, XSQLDA *out)
{
	int rc;
#ifdef HAVE_FB_CANCEL_OPERATION
	rc = fb_blocking_call(fb_connection, stmt, in, out, 0);
#else
	rc = (int) isc_dsql_execute2(fb_connection->isc_status, &fb_connection->transact, stmt,
								 SQLDA_VERSION1, in, out);
#endif
	/* un error puede ser "sentencia invalidada": mejor tirar la cache */
	if (fb_connection->isc_status[0] == 1 && fb_connection->isc_status[1])
		fb_stmt_cache_invalidate(fb_connection);
	return rc;
}

static int fb_stmt_fetch(struct FbConnection *fb_connection, isc_stmt_handle *stmt, XSQLDA *out)
{
#ifdef HAVE_FB_CANCEL_OPERATION
	return fb_blocking_call(fb_connection, stmt, NULL, out, 1);
#else
	return (int) isc_dsql_fetch(fb_connection->isc_status, stmt, 1, out);
#endif
}

/* ==========================================================================
 * Cache de sentencias preparadas por conexion
 *
 * Cada execute hace alloc_statement + prepare + describe_bind + describe, tres
 * idas y vueltas al servidor que se pueden ahorrar cuando el mismo SQL se
 * repite. La entrada guarda la sentencia ya preparada y sus descripciones
 * (SQLDA); el cursor la toma prestada y la devuelve al cerrarse.
 *
 * Propiedad: un cursor con cache == NULL es dueno de su sentencia y la destruye
 * como siempre; uno con entrada solo cierra el cursor y marca la entrada libre.
 * ========================================================================== */

/* Cache de sentencias ACTIVA (0.10.7).
 *
 * 0.10.6 la desactivo (segfault: la cache compartia sentencia y SQLDA entre el
 * cursor y la entrada, y el GC podia finalizar un cursor mientras la sentencia
 * seguia cacheada). 0.10.7 la reactiva con el anclaje de cursores: todo cursor
 * cuya sentencia esta en la cache se mantiene vivo (rb_gc_mark de
 * cached_cursors) hasta que la devuelve, asi la cache puede dropear sus
 * handles con seguridad. Ver PENDIENTES-gema-fb.md.
 *
 * 0.10.8 corrige el SQLDA de salida con mas de SQLDA_COLSINIT (50) columnas: se
 * reasignaba DESPUES de registrar la entrada en la cache, asi que la entrada
 * quedaba apuntando al SQLDA de 50 liberado. La segunda ejecucion del mismo SQL
 * leia memoria liberada (query devolvia 0 en lugar de las filas) y acababa en
 * doble free / SIGSEGV. Ahora la reasignacion se hace antes de registrar.
 *
 * 0.10.9 ancla al cursor DUENO de la entrada mientras su sentencia este en la
 * cache (FbStmtCache.owner). Firebird se guarda la direccion del campo stmt que
 * se le da al preparar y escribe NULL ahi al hacer DSQL_drop; si el GC ya habia
 * liberado ese cursor (y Ruby reutilizaba la misma direccion para otro), el
 * handle del cursor nuevo quedaba a cero y el siguiente DDL fallaba con
 * "invalid statement handle". Ver PENDIENTES-gema-fb.md, punto 3.
 */
#define FB_STMT_CACHE_MAX	64

/* Mantiene vivo (marcado desde fb_connection_mark) el cursor cuya sentencia esta
   en la cache: la entrada guarda el handle y los SQLDA que salieron del cursor,
   y si el GC lo finalizaba mientras la sentencia seguia cacheada, Firebird
   acababa escribiendo sobre esa memoria ya liberada (valgrind: invalid write de
   isc_dsql_free_statement sobre un FbCursor liberado). */
static void fb_cursor_pin(struct FbConnection *fb_connection, VALUE cursor)
{
	int i;
	if (NIL_P(fb_connection->cached_cursors)) {
		fb_connection->cached_cursors = rb_ary_new();
	} else {
		long i;
		for (i = 0; i < RARRAY_LEN(fb_connection->cached_cursors); i++) {
			if (RARRAY_PTR(fb_connection->cached_cursors)[i] == cursor)
				return;
		}
	}
	rb_ary_push(fb_connection->cached_cursors, cursor);
}

static void fb_cursor_unpin(struct FbConnection *fb_connection, VALUE cursor)
{
	/* Borrar de verdad (no dejar Qnil): connection_close/drop iteran este array
	   y un hueco nil les hacia llamar cursor_close(nil). */
	if (!NIL_P(fb_connection->cached_cursors))
		rb_ary_delete(fb_connection->cached_cursors, cursor);
}

static struct FbStmtCache *fb_stmt_cache_find(struct FbConnection *fb_connection, const char *sql)
{
	struct FbStmtCache *e;

	for (e = fb_connection->stmt_cache; e; e = e->next) {
		if (!e->in_use && !e->invalid && strcmp(e->sql, sql) == 0)
			return e;
	}
	return NULL;
}

static void fb_stmt_cache_destroy(struct FbConnection *fb_connection, struct FbStmtCache **link)
{
	struct FbStmtCache *e = *link;

	*link = e->next;
	if (fb_connection->db) {	/* tras desconectar, la sentencia ya no existe */
		/* OJO: status LOCAL. Usar el de la conexion pisaba el error que se esta
		   procesando (p.ej. el del timeout) y la excepcion se perdia. */
		ISC_STATUS status[20];
		memset(status, 0, sizeof(status));
		isc_dsql_free_statement(status, &e->stmt, DSQL_drop);
		fb_error_check_warn(status);
	}
	/* El cursor dueno esta anclado (fb_cursor_pin) mientras su sentencia viva en
	   la cache: Firebird se queda con la DIRECCION del campo stmt que le dimos en
	   isc_dsql_alloc_statement2/prepare, y al hacer DSQL_drop escribe NULL ahi
	   ademas de en la variable que le pasamos. Si el GC ya hubiera liberado ese
	   cursor y la memoria se hubiera reciclado para otro (es lo que hace Ruby: la
	   misma direccion se reusa), ese NULL caia en el handle del cursor nuevo ->
	   "invalid statement handle" al ejecutarlo (se ve con un DDL despues de
	   consultas cacheadas). Por eso el dueno no se despina hasta este punto. */
	if (!NIL_P(e->owner)) {
		fb_cursor_unpin(fb_connection, e->owner);
		e->owner = Qnil;
	}
	xfree(e->i_sqlda);
	xfree(e->o_sqlda);
	xfree(e->sql);
	xfree(e);
}

static struct FbStmtCache *fb_stmt_cache_add(struct FbConnection *fb_connection, struct FbCursor *fb_cursor,
											 const char *sql, long statement_type, VALUE cursor)
{
	struct FbStmtCache *e;
	struct FbStmtCache **p;
	int count = 0;

	for (e = fb_connection->stmt_cache; e; e = e->next)
		count++;

	if (count >= FB_STMT_CACHE_MAX) {
		struct FbStmtCache **free_link = NULL;
		for (p = &fb_connection->stmt_cache; *p; p = &(*p)->next)
			if (!(*p)->in_use) { free_link = p; break; }
		if (!free_link)
			return NULL;	/* todas en uso: este cursor sera dueno de su sentencia */
		fb_stmt_cache_destroy(fb_connection, free_link);
	}

	e = (struct FbStmtCache *) xmalloc(sizeof(struct FbStmtCache));
	e->sql = (char *) xmalloc(strlen(sql) + 1);
	strcpy(e->sql, sql);
	e->stmt = fb_cursor->stmt;
	e->i_sqlda = fb_cursor->i_sqlda;
	e->o_sqlda = fb_cursor->o_sqlda;
	e->statement_type = statement_type;
	e->in_use = 1;
	e->invalid = 0;
	e->owner = cursor;
	e->next = fb_connection->stmt_cache;
	fb_connection->stmt_cache = e;
	return e;
}

/* Invalida la cache (cambio de esquema o error de ejecucion): las entradas
   libres se destruyen ya, las que estan en uso se marcan para destruirse
   cuando el cursor las libere (no se pueden tocar ahora). */
static void fb_stmt_cache_invalidate(struct FbConnection *fb_connection)
{
	struct FbStmtCache **p = &fb_connection->stmt_cache;

	while (*p) {
		if ((*p)->in_use) {
			(*p)->invalid = 1;
			p = &(*p)->next;
		} else {
			fb_stmt_cache_destroy(fb_connection, p);	/* avanza *p */
		}
	}
}

/* El cursor devuelve la entrada prestada. */
static void fb_stmt_cache_release(struct FbConnection *fb_connection, struct FbCursor *fb_cursor)
{
	struct FbStmtCache *e = fb_cursor->cache;
	struct FbStmtCache **p;

	fb_cursor->cache = NULL;
	fb_cursor->stmt = 0;
	fb_cursor->i_sqlda = NULL;
	fb_cursor->o_sqlda = NULL;
	if (!e)
		return;

	if (!e->invalid) {
		e->in_use = 0;
		return;
	}

	for (p = &fb_connection->stmt_cache; *p; p = &(*p)->next) {
		if (*p == e) {
			fb_stmt_cache_destroy(fb_connection, p);
			return;
		}
	}
}

/* Solo libera memoria: se llama cuando la conexion ya se desconecto (las
   sentencias se van con el detach). */
static void fb_stmt_cache_free(struct FbConnection *fb_connection)
{
	struct FbStmtCache *e = fb_connection->stmt_cache, *next;

	while (e) {
		next = e->next;
		xfree(e->i_sqlda);
		xfree(e->o_sqlda);
		xfree(e->sql);
		xfree(e);
		e = next;
	}
	fb_connection->stmt_cache = NULL;
}

static XSQLDA* sqlda_alloc(long cols)
{
	XSQLDA *sqlda;

	sqlda = (XSQLDA*)xmalloc(XSQLDA_LENGTH(cols));
#ifdef SQLDA_CURRENT_VERSION
	sqlda->version = SQLDA_CURRENT_VERSION;
#else
	sqlda->version = SQLDA_VERSION1;
#endif
	sqlda->sqln = cols;
	sqlda->sqld = 0;
	return sqlda;
}

static VALUE cursor_close _((VALUE));
static VALUE cursor_drop _((VALUE));
static VALUE cursor_execute _((int, VALUE*, VALUE));
static VALUE cursor_fetchall _((int, VALUE*, VALUE));

static void fb_cursor_mark(struct FbCursor *fb_cursor);
static void fb_cursor_free(struct FbCursor *fb_cursor);
static void fb_connection_mark(struct FbConnection *fb_connection);
static void fb_connection_free(struct FbConnection *fb_connection);

/* ruby data types */

static const rb_data_type_t fbdatabase_data_type = {
    "FBDB",
    {
        NULL,
        NULL,
        NULL,
        NULL,
    },
    0, 0, 0
};

static const rb_data_type_t fbconnection_data_type = {
    "fbdb/connection",
    {
        (void (*)(void *))fb_connection_mark,
        (void (*)(void *))fb_connection_free,
        NULL,
    },
    0, 0, 0
};

static const rb_data_type_t fbcursor_data_type = {
    "fbdb/cursor",
    {
        (void (*)(void *))fb_cursor_mark,
        (void (*)(void *))fb_cursor_free,
        NULL,
    },
    0, 0, 0
};

/* connection utilities */
static void fb_connection_check(struct FbConnection *fb_connection)
{
	if (fb_connection->db == 0) {
		rb_raise(rb_eFbError, "closed db connection");
	}
}

static void fb_connection_close_cursors(struct FbConnection *fb_connection)
{
	int i;
	long len = RARRAY_LEN(fb_connection->cursor);
	for (i = 0; i < len; i++) {
		cursor_close(RARRAY_PTR(fb_connection->cursor)[i]);
	}
}

static void fb_connection_drop_cursors(struct FbConnection *fb_connection)
{
	int i;
	long len = RARRAY_LEN(fb_connection->cursor);
	for (i = 0; i < len; i++) {
		cursor_drop(RARRAY_PTR(fb_connection->cursor)[i]);
	}
  rb_ary_clear(fb_connection->cursor);
}

static void fb_connection_disconnect(struct FbConnection *fb_connection)
{
	if (fb_connection->transact) {
		isc_commit_transaction(fb_connection->isc_status, &fb_connection->transact);
		fb_error_check(fb_connection->isc_status);
	}
	if (fb_connection->dropped) {
		isc_drop_database(fb_connection->isc_status, &fb_connection->db);
	} else {
		isc_detach_database(fb_connection->isc_status, &fb_connection->db);
	}
	fb_error_check(fb_connection->isc_status);
}

static void fb_connection_disconnect_warn(struct FbConnection *fb_connection)
{
	if (fb_connection->transact) {
		isc_commit_transaction(fb_connection->isc_status, &fb_connection->transact);
		fb_error_check_warn(fb_connection->isc_status);
	}
	isc_detach_database(fb_connection->isc_status, &fb_connection->db);
	fb_error_check_warn(fb_connection->isc_status);
}

static void fb_connection_mark(struct FbConnection *fb_connection)
{
	rb_gc_mark(fb_connection->cursor);
	rb_gc_mark(fb_connection->cached_cursors);
	/* encoding es un VALUE guardado en la struct: si no se marca, el GC puede
	   liberarlo y force_encoding() acabaria usandolo (crash o "unknown
	   encoding name - ..."). */
	rb_gc_mark(fb_connection->encoding);
}

static void fb_connection_free(struct FbConnection *fb_connection)
{
	if (fb_connection->db) {
		fb_connection_disconnect_warn(fb_connection);
	}
	fb_stmt_cache_free(fb_connection);
	xfree(fb_connection);
}

/*
static struct FbConnection* fb_connection_check_retrieve(VALUE data)
{
	if (TYPE(data) != T_DATA || RDATA(data)->dfree != (void *)fb_connection_free) {
		rb_raise(rb_eTypeError,
			"Wrong argument type %s (expected Fb::Connection)",
			rb_class2name(CLASS_OF(data)));
	}
	return (struct FbConnection*)RDATA(data)->data;
}
*/

static unsigned short fb_connection_db_SQL_Dialect(struct FbConnection *fb_connection)
{
	long dialect;
	long length;
	char db_info_command = isc_info_db_sql_dialect;
	char isc_info_buff[16];

	/* Get the db SQL Dialect */
	isc_database_info(fb_connection->isc_status, &fb_connection->db,
			1, &db_info_command,
			sizeof(isc_info_buff), isc_info_buff);
	fb_error_check(fb_connection->isc_status);

	if (isc_info_buff[0] == isc_info_db_sql_dialect) {
		length = isc_vax_integer(&isc_info_buff[1], 2);
		dialect = isc_vax_integer(&isc_info_buff[3], (short)length);
	} else {
		dialect = 1;
	}
	return dialect;
}

static unsigned short fb_connection_dialect(struct FbConnection *fb_connection)
{
	return fb_connection->dialect;
}

/*
static unsigned short fb_connection_db_dialect(struct FbConnection *fb_connection)
{
	return fb_connection->db_dialect;
}
*/

/* Transaction option list */

static trans_opts	rcom_opt_S[] =
{
	{"NO",			"RECORD_VERSION",	isc_tpb_no_rec_version,	-1,	0},
	{"RECORD_VERSION",	0,			isc_tpb_rec_version,	-1,	0},
	{"*",			0,			isc_tpb_no_rec_version,	-1,	0},
	{0,			0,			0,			0,	0}
};


static trans_opts	read_opt_S[] =
{
	{"WRITE",	0,	isc_tpb_write,		1,	0},
	{"ONLY",		0,	isc_tpb_read,		1,	0},
	{"COMMITTED",	0,	isc_tpb_read_committed,	2,	rcom_opt_S},
	{0,		0,	0,			0,	0}
};


static trans_opts	snap_opt_S[] =
{
	{"TABLE",	"STABILITY",	isc_tpb_consistency,	2,	0},
	{"*",		0,		isc_tpb_concurrency,	2,	0},
	{0,			0,	0,			0,	0}
};


static trans_opts	isol_opt_S[] =
{
	{"SNAPSHOT",	0,		0,			0,	snap_opt_S},
	{"READ",		"COMMITTED",	isc_tpb_read_committed,	2,	rcom_opt_S},
	{0,		0,		0,			0,	0}
};


static trans_opts	trans_opt_S[] =
{
	{"READ",		0,		0,		0,	read_opt_S},
	{"WAIT",		0,		isc_tpb_wait,	3,	0},
	{"NO",		"WAIT",		isc_tpb_nowait,	3,	0},
	{"ISOLATION",	"LEVEL",	0,		0,	isol_opt_S},
	{"SNAPSHOT",	0,		0,		0,	snap_opt_S},
	{"RESERVING",	0,		-1,		0,	0},
	{0,		0,		0,		0,	0}
};

/* Name1	Name2		Option value	    Position	Sub-option */

#define	RESV_TABLEEND	"FOR"
#define	RESV_SHARED	"SHARED"
#define	RESV_PROTECTD	"PROTECTED"
#define	RESV_READ	"READ"
#define	RESV_WRITE	"WRITE"
#define	RESV_CONTINUE	','

static char* trans_parseopts(VALUE opt, long *tpb_len)
{
	char *s, *trans;
	long used;
	long size;
	char *tpb;
	trans_opts *curr_p;
	trans_opts *target_p;
	char *check1_p;
	char *check2_p;
	int count;
	int next_c;
	char check_f[4];
	char *resv_p;
	char *resend_p;
	char *tblend_p = 0;
	long tbl_len;
	long res_first;
	int res_count;
	long ofs;
	char sp_prm;
	char rw_prm;
	int cont_f;
	const char *desc = 0;

	/* Initialize */
	s = StringValuePtr(opt);
	trans = ALLOCA_N(char, strlen(s)+1);
	strcpy(trans, s);
	s = trans;
	while (*s) {
		*s = UPPER(*s);
		s++;
	}

	used = 0;
	size = 0;
	tpb = NULL;
	memset((void *)check_f, 0, sizeof(check_f));

	/* Set the default transaction option */
	tpb = (char*)xmalloc(TPBBUFF_ALLOC);
	size = TPBBUFF_ALLOC;
	memcpy((void*)tpb, (void*)isc_tpb_0, sizeof(isc_tpb_0));
	used = sizeof(isc_tpb_0);

	/* Analize the transaction option strings */
	curr_p = trans_opt_S;
	check1_p = strtok(trans, CMND_DELIMIT);
	if (check1_p) {
		check2_p = strtok(0, CMND_DELIMIT);
	} else {
		check2_p = 0;
	}
	while (curr_p) {
		target_p = 0;
		next_c = 0;
		for (count = 0; curr_p[count].option1; count++) {
			if (!strcmp(curr_p[count].option1, "*")) {
				target_p = &curr_p[count];
				break;
			} else if (check1_p && !strcmp(check1_p, curr_p[count].option1)) {
				if (!curr_p[count].option2) {
					next_c = 1;
					target_p = &curr_p[count];
					break;
				} else if (check2_p && !strcmp(check2_p, curr_p[count].option2)) {
					next_c = 2;
					target_p = &curr_p[count];
					break;
				}
			}
		}

		if (!target_p) {
			desc = "Illegal transaction option was specified";
			goto error;
		}

		/* Set the transaction option */
		if (target_p->optval > '\0') {
			if (target_p->position > 0) {
				if (check_f[target_p->position]) {
					desc = "Duplicate transaction option was specified";
					goto error;
				}
				tpb[target_p->position] = target_p->optval;
				check_f[target_p->position] = 1;
			} else {
				if (used + 1 > size) {
					tpb = (char *)realloc(tpb, size + TPBBUFF_ALLOC);
					size += TPBBUFF_ALLOC;
				}
				tpb[used] = target_p->optval;
				used++;
			}
		} else if (target_p->optval) {		/* RESERVING ... FOR */
			if (check_f[0]) {
				desc = "Duplicate transaction option was specified";
				goto error;
			}
			resv_p = check2_p;
			if (!resv_p || !strcmp(resv_p, RESV_TABLEEND)) {
				desc = "RESERVING needs table name list";
				goto error;
			}
			while (resv_p) {
				res_first = used;
				res_count = 0;
				resend_p = strtok(0, CMND_DELIMIT);
				while (resend_p) {
					if (!strcmp(resend_p, RESV_TABLEEND)) {
						break;
					}
					resend_p = strtok(0, CMND_DELIMIT);
				}

				if (!resend_p) {
					desc = "Illegal transaction option was specified";
					goto error;
				}

				while (resv_p < resend_p) {
					if (*resv_p == '\0' || (ofs = strspn(resv_p, LIST_DELIMIT)) < 0) {
						resv_p++;
					} else {
						resv_p = &resv_p[ofs];
						tblend_p = strpbrk(resv_p, LIST_DELIMIT);
						if (tblend_p) {
							tbl_len = tblend_p - resv_p;
						} else {
							tbl_len = strlen(resv_p);
						}
						if (tbl_len > META_NAME_MAX) {
							desc = "Illegal table name was specified";
							goto error;
						}

						if (tbl_len > 0) {
							if (used + tbl_len + 3 > size) {
								tpb = (char*)xrealloc(tpb, size+TPBBUFF_ALLOC);
								size += TPBBUFF_ALLOC;
							}
							tpb[used+1] = (char)tbl_len;
							memcpy((void *)&tpb[used+2],resv_p, tbl_len);
							used += tbl_len + 3;
							res_count++;
						}
						resv_p += tbl_len;
					}
				}

				resv_p = strtok(0, CMND_DELIMIT);
				if (resv_p && !strcmp(resv_p, RESV_SHARED)) {
					sp_prm = isc_tpb_shared;
				} else if (resv_p && !strcmp(resv_p, RESV_PROTECTD)) {
					sp_prm = isc_tpb_protected;
				} else {
					desc = "RESERVING needs {SHARED|PROTECTED} {READ|WRITE}";
					goto error;
				}

				cont_f = 0;
				resv_p = strtok(0, CMND_DELIMIT);
				if (resv_p) {
					if (resv_p[strlen(resv_p)-1] == RESV_CONTINUE) {
						cont_f = 1;
						resv_p[strlen(resv_p)-1] = '\0';
					} else {
						tblend_p = strpbrk(resv_p, LIST_DELIMIT);
						if (tblend_p) {
							cont_f = 2;
							*tblend_p = '\0';
						}
					}
				}

				if (resv_p && !strcmp(resv_p, RESV_READ)) {
					rw_prm = isc_tpb_lock_read;
				} else if (resv_p && !strcmp(resv_p, RESV_WRITE)) {
					rw_prm = isc_tpb_lock_write;
				} else {
					desc = "RESERVING needs {SHARED|PROTECTED} {READ|WRITE}";
					goto error;
				}

				ofs = res_first;
				for (count = 0; count < res_count; count++) {
					tpb[ofs++] = rw_prm;
					ofs += tpb[ofs] + 1;
					tpb[ofs++] = sp_prm;
				}

				if (cont_f == 1) {
					resv_p = strtok(0, CMND_DELIMIT);
					if (!resv_p) {
						desc = "Unexpected end of command";
						goto error;
					}
				}
				if (cont_f == 2) {
					resv_p = tblend_p + 1;
				} else {
					resv_p = strtok(0, CMND_DELIMIT);
					if (resv_p) {
						if ((int)strlen(resv_p) == 1 && resv_p[0] == RESV_CONTINUE) {
							resv_p = strtok(0, CMND_DELIMIT);
							if (!resv_p) {
								desc = "Unexpected end of command";
								goto error;
							}
						} else if (resv_p[0] == RESV_CONTINUE) {
							resv_p++;
						} else {
							next_c = 1;
							check2_p = resv_p;
							resv_p = 0;
						}
					} else {
						next_c = 0;
						check1_p = check2_p = 0;
					}
				}
			}

			check_f[0] = 1;
		}


		/* Set the next check list */
		curr_p = target_p->sub_opts;

		for (count = 0; count < next_c; count++) {
			check1_p = check2_p;
			if (check2_p) {
				check2_p = strtok(0, CMND_DELIMIT);
			}
		}

		if (check1_p && !curr_p) {
			curr_p = trans_opt_S;
		}
	}

	/* Set the results */
	*tpb_len = used;
	return tpb;

error:
	xfree(tpb);
	rb_raise(rb_eFbError, "%s", desc);
}

static void fb_connection_transaction_start(struct FbConnection *fb_connection, VALUE opt)
{
	char *tpb = 0;
	long tpb_len;

	if (fb_connection->transact) {
		rb_raise(rb_eFbError, "A transaction has been already started");
	}

	if (!NIL_P(opt)) {
		tpb = trans_parseopts(opt, &tpb_len);
	} else {
		tpb_len = 0;
		tpb = NULL;
	}

	isc_start_transaction(fb_connection->isc_status, &fb_connection->transact, 1, &fb_connection->db, tpb_len, tpb);
	xfree(tpb);
	fb_error_check(fb_connection->isc_status);
}

static void fb_connection_commit(struct FbConnection *fb_connection)
{
	if (fb_connection->transact) {
		fb_connection_close_cursors(fb_connection);
		isc_commit_transaction(fb_connection->isc_status, &fb_connection->transact);
		fb_error_check(fb_connection->isc_status);
	}
}

static void fb_connection_rollback(struct FbConnection *fb_connection)
{
	if (fb_connection->transact) {
		fb_connection_close_cursors(fb_connection);
		isc_rollback_transaction(fb_connection->isc_status, &fb_connection->transact);
		fb_error_check(fb_connection->isc_status);
	}
}

/* call-seq:
 *   transaction(options) -> true
 *   transaction(options) { } -> block result
 *
 * Start a transaction for this connection.
 */
static VALUE connection_transaction(int argc, VALUE *argv, VALUE self)
{
	struct FbConnection *fb_connection;
	VALUE opt = Qnil;

	rb_scan_args(argc, argv, "01", &opt);
	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);

	fb_connection_transaction_start(fb_connection, opt);

	if (rb_block_given_p()) {
		int state;
		VALUE result = rb_protect(rb_yield, 0, &state);
		if (state) {
			fb_connection_rollback(fb_connection);
			return rb_funcall(rb_mKernel, rb_intern("raise"), 0);
		} else {
			fb_connection_commit(fb_connection);
			return result;
		}
	} else {
		return Qtrue;
   	}
}

/* call-seq:
 *   transaction_started()? -> true or false
 *
 * Returns true if a transaction is currently active.
 */
static VALUE connection_transaction_started(VALUE self)
{
	struct FbConnection *fb_connection;
	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);

	return fb_connection->transact ? Qtrue : Qfalse;
}

/* call-seq:
 *   commit() -> nil
 *
 * Commit the current transaction.
 */
static VALUE connection_commit(VALUE self)
{
	struct FbConnection *fb_connection;
	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);

	fb_connection_commit(fb_connection);
	return Qnil;
}

/* call-seq:
 *   rollback() -> nil
 *
 * Rollback the current transaction.
 */
static VALUE connection_rollback(VALUE self)
{
	struct FbConnection *fb_connection;
	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);

	fb_connection_rollback(fb_connection);
	return Qnil;
}

/*
 * call-seq:
 *   open?() -> true or false
 *
 * Current connection status.
 */

static VALUE connection_is_open(VALUE self)
{
	struct FbConnection *fb_connection;

	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);
	return (fb_connection->db == 0) ? Qfalse : Qtrue;
}

/* call-seq:
 *   to_s() -> String
 *
 * Return current database connection string and either (OPEN) or (CLOSED).
 */
static VALUE connection_to_s(VALUE self)
{
	VALUE is_open = connection_is_open(self);
	VALUE status = (is_open == Qtrue) ? rb_str_new2(" (OPEN)") : rb_str_new2(" (CLOSED)");
	VALUE database = rb_iv_get(self, "@database");
	VALUE s = rb_str_dup(database);
	return rb_str_concat(s, status);
}

/* call-seq:
 *   cursor() -> Cursor
 *
 * Creates a +Cursor+ for the +Connection+ and allocates a statement.
 * This function is no longer published.
 */
static VALUE connection_cursor(VALUE self)
{
	VALUE c;
	struct FbConnection *fb_connection;
	struct FbCursor *fb_cursor;

	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_connection_check(fb_connection);

	c = TypedData_Make_Struct(rb_cFbCursor, struct FbCursor, &fbcursor_data_type, fb_cursor);
	fb_cursor->connection = self;
	fb_cursor->fields_ary = Qnil;
	fb_cursor->fields_hash = Qnil;
	fb_cursor->open = Qfalse;
	fb_cursor->eof = Qfalse;
	fb_cursor->stmt = 0;
	fb_cursor->i_sqlda = sqlda_alloc(SQLDA_COLSINIT);
	fb_cursor->o_sqlda = sqlda_alloc(SQLDA_COLSINIT);
	fb_cursor->i_buffer = NULL;
	fb_cursor->i_buffer_size = 0;
	fb_cursor->o_buffer = NULL;
	fb_cursor->o_buffer_size = 0;
	fb_cursor->exec_procedure = 0;
	fb_cursor->fetched = 0;
	fb_cursor->cache = NULL;
	isc_dsql_alloc_statement2(fb_connection->isc_status, &fb_connection->db, &fb_cursor->stmt);
	fb_error_check(fb_connection->isc_status);

	return c;
}

/* call-seq:
 *   execute(sql, *args) -> Cursor or rows affected
 *   execute(sql, *args) {|cursor| } -> block result
 *
 * Allocates a +Cursor+ and executes the +sql+ statement, matching up the
 * parameters in +args+ with the place holders in the statement, represented by ?'s.
 *
 * If the sql statement returns a result set and a block is provided, the cursor is
 * yielded to the block before being automatically closed.
 *
 * If the sql statement returns a result set and a block is not provided, a +Cursor+
 * object is returned.
 *
 * If the sql statement performs an INSERT, UPDATE or DELETE, the number of rows
 * affected is returned.  Other statements, such as schema updates, return -1.
 *
 * +*args+ can be one or more tuples of parameters or an array of tuples (arrays),
 * in which case the statement will be executed once for each tuple of parameters.
 *
 * If no transaction is currently active, a transaction is automatically started
 * and is closed when the cursor is closed.  Note that if additional statements
 * yielding cursors are started before the first cursor is closed, these cursors will
 * also be closed when the first one is closed and its transaction committed.
 */
static VALUE connection_execute(int argc, VALUE *argv, VALUE self)
{
	VALUE cursor = connection_cursor(self);
	VALUE val = cursor_execute(argc, argv, cursor);

	if (NIL_P(val)) {
		if (rb_block_given_p()) {
			return rb_ensure(rb_yield,cursor,cursor_close,cursor);
   		} else {
				HERE("connection_execute Y");
				return cursor;
   		}
	} else {
		cursor_drop(cursor);
	}
	return val;
}

/* call-seq:
 *   query(:array, sql, *arg) -> Array of Arrays or nil
 *   query(:hash, sql, *arg) -> Array of Hashes or nil
 *   query(sql, *args) -> Array of Arrays or nil
 *
 * For queries returning a result set, an array is returned, containing
 * either a list of Arrays or Hashes, one for each row.
 *
 * If the sql statement performs an INSERT, UPDATE or DELETE, the number of rows
 * affected is returned.  Other statements, such as schema updates, return -1.
 *
 * If no transaction is currently active, a transaction is automatically started
 * and committed.  Otherwise, the statement executes within the context of the
 * current transaction.
 */
static VALUE connection_query(int argc, VALUE *argv, VALUE self)
{
	VALUE format;
	VALUE cursor;
	VALUE result;

	if (argc >= 1 && TYPE(argv[0]) == T_SYMBOL) {
		format = argv[0];
		argc--; argv++;
	} else {
		format = ID2SYM(rb_intern("array"));
	}
	cursor = connection_cursor(self);
	result = cursor_execute(argc, argv, cursor);
	if (NIL_P(result)) {
		result = cursor_fetchall(1, &format, cursor);
		cursor_close(cursor);
	}

	return result;
}

/* call-seq:
 *   encoding = name -> name
 *
 * Cambia el encoding con el que el driver etiqueta los strings que devuelve.
 * Actualiza el ivar y el campo C (el ivar solo no basta: el driver usa el C).
 */
static VALUE connection_set_encoding(VALUE self, VALUE encoding)
{
	struct FbConnection *fb_connection;

	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_connection->encoding = encoding;
	rb_iv_set(self, "@encoding", encoding);
	return encoding;
}

/* call-seq:
 *   command_timeout() -> Float
 *
 * Segundos maximos que puede tardar una sentencia en esta conexion (0 = sin
 * limite). Se aplica a cada sentencia en el momento de prepararla.
 */
static VALUE connection_command_timeout(VALUE self)
{
	struct FbConnection *fb_connection;

	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);
	return rb_float_new(fb_connection->command_timeout_ms / 1000.0);
}

/* call-seq:
 *   command_timeout = seconds -> seconds
 */
static VALUE connection_set_command_timeout(VALUE self, VALUE seconds)
{
	struct FbConnection *fb_connection;

	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);
	if (NIL_P(seconds)) {
		fb_connection->command_timeout_ms = 0;
	} else {
		double secs = NUM2DBL(seconds);
		if (secs < 0.0)
			rb_raise(rb_eArgError, "command_timeout must be >= 0");
		fb_connection->command_timeout_ms = (long) (secs * 1000.0);
	}
	return seconds;
}

/* call-seq:
 *   cancel() -> true
 *
 * Pide a Firebird que cancele la operacion en curso en esta conexion. Pensado
 * para llamarse desde otro hilo (la ejecucion cede el GVL mientras espera).
 */
static VALUE connection_cancel(VALUE self)
{
#ifdef HAVE_FB_CANCEL_OPERATION
	struct FbConnection *fb_connection;
	ISC_STATUS status[20];

	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_connection_check(fb_connection);
	memset(status, 0, sizeof(status));
	fb_cancel_operation(status, &fb_connection->db, fb_cancel_raise);
	fb_error_check(status);
	return Qtrue;
#else
	rb_raise(rb_eFbError, "cancel is not supported by this Firebird client");
#endif
}

/* call-seq:
 *   close() -> nil
 *
 * Closes connection to database.  All open cursors are dropped.
 */
static VALUE connection_close(VALUE self)
{
	struct FbConnection *fb_connection;

	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);

	if (fb_connection->dropped) return Qnil;

	fb_connection_check(fb_connection);
	fb_connection_disconnect(fb_connection);
	fb_connection_drop_cursors(fb_connection);

	return Qnil;
}

/* call-seq:
 *   drop() -> nil
 *
 * Drops connected database.  All open cursors are dropped.
 */
static VALUE connection_drop(VALUE self)
{
	struct FbConnection *fb_connection;

	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_connection->dropped = 1;
	fb_connection_disconnect(fb_connection);
	fb_connection_drop_cursors(fb_connection);

	return Qnil;
}

/* call-seq:
 *   dialect() -> int
 *
 * Returns dialect of connection.
 */
static VALUE connection_dialect(VALUE self)
{
	struct FbConnection *fb_connection;

	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_connection_check(fb_connection);

	return INT2FIX(fb_connection->dialect);
}

/* call-seq:
 *   db_dialect() -> int
 *
 * Returns database dialect.
 */
static VALUE connection_db_dialect(VALUE self)
{
	struct FbConnection *fb_connection;

	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_connection_check(fb_connection);

	return INT2FIX(fb_connection->db_dialect);
}

static void fb_cursor_check(struct FbCursor *fb_cursor)
{
	if (fb_cursor->stmt == 0) {
		rb_raise(rb_eFbError, "dropped db cursor");
	}
	if (!fb_cursor->open) {
		rb_raise(rb_eFbError, "closed db cursor");
	}
}

static void fb_cursor_drop(struct FbCursor *fb_cursor)
{
	ISC_STATUS isc_status[20];
	if (fb_cursor->open) {
		isc_dsql_free_statement(isc_status, &fb_cursor->stmt, DSQL_close);
		fb_error_check(isc_status);
	}
	if (fb_cursor->cache) {
		struct FbConnection *fb_connection;
		TypedData_Get_Struct(fb_cursor->connection, struct FbConnection, &fbconnection_data_type, fb_connection);
		fb_stmt_cache_release(fb_connection, fb_cursor);	/* la sentencia vuelve a la cache */
		return;
	}
	if (fb_cursor->stmt) {
		isc_dsql_free_statement(isc_status, &fb_cursor->stmt, DSQL_drop);
		fb_error_check(isc_status);
		fb_cursor->stmt = 0;
	}
}

static void fb_cursor_drop_warn(struct FbCursor *fb_cursor)
{
	ISC_STATUS isc_status[20];
	if (fb_cursor->open) {
		isc_dsql_free_statement(isc_status, &fb_cursor->stmt, DSQL_close);
		fb_error_check_warn(isc_status);
	}
	if (fb_cursor->cache) {
		struct FbConnection *fb_connection;
		TypedData_Get_Struct(fb_cursor->connection, struct FbConnection, &fbconnection_data_type, fb_connection);
		fb_stmt_cache_release(fb_connection, fb_cursor);
		return;
	}
	if (fb_cursor->stmt) {
		isc_dsql_free_statement(isc_status, &fb_cursor->stmt, DSQL_drop);
		fb_error_check_warn(isc_status);
		fb_cursor->stmt = 0;
	}
}

static void fb_cursor_mark(struct FbCursor *fb_cursor)
{
	rb_gc_mark(fb_cursor->connection);
	rb_gc_mark(fb_cursor->fields_ary);
	rb_gc_mark(fb_cursor->fields_hash);
}

static void fb_cursor_free(struct FbCursor *fb_cursor)
{
	if (fb_cursor->stmt) {
		if (fb_cursor->cache) {
			/* La sentencia y los SQLDA pertenecen a la cache de la conexion.
			   Aqui corremos dentro del GC: la conexion puede estar ya liberada,
			   asi que NO se puede desreferenciar fb_cursor->connection ni tocar
			   la entrada. Se cierra/destruye la sentencia con un status local
			   (si la conexion sigue viva es correcto; si ya no, solo avisa) y
			   la entrada queda marcada en uso hasta que la conexion muera. */
			/* La sentencia (y sus SQLDA) son de la entrada de la cache: este
			   cursor NO debe tocarlas. Ni DSQL_drop ni DSQL_close: aqui corremos
			   dentro del GC (puede que mientras otra llamada a Firebird esta en
			   vuelo, con el GVL cedido) y tocar un handle ajeno escribia sobre
			   memoria ya reciclada -> segfault al llegar un DDL
			   (valgrind: invalid write de isc_dsql_free_statement dentro de
			   fb_stmt_cache_destroy, en un bloque que era un FbCursor liberado).
			   La entrada se libera cuando la cache haga invalidate/dispose. */
			fb_cursor->stmt = 0; /* solo olvidarlo */
		} else {
			fb_cursor_drop_warn(fb_cursor);
		}
	}
	if (!fb_cursor->cache) {	/* los SQLDA de una sentencia cacheada son de la entrada */
		xfree(fb_cursor->i_sqlda);
		xfree(fb_cursor->o_sqlda);
	}
	xfree(fb_cursor->i_buffer);
	xfree(fb_cursor->o_buffer);
	xfree(fb_cursor);
}

static VALUE sql_decimal_to_bigdecimal(long long sql_data, int scale)
{
	char bigdecimal_buffer[128];  /* Increased buffer size */
	int is_negative = 0;
	unsigned long long abs_data;
	int len, decimal_places, dot_pos;
	
	/* Handle negative numbers */
	if (sql_data < 0) {
		is_negative = 1;
		/* Use unsigned arithmetic to avoid overflow with minimum integer value */
		abs_data = (unsigned long long)(-sql_data);
	} else {
		abs_data = (unsigned long long)sql_data;
	}
	
	/* Convert to string */
	int wrote = snprintf(bigdecimal_buffer, sizeof(bigdecimal_buffer), "%llu", abs_data);
	if (wrote < 0 || wrote >= (int)sizeof(bigdecimal_buffer)) {
		rb_raise(rb_eRuntimeError, "Buffer overflow in decimal conversion");
	}
	len = wrote;
	
	/* Add decimal point if scale != 0 */
	if (scale != 0) {
		decimal_places = (scale < 0) ? -scale : scale;
		dot_pos = len - decimal_places;
		
		/* If we need leading zeros before the decimal point */
		if (dot_pos <= 0) {
			/* Need to add leading zeros to ensure at least one digit before decimal point */
			int leading_zeros = 1 - dot_pos;
			
			/* Check buffer bounds */
			if (len + leading_zeros + 1 >= (int)sizeof(bigdecimal_buffer)) {
				rb_raise(rb_eRuntimeError, "Buffer overflow in decimal conversion");
			}
			
			/* Shift the string to make room for leading zeros */
			memmove(bigdecimal_buffer + leading_zeros, bigdecimal_buffer, len + 1);
			
			/* Add leading zeros */
			memset(bigdecimal_buffer, '0', leading_zeros);
			
			len += leading_zeros;
			dot_pos = len - decimal_places;
		}
		
		/* Check buffer bounds before inserting decimal point */
		if (len + 1 >= (int)sizeof(bigdecimal_buffer)) {
			rb_raise(rb_eRuntimeError, "Buffer overflow in decimal conversion");
		}
		
		/* Insert decimal point */
		memmove(bigdecimal_buffer + dot_pos + 1, bigdecimal_buffer + dot_pos, len - dot_pos);
		bigdecimal_buffer[dot_pos] = '.';
		bigdecimal_buffer[len + 1] = '\0';
	}
	
	/* Add minus sign if needed - do this AFTER decimal point placement */
	if (is_negative) {
		/* Shift string to make room for minus sign */
		len = (int)strlen(bigdecimal_buffer);
		memmove(bigdecimal_buffer + 1, bigdecimal_buffer, len + 1);
		bigdecimal_buffer[0] = '-';
		bigdecimal_buffer[len + 1] = '\0';  /* Ensure proper null termination */
	}
	
	return rb_funcall(rb_cObject, rb_intern("BigDecimal"), 1, rb_str_new2(bigdecimal_buffer));
}

static VALUE object_to_unscaled_bigdecimal(VALUE object, int scale)
{
	int i;
	long ratio = 1;
	for (i = 0; i > scale; i--)
		ratio *= 10;
	if (TYPE(object) == T_FLOAT)
		object = rb_funcall(object, rb_intern("to_s"), 0);
	object = rb_funcall(rb_cObject, rb_intern("BigDecimal"), 1, object);
	object = rb_funcall(object, rb_intern("*"), 1, LONG2NUM(ratio));
	return rb_funcall(object, rb_intern("round"), 0);
}

/* Los tipos "con zona" (Firebird 4+) se escriben como instante en UTC con la
   zona GMT: la zona de Firebird es un id de 16 bits de RDB$TIME_ZONES (GMT =
   0xFFFF) y un Time de Ruby es un offset fijo, asi que se normaliza la zona
   pero se conserva el instante exacto. */
#define FB_TIME_ZONE_GMT	65535

static void fb_tm_from_zoned(struct tm *tm, VALUE obj)
{
	VALUE t = obj;
	if (!rb_obj_is_kind_of(t, rb_cTime)) {
		if (rb_respond_to(t, rb_intern("to_str")))
			t = rb_funcall(rb_cTime, rb_intern("parse"), 1, rb_funcall(t, rb_intern("to_str"), 0));
		else if (rb_respond_to(t, rb_intern("to_time")))
			t = rb_funcall(t, rb_intern("to_time"), 0);
		else
			rb_raise(rb_eTypeError, "Expecting Time object or string.");
	}
	tm_from_timestamp(tm, rb_funcall(t, rb_intern("getutc"), 0));
}

static void fb_cursor_set_inputparams(struct FbCursor *fb_cursor, long argc, VALUE *argv)
{
	struct FbConnection *fb_connection;
	long count;
	long offset;
	/* long type; */
	short dtp;
	VALUE obj;
	long lvalue;
	ISC_INT64 llvalue;
	long alignment;
	double dvalue;
	double dcheck;
	VARY *vary;
	XSQLVAR *var;

	isc_blob_handle blob_handle;
	ISC_QUAD blob_id;
	/* static char blob_items[] = { isc_info_blob_max_segment }; */
	/* char blob_info[16]; */
	char *p;
	long length;
	/* struct time_object *tobj; */
	struct tm tms;

	TypedData_Get_Struct(fb_cursor->connection, struct FbConnection, &fbconnection_data_type, fb_connection);

	/* Check the number of parameters */
	if (fb_cursor->i_sqlda->sqld != argc) {
		rb_raise(rb_eFbError, "statement requires %d items; %ld given", fb_cursor->i_sqlda->sqld, argc);
	}

	/* Get the parameters */
	for (count = 0,offset = 0; count < argc; count++) {
		obj = argv[count];

		/* type = TYPE(obj); */

		/* Convert the data type for InterBase */
		var = &fb_cursor->i_sqlda->sqlvar[count];
		if (!NIL_P(obj)) {
			dtp = var->sqltype & ~1;		/* Erase null flag */
			alignment = var->sqllen;

			switch (dtp) {
				case SQL_TEXT :
					alignment = 1;
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					obj = rb_obj_as_string(obj);
					if (RSTRING_LEN(obj) > var->sqllen) {
						rb_raise(rb_eRangeError, "CHAR overflow: %ld bytes exceeds %d byte(s) allowed.",
							RSTRING_LEN(obj), var->sqllen);
					}
					/* Relleno a la longitud declarada. Antes se mutaba var->sqllen
					   para mandar solo los bytes del valor; con la cache de sentencias
					   esa descripcion se comparte entre cursores, asi que la mutacion
					   corromperia el resto de usos. */
					memset(var->sqldata, ' ', var->sqllen);
					memcpy(var->sqldata, RSTRING_PTR(obj), RSTRING_LEN(obj));
					offset += var->sqllen;
					break;

				case SQL_VARYING :
					alignment = sizeof(short);
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					vary = (VARY *)var->sqldata;
					obj = rb_obj_as_string(obj);
					if (RSTRING_LEN(obj) > var->sqllen) {
						rb_raise(rb_eRangeError, "VARCHAR overflow: %ld bytes exceeds %d byte(s) allowed.",
							RSTRING_LEN(obj), var->sqllen);
					}
					memcpy(vary->vary_string, RSTRING_PTR(obj), RSTRING_LEN(obj));
					vary->vary_length = RSTRING_LEN(obj);
					offset += vary->vary_length + sizeof(short);
					break;

				case SQL_SHORT :
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					if (var->sqlscale < 0) {
						lvalue = NUM2LONG(object_to_unscaled_bigdecimal(obj, var->sqlscale));
					} else {
						lvalue = NUM2LONG(object_to_fixnum(obj));
					}
					if (lvalue < -32768 || lvalue > 32767) {
						rb_raise(rb_eRangeError, "short integer overflow");
					}
					*(short *)var->sqldata = lvalue;
					offset += alignment;
					break;

				case SQL_LONG :
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					if (var->sqlscale < 0) {
						lvalue = NUM2LONG(object_to_unscaled_bigdecimal(obj, var->sqlscale));
					} else {
						lvalue = NUM2LONG(object_to_fixnum(obj));
					}
					if (lvalue < -2147483648LL || lvalue > 2147483647LL) {
						rb_raise(rb_eRangeError, "integer overflow");
					}
					*(ISC_LONG *)var->sqldata = (ISC_LONG)lvalue;
					offset += alignment;
					break;

				case SQL_FLOAT :
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					obj = double_from_obj(obj);
					dvalue = NUM2DBL(obj);
					if (dvalue >= 0.0) {
						dcheck = dvalue;
					} else {
						dcheck = dvalue * -1;
					}
					if (dcheck != 0.0 && (dcheck < FLT_MIN || dcheck > FLT_MAX)) {
						rb_raise(rb_eRangeError, "float overflow");
					}
					*(float *)var->sqldata = (float)dvalue;
					offset += alignment;
					break;

				case SQL_DOUBLE :
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					obj = double_from_obj(obj);
					dvalue = NUM2DBL(obj);
					*(double *)var->sqldata = dvalue;
					offset += alignment;
					break;

				case SQL_INT64 :
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);

					if (var->sqlscale < 0) {
						llvalue = NUM2LL(object_to_unscaled_bigdecimal(obj, var->sqlscale));
					} else {
						llvalue = NUM2LL(object_to_fixnum(obj));
					}

					*(ISC_INT64 *)var->sqldata = llvalue;
					offset += alignment;
					break;

				case SQL_BLOB :
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					obj = rb_obj_as_string(obj);

					blob_handle = 0;
					isc_create_blob2(
						fb_connection->isc_status,&fb_connection->db,&fb_connection->transact,
						&blob_handle,&blob_id,0,NULL);
					fb_error_check(fb_connection->isc_status);
					length = RSTRING_LEN(obj);
					p = RSTRING_PTR(obj);
					while (length >= FB_BLOB_SEGMENT_SIZE) {
						isc_put_segment(fb_connection->isc_status,&blob_handle,FB_BLOB_SEGMENT_SIZE,p);
						fb_error_check(fb_connection->isc_status);
						p += FB_BLOB_SEGMENT_SIZE;
						length -= FB_BLOB_SEGMENT_SIZE;
					}
					if (length) {
						isc_put_segment(fb_connection->isc_status,&blob_handle,length,p);
						fb_error_check(fb_connection->isc_status);
					}
					isc_close_blob(fb_connection->isc_status,&blob_handle);
					fb_error_check(fb_connection->isc_status);

					*(ISC_QUAD *)var->sqldata = blob_id;
					offset += alignment;
					break;

				case SQL_TIMESTAMP :
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					tm_from_timestamp(&tms, obj);
					isc_encode_timestamp(&tms, (ISC_TIMESTAMP *)var->sqldata);
					offset += alignment;
					break;

				case SQL_TYPE_TIME :
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					tm_from_timestamp(&tms, obj);
					isc_encode_sql_time(&tms, (ISC_TIME *)var->sqldata);
					offset += alignment;
					break;

				case SQL_TYPE_DATE :
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					tm_from_date(&tms, obj);
					isc_encode_sql_date(&tms, (ISC_DATE *)var->sqldata);
					offset += alignment;
					break;

#if 0
				case SQL_ARRAY :
					/* Not supported now
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					if (get_arrayvalue(self, type, obj, var))
						return(STATUS_ABNORMAL);
					offset += alignment;
					break;
					*/
					rb_raise(rb_eArgError, "Arrays not supported");
					break;
#endif

				case SQL_BOOLEAN:
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					*(bool *)var->sqldata = obj;
					offset += alignment;
					break;

				case SQL_INT128:
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					/* 128 bits completos: un bignum no pasa por NUM2LL */
					fb_int128_write(var->sqldata, (var->sqlscale < 0)
						? object_to_unscaled_bigdecimal(obj, var->sqlscale)
						: object_to_fixnum(obj));
					offset += alignment;
					break;

				case SQL_TIMESTAMP_TZ:
				case SQL_TIMESTAMP_TZ_EX:
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					fb_tm_from_zoned(&tms, obj);
					isc_encode_timestamp(&tms, (ISC_TIMESTAMP *)var->sqldata);
					/* el campo son 12 bytes: instante (8) + id de zona (2) */
					*(unsigned short *)(var->sqldata + sizeof(ISC_TIMESTAMP)) = FB_TIME_ZONE_GMT;
					offset += alignment;
					break;

				case SQL_TIME_TZ:
				case SQL_TIME_TZ_EX:
					offset = FB_ALIGN(offset, alignment);
					var->sqldata = (char *)(fb_cursor->i_buffer + offset);
					fb_tm_from_zoned(&tms, obj);
					isc_encode_sql_time(&tms, (ISC_TIME *)var->sqldata);
					/* 8 bytes: hora (4) + id de zona (2) */
					*(unsigned short *)(var->sqldata + sizeof(ISC_TIME)) = FB_TIME_ZONE_GMT;
					offset += alignment;
					break;

				case SQL_DEC16:
				case SQL_DEC34:
					rb_raise(rb_eFbError, "DECFLOAT datatype is not supported in this driver version");

				default:
					rb_raise(rb_eFbError, "Specified table includes unsupported datatype (%d)", dtp);
			}

			if (var->sqltype & 1) {
				offset = FB_ALIGN(offset, sizeof(short));
				var->sqlind = (short *)(fb_cursor->i_buffer + offset);
				*var->sqlind = 0;
				offset += sizeof(short);
			}
		} else if (var->sqltype & 1) {
			var->sqldata = 0;
			offset = FB_ALIGN(offset, sizeof(short));
			var->sqlind = (short *)(fb_cursor->i_buffer + offset);
			*var->sqlind = -1;
			offset += sizeof(short);
		} else {
			rb_raise(rb_eFbError, "specified column is not permitted to be null");
		}
	}
}

#ifdef FB_HAVE_BATCH_API

/* ==========================================================================
 * Batch de protocolo (Firebird 4+)
 *
 * El camino legacy ejecuta isc_dsql_execute2 una vez por registro, o sea un
 * round trip por fila. Con IBatch los mensajes se agrupan y se mandan en una
 * sola operacion (op_batch_create/msg/exec, que implementa libfbclient; desde
 * C no se emiten a mano). Los puentes fb_get_statement_interface /
 * fb_get_transaction_interface (Firebird 4.0+) convierten los handles legacy
 * de fb.c en los objetos que necesita la API OO.
 * ========================================================================== */

#define FB_BATCH_ROWS_PER_ADD	500
/* Tamano del buffer de batch que se declara en el BPB. */
#define FB_BATCH_BUFFER_BYTES	(32 * 1024 * 1024)
/* Umbral de troceo del cliente: la MITAD del declarado, porque el servidor
 * cuenta tambien las cabeceras BLR ademas de los mensajes y su comprobacion
 * salta antes de llegar al limite declarado (si se usa el mismo valor, el
 * IBatch_add que cruza el limite falla antes de la primera ejecucion). */
#define FB_BATCH_FLUSH_BYTES	(FB_BATCH_BUFFER_BYTES / 2)
/* Minimo de filas para que el batch compense: con una sola fila paga los
 * puentes y un createBatch extra (~4 us) sin ganar nada. */
#define FB_BATCH_MIN_ROWS	2
/* Fila mas grande que se acepta empaquetar en el lote (mensaje + blobs). Los
 * blobs ocupan lugar en el buffer del lote del servidor, asi que una fila
 * enorme (un blob de decenas de MB) no entra: esa va por el camino legacy,
 * que no tiene ese limite. */
#define FB_BATCH_MAX_ROW_BYTES	(FB_BATCH_FLUSH_BYTES / 2)

typedef struct {
	unsigned offset;
	unsigned null_offset;		/* (unsigned)-1 si el parametro no admite NULL */
	unsigned type;				/* SQL_* */
	unsigned length;
	int		 scale;
	int		 has_null;
} FbBatchField;

static IMaster *fb_oo_master;
static IUtil   *fb_oo_util;
static IStatus *fb_oo_status;

/* El master interface se pide una sola vez por proceso. */
static IStatus *fb_batch_status(void)
{
	if (!fb_oo_master) {
		fb_oo_master = fb_get_master_interface();
		if (!fb_oo_master)
			return NULL;
		fb_oo_util = IMaster_getUtilInterface(fb_oo_master);
		fb_oo_status = IMaster_getStatus(fb_oo_master);
	}
	return fb_oo_status;
}

/* IStatus::getErrors() devuelve el mismo vector ISC_STATUS que usa la API
   legacy, asi que el reporte de errores se reutiliza tal cual. */
static void fb_batch_error_check(IStatus *status)
{
	if (IStatus_getState(status) & IStatus_STATE_ERRORS) {
		ISC_STATUS *vector = (ISC_STATUS *) IStatus_getErrors(status);
		fb_error_check(vector);
		rb_raise(rb_eFbError, "Firebird batch error");
	}
}

/* Suelta las interfaces obtenidas y devuelve -1 (= usar el loop legacy). */
static long fb_batch_give_up(IStatement *istmt, ITransaction *itra,
							 IMessageMetadata *meta, IXpbBuilder *pb, IBatch *batch)
{
	if (batch) IBatch_release(batch);
	if (pb) IXpbBuilder_dispose(pb);
	if (meta) IMessageMetadata_release(meta);
	if (istmt) IStatement_release(istmt);
	if (itra) ITransaction_release(itra);
	return -1;
}

/* Bytes que ocupan los blobs de una fila (0 si no hay blobs). Sirve para
   decidir cuantas filas entran en un lote: el servidor cuenta los blobs dentro
   de su buffer de batch, asi que hay que contarlos junto con los mensajes. */
static size_t fb_batch_row_blob_bytes(VALUE row, unsigned nparams, FbBatchField *f)
{
	size_t total = 0;
	unsigned i;

	if (TYPE(row) != T_ARRAY || RARRAY_LEN(row) != (long) nparams)
		return 0;

	for (i = 0; i < nparams; i++) {
		VALUE obj;
		if (f[i].type != SQL_BLOB)
			continue;
		obj = RARRAY_PTR(row)[i];
		if (NIL_P(obj))
			continue;
		total += (size_t) RSTRING_LEN(rb_obj_as_string(obj));
	}
	return total;
}

/* Empaqueta una fila en el buffer de mensajes segun la metadata.
   Devuelve NULL si todo bien, o el mensaje de error (sin lanzar excepcion,
   para poder liberar memoria antes de hacerlo). */
static const char *fb_batch_pack_row(IBatch *batch, IStatus *status, char *msg, VALUE *vals,
									 unsigned nparams, FbBatchField *f, struct tm *tms)
{
	unsigned i;

	for (i = 0; i < nparams; i++) {
		VALUE obj = vals[i];
		char *dest = msg + f[i].offset;

		if (f[i].has_null)
			*(short *)(msg + f[i].null_offset) = NIL_P(obj) ? -1 : 0;

		if (NIL_P(obj)) {
			if (!f[i].has_null)
				return "specified column is not permitted to be null";
			continue;
		}

		switch (f[i].type) {
		case SQL_VARYING: {
			VARY *vary = (VARY *) dest;
			obj = rb_obj_as_string(obj);
			if ((unsigned) RSTRING_LEN(obj) > f[i].length)
				return "VARCHAR overflow: value exceeds the column length";
			vary->vary_length = (short) RSTRING_LEN(obj);
			memcpy(vary->vary_string, RSTRING_PTR(obj), RSTRING_LEN(obj));
			break;
		}
		case SQL_TEXT:
			obj = rb_obj_as_string(obj);
			if ((unsigned) RSTRING_LEN(obj) > f[i].length)
				return "CHAR overflow: value exceeds the column length";
			memset(dest, ' ', f[i].length);
			memcpy(dest, RSTRING_PTR(obj), RSTRING_LEN(obj));
			break;
		case SQL_SHORT: {
			long lvalue = (f[i].scale < 0)
				? NUM2LONG(object_to_unscaled_bigdecimal(obj, f[i].scale))
				: NUM2LONG(object_to_fixnum(obj));
			if (lvalue < -32768 || lvalue > 32767)
				return "short integer overflow";
			*(short *) dest = (short) lvalue;
			break;
		}
		case SQL_LONG: {
			long lvalue = (f[i].scale < 0)
				? NUM2LONG(object_to_unscaled_bigdecimal(obj, f[i].scale))
				: NUM2LONG(object_to_fixnum(obj));
			if (lvalue < -2147483648LL || lvalue > 2147483647LL)
				return "integer overflow";
			*(ISC_LONG *) dest = (ISC_LONG) lvalue;
			break;
		}
		case SQL_INT64:
			*(ISC_INT64 *) dest = (f[i].scale < 0)
				? NUM2LL(object_to_unscaled_bigdecimal(obj, f[i].scale))
				: NUM2LL(object_to_fixnum(obj));
			break;
		case SQL_FLOAT: {
			double dvalue = NUM2DBL(double_from_obj(obj));
			double dcheck = (dvalue < 0.0) ? -dvalue : dvalue;
			if (dcheck != 0.0 && (dcheck < FLT_MIN || dcheck > FLT_MAX))
				return "float overflow";
			*(float *) dest = (float) dvalue;
			break;
		}
		case SQL_DOUBLE:
			*(double *) dest = NUM2DBL(double_from_obj(obj));
			break;
		case SQL_TIMESTAMP:
			tm_from_timestamp(tms, obj);
			isc_encode_timestamp(tms, (ISC_TIMESTAMP *) dest);
			break;
		case SQL_TYPE_TIME:
			tm_from_timestamp(tms, obj);
			isc_encode_sql_time(tms, (ISC_TIME *) dest);
			break;
		case SQL_TYPE_DATE:
			tm_from_date(tms, obj);
			isc_encode_sql_date(tms, (ISC_DATE *) dest);
			break;
		case SQL_BOOLEAN:
			*(bool *) dest = RTEST(obj) ? true : false;
			break;
		case SQL_INT128:
			fb_int128_write(dest, (f[i].scale < 0)
				? object_to_unscaled_bigdecimal(obj, f[i].scale)
				: object_to_fixnum(obj));
			break;
		case SQL_BLOB: {
			/* El blob no viaja en el mensaje: el mensaje lleva solo su id, y
			   los bytes van por el flujo de blobs del batch (IBatch_addBlob),
			   que la libreria cliente empaqueta y manda junto con los
			   mensajes. Antes cada blob pagaba create_blob, un put_segment por
			   cada 64 KB (o cada 4 KB) y el close, todo con su ida y vuelta. */
			ISC_QUAD blob_id;
			long blob_length;
			char *blob_ptr;

			memset(&blob_id, 0, sizeof(blob_id));
			obj = rb_obj_as_string(obj);
			blob_length = RSTRING_LEN(obj);
			blob_ptr = RSTRING_PTR(obj);

			if (blob_length <= FB_BLOB_SEGMENT_SIZE) {
				IBatch_addBlob(batch, status, (unsigned) blob_length, blob_ptr,
							   &blob_id, 0, NULL);
			} else {
				unsigned chunk;
				IBatch_addBlob(batch, status, (unsigned) FB_BLOB_SEGMENT_SIZE, blob_ptr,
							   &blob_id, 0, NULL);
				blob_ptr += FB_BLOB_SEGMENT_SIZE;
				blob_length -= FB_BLOB_SEGMENT_SIZE;
				/* El resto va en trozos del tamano maximo de segmento: con
				   blobs segmentados (el modo que declara el driver, igual que
				   isc_create_blob2) un segmento no puede pasar de 64 KB - 1. */
				while (blob_length > 0 &&
					   !(IStatus_getState(status) & IStatus_STATE_ERRORS)) {
					chunk = (blob_length > FB_BLOB_SEGMENT_SIZE)
						? (unsigned) FB_BLOB_SEGMENT_SIZE : (unsigned) blob_length;
					IBatch_appendBlobData(batch, status, chunk, blob_ptr);
					blob_ptr += chunk;
					blob_length -= chunk;
				}
			}

			if (IStatus_getState(status) & IStatus_STATE_ERRORS)
				return "no se pudo enviar el blob al batch";

			*(ISC_QUAD *) dest = blob_id;
			break;
		}
		default:
			return "datatype not supported by the batch path";
		}
	}
	return NULL;
}

/* Ejecuta el lote acumulado y devuelve las filas afectadas; en error deja el
   indice y el texto en fail_pos/fail_msg y devuelve -1. */
static long fb_batch_execute_and_count(IBatch *batch, ITransaction *itra, IStatus *status,
									   unsigned *fail_pos, char *fail_msg, size_t fail_msg_size)
{
	IBatchCompletionState *cs;
	unsigned size, p;
	long ok = 0;

	IStatus_init(status);
	cs = IBatch_execute(batch, status, itra);
	fb_batch_error_check(status);
	if (!cs)
		return -1;

	size = IBatchCompletionState_getSize(cs, status);
	for (p = 0; p < size; p++) {
		int state = IBatchCompletionState_getState(cs, status, p);
		if (state == IBatchCompletionState_EXECUTE_FAILED) {
			IStatus *detail = IMaster_getStatus(fb_oo_master);
			char text[1024];
			if (detail) {
				IBatchCompletionState_getStatus(cs, status, detail, p);
				IUtil_formatStatus(fb_oo_util, text, sizeof(text) - 1, detail);
				text[sizeof(text) - 1] = 0;
				IStatus_dispose(detail);
				snprintf(fail_msg, fail_msg_size, "record %u: %s", p, text);
			}
			else
				snprintf(fail_msg, fail_msg_size, "record %u failed", p);
			*fail_pos = p;
			IBatchCompletionState_dispose(cs);
			return -1;
		}
		if (state > 0)
			ok += state;
		else
			ok++;			/* SUCCESS_NO_INFO: se ejecuto, sin conteo */
	}
	IBatchCompletionState_dispose(cs);
	return ok;
}

/* Camino rapido para con.execute(sql, [fila1, fila2, ...]).
   Devuelve las filas insertadas, o -1 si no aplica (el llamador usa el loop). */
static long fb_cursor_execute_batch(struct FbCursor *fb_cursor, struct FbConnection *fb_connection,
									VALUE filas)
{
	IStatus *status = fb_batch_status();
	IStatement *istmt = NULL;
	ITransaction *itra = NULL;
	IMessageMetadata *meta = NULL;
	IXpbBuilder *pb = NULL;
	IBatch *batch = NULL;
	FbBatchField *f;
	struct tm tms;
	char *buf = NULL;
	char fail_msg[1200];
	const char *pack_err = NULL;
	unsigned nparams = 0, msg_len = 0, i;
	long nrows, done = 0, inserted = 0;
	size_t pending_bytes = 0;
	int executed_any = 0;
	int has_blob = 0;

	if (!status)
		return -1;

	nrows = RARRAY_LEN(filas);
	if (nrows < FB_BATCH_MIN_ROWS)
		return -1;	/* una sola fila va mejor por el loop legacy */

	/* 1) objetos OO a partir de los handles legacy */
	if (fb_get_statement_interface(fb_connection->isc_status, &istmt, &fb_cursor->stmt) != 0 || !istmt)
		return fb_batch_give_up(NULL, NULL, NULL, NULL, NULL);
	if (fb_get_transaction_interface(fb_connection->isc_status, &itra, &fb_connection->transact) != 0 || !itra)
		return fb_batch_give_up(istmt, NULL, NULL, NULL, NULL);

	/* 2) metadata de los parametros de entrada */
	IStatus_init(status);
	meta = IStatement_getInputMetadata(istmt, status);
	if (!meta || (IStatus_getState(status) & IStatus_STATE_ERRORS)) {
		IStatus_init(status);
		return fb_batch_give_up(istmt, itra, NULL, NULL, NULL);
	}
	nparams = IMessageMetadata_getCount(meta, status);
	msg_len = IMessageMetadata_getMessageLength(meta, status);
	{
		/* Cada mensaje del buffer debe empezar en un limite alineado: si la
		   longitud declarada no lo esta, hay que usar la alineada como paso
		   (si no, el servidor lee los registros corridos). */
		unsigned aligned = IMessageMetadata_getAlignedLength(meta, status);
		if (aligned > msg_len)
			msg_len = aligned;
	}
	if ((IStatus_getState(status) & IStatus_STATE_ERRORS) || nparams == 0 || msg_len == 0) {
		IStatus_init(status);
		return fb_batch_give_up(istmt, itra, meta, NULL, NULL);
	}

	f = (FbBatchField *) ALLOCA_N(FbBatchField, nparams);
	for (i = 0; i < nparams; i++) {
		unsigned noff = IMessageMetadata_getNullOffset(meta, status, i);
		f[i].type = IMessageMetadata_getType(meta, status, i);
		f[i].length = IMessageMetadata_getLength(meta, status, i);
		f[i].offset = IMessageMetadata_getOffset(meta, status, i);
		f[i].scale = IMessageMetadata_getScale(meta, status, i);
		f[i].null_offset = noff;
		f[i].has_null = (noff != (unsigned) -1);
		switch (f[i].type) {
		case SQL_VARYING: case SQL_TEXT:
		case SQL_SHORT: case SQL_LONG: case SQL_INT64:
		case SQL_FLOAT: case SQL_DOUBLE:
		case SQL_TIMESTAMP: case SQL_TYPE_TIME: case SQL_TYPE_DATE:
		case SQL_BOOLEAN: case SQL_INT128:
			break;
		case SQL_BLOB:
			/* los blobs van por IBatch_addBlob; hay que declarar la politica
			   en el BPB (mas abajo) */
			has_blob = 1;
			break;
		default:
			/* tipo que el batch no maneja: que lo haga el camino legacy */
			IStatus_init(status);
			return fb_batch_give_up(istmt, itra, meta, NULL, NULL);
		}
		if (IStatus_getState(status) & IStatus_STATE_ERRORS) {
			IStatus_init(status);
			return fb_batch_give_up(istmt, itra, meta, NULL, NULL);
		}
	}

	/* Si alguna fila sola no entra en el lote, que se ocupe el camino legacy:
	   ahi no hay buffer de batch y no hay limite (y todavia no se agrego nada
	   al batch, asi que se puede abandonar limpiamente). */
	if (has_blob) {
		long r;
		for (r = 0; r < nrows; r++) {
			VALUE row = rb_ary_entry(filas, r);
			if ((size_t) msg_len + fb_batch_row_blob_bytes(row, nparams, f) > FB_BATCH_MAX_ROW_BYTES)
				return fb_batch_give_up(istmt, itra, meta, NULL, NULL);
		}
	}

	/* 3) BPB + batch (el BPB se arma con IXpbBuilder; a mano no funciona) */
	IStatus_init(status);
	pb = IUtil_getXpbBuilder(fb_oo_util, status, IXpbBuilder_BATCH, NULL, 0);
	if (!(IStatus_getState(status) & IStatus_STATE_ERRORS)) {
		IXpbBuilder_insertInt(pb, status, IBatch_TAG_RECORD_COUNTS, 1);
		IXpbBuilder_insertInt(pb, status, IBatch_TAG_BUFFER_BYTES_SIZE, (int) FB_BATCH_BUFFER_BYTES);
		if (has_blob)
			/* IBatch_BLOB_ID_ENGINE: el id lo genera la libreria cliente, y
			   tiene que estar en el BPB antes de crear el batch. */
			IXpbBuilder_insertInt(pb, status, IBatch_TAG_BLOB_POLICY, IBatch_BLOB_ID_ENGINE);
	}
	if (IStatus_getState(status) & IStatus_STATE_ERRORS) {
		IStatus_init(status);
		return fb_batch_give_up(istmt, itra, meta, pb, NULL);
	}

	IStatus_init(status);
	batch = IStatement_createBatch(istmt, status, meta,
								   IXpbBuilder_getBufferLength(pb, status),
								   IXpbBuilder_getBuffer(pb, status));
	if (!batch || (IStatus_getState(status) & IStatus_STATE_ERRORS)) {
		IStatus_init(status);
		return fb_batch_give_up(istmt, itra, meta, pb, NULL);
	}

	/* Los blobs del batch, por defecto, se guardan como "stream" (un solo
	   segmento con todos los bytes); se pide el modo segmentado, que es el
	   mismo que produce isc_create_blob2, para que el resto de los lectores
	   (incluido el isc_get_segment de este driver) sigan viendo blobs
	   normales. Hay que llamarlo antes de agregar mensajes o blobs. */
	if (has_blob) {
		static const unsigned char blob_bpb[] = {
			isc_bpb_version1, isc_bpb_type, 1, isc_bpb_type_segmented
		};
		IStatus_init(status);
		IBatch_setDefaultBpb(batch, status, sizeof(blob_bpb), blob_bpb);
		if (IStatus_getState(status) & IStatus_STATE_ERRORS) {
			IStatus_init(status);
			return fb_batch_give_up(istmt, itra, meta, pb, batch);
		}
	}

	/* 4) empaquetar y mandar */
	buf = (char *) xmalloc((size_t) msg_len * FB_BATCH_ROWS_PER_ADD);
	while (done < nrows) {
		long cnt;
		long k;
		size_t group_bytes = 0;

		/* Cuantas filas entran en este grupo: ni mas de FB_BATCH_ROWS_PER_ADD
		   ni mas bytes de los que admite el buffer del lote (los blobs
		   cuentan). */
		for (cnt = 0; cnt < FB_BATCH_ROWS_PER_ADD && done + cnt < nrows; cnt++) {
			VALUE row = rb_ary_entry(filas, done + cnt);
			size_t row_bytes = (size_t) msg_len + fb_batch_row_blob_bytes(row, nparams, f);
			if (cnt > 0 && group_bytes + row_bytes > FB_BATCH_FLUSH_BYTES)
				break;
			group_bytes += row_bytes;
		}

		/* Los blobs se mandan al empaquetar, no al IBatch_add: si lo pendiente
		   mas este grupo no entra en el buffer, hay que vaciar antes. */
		if (pending_bytes && pending_bytes + group_bytes > FB_BATCH_FLUSH_BYTES) {
			long ok = fb_batch_execute_and_count(batch, itra, status, &i, fail_msg, sizeof(fail_msg));
			if (ok < 0) {
				xfree(buf);
				fb_batch_give_up(istmt, itra, meta, pb, batch);
				rb_raise(rb_eFbError, "batch failed: %s", fail_msg);
			}
			inserted += ok;
			executed_any = 1;
			pending_bytes = 0;
		}

		IStatus_init(status);
		for (k = 0; k < cnt; k++) {
			VALUE row = rb_ary_entry(filas, done + k);
			char *msg = buf + (size_t) k * msg_len;
			if (TYPE(row) != T_ARRAY || RARRAY_LEN(row) != (long) nparams) {
				xfree(buf);
				/* ya se ejecuto parte del lote: no se puede volver al loop */
				if (executed_any) {
					fb_batch_give_up(istmt, itra, meta, pb, batch);
					rb_raise(rb_eFbError, "statement requires %d items; %ld given",
							 (int) nparams, TYPE(row) == T_ARRAY ? RARRAY_LEN(row) : -1L);
				}
				return fb_batch_give_up(istmt, itra, meta, pb, batch);
			}
			memset(msg, 0, msg_len);
			pack_err = fb_batch_pack_row(batch, status, msg, RARRAY_PTR(row), nparams, f, &tms);
			if (pack_err)
				break;
		}
		if (pack_err) {
			xfree(buf);
			if (executed_any) {
				fb_batch_give_up(istmt, itra, meta, pb, batch);
				rb_raise(rb_eFbError, "%s", pack_err);
			}
			return fb_batch_give_up(istmt, itra, meta, pb, batch);
		}

		IStatus_init(status);
		IBatch_add(batch, status, (unsigned) cnt, buf);
		if (IStatus_getState(status) & IStatus_STATE_ERRORS) {
			IStatus *err = status;
			if (!executed_any) {
				IStatus_init(status);
				xfree(buf);
				return fb_batch_give_up(istmt, itra, meta, pb, batch);
			}
			xfree(buf);
			fb_batch_error_check(err);
			fb_batch_give_up(istmt, itra, meta, pb, batch);
			rb_raise(rb_eFbError, "Firebird batch error");
		}
		done += cnt;
		/* lo que ocupa el grupo en el buffer del lote: los mensajes y los
		   blobs (que el servidor tambien cuenta) */
		pending_bytes += group_bytes;
	}

	{
		unsigned fail_pos = 0;
		long ok = fb_batch_execute_and_count(batch, itra, status, &fail_pos, fail_msg, sizeof(fail_msg));
		xfree(buf);
		if (ok < 0) {
			fb_batch_give_up(istmt, itra, meta, pb, batch);
			rb_raise(rb_eFbError, "batch failed: %s", fail_msg);
		}
		inserted += ok;
	}

	fb_batch_give_up(istmt, itra, meta, pb, batch);
	return inserted;
}

#endif /* FB_HAVE_BATCH_API */

static long fb_cursor_execute_withparams(struct FbCursor *fb_cursor, long argc, VALUE *argv)
{
	struct FbConnection *fb_connection;

	TypedData_Get_Struct(fb_cursor->connection, struct FbConnection, &fbconnection_data_type, fb_connection);
	/* Check the first object type of the parameters */
	if (argc >= 1 && TYPE(argv[0]) == T_ARRAY) {
		int i;
		VALUE obj;
		VALUE ary = argv[0];
		if (RARRAY_LEN(ary) > 0 && TYPE(RARRAY_PTR(ary)[0]) == T_ARRAY) {
#ifdef FB_HAVE_BATCH_API
			/* batch de protocolo: los N registros viajan en un solo round trip */
			{
				long batch_rows = fb_cursor_execute_batch(fb_cursor, fb_connection, ary);
				if (batch_rows >= 0)
					return batch_rows;
			}
#endif
			for (i = 0; i < RARRAY_LEN(ary); i++) {
				obj = rb_ary_entry(ary, i);
				fb_cursor_execute_withparams(fb_cursor, 1, &obj);
			}
		} else {
			for (i = 0; i < argc; i++) {
				obj = argv[i];

				/* Set the input parameters */
				Check_Type(obj, T_ARRAY);
				fb_cursor_set_inputparams(fb_cursor, RARRAY_LEN(obj), RARRAY_PTR(obj));

				/* Execute SQL statement */
				fb_stmt_execute2(fb_connection, &fb_cursor->stmt, fb_cursor->i_sqlda, NULL);
				fb_error_check(fb_connection->isc_status);
			}
		}
	} else {
		/* Set the input parameters */
		fb_cursor_set_inputparams(fb_cursor, argc, argv);

		/* Execute SQL statement */
		fb_stmt_execute2(fb_connection, &fb_cursor->stmt, fb_cursor->i_sqlda, NULL);
		fb_error_check(fb_connection->isc_status);
	}

	return -1;	/* no se uso el batch */
}

static VALUE precision_from_sqlvar(XSQLVAR *sqlvar)
{
	switch(sqlvar->sqltype & ~1) {
		case SQL_TEXT:		return Qnil;
		case SQL_VARYING:	return Qnil;
		case SQL_SHORT:
			switch (sqlvar->sqlsubtype) {
				case 0:		return INT2FIX(0);
				case 1:		return INT2FIX(4);
				case 2:		return INT2FIX(4);
			}
			break;
		case SQL_LONG:
			switch (sqlvar->sqlsubtype) {
				case 0:		return INT2FIX(0);
				case 1:		return INT2FIX(9);
				case 2:		return INT2FIX(9);
			}
			break;
		case SQL_FLOAT:		return Qnil;
		case SQL_DOUBLE:
		case SQL_D_FLOAT:
			switch (sqlvar->sqlsubtype) {
				case 0:		return Qnil;
				case 1:		return INT2FIX(15);
				case 2:		return INT2FIX(15);
			}
			break;
		case SQL_TIMESTAMP:	return Qnil;
		case SQL_BLOB:		return Qnil;
		case SQL_ARRAY:		return Qnil;
#if (FB_API_VER >= 30)
		case SQL_BOOLEAN: return Qnil;
#endif
		case SQL_QUAD:		return Qnil;
		case SQL_TYPE_TIME:	return Qnil;
		case SQL_TYPE_DATE:	return Qnil;
		case SQL_INT64:
			switch (sqlvar->sqlsubtype) {
				case 0:		return INT2FIX(0);
				case 1:		return INT2FIX(18);
				case 2:		return INT2FIX(18);
			}
			break;
	}
	return Qnil;
}

static int no_lowercase(VALUE value)
{
	VALUE local_value = StringValue(value);
	int result = rb_funcall(re_lowercase, id_matches, 1, local_value) == Qnil;
	return result;
}

static VALUE fb_cursor_fields_ary(XSQLDA *sqlda, short downcase_names)
{
	long cols;
	long count;
	XSQLVAR *var;
	short dtp;
	VALUE ary;

	cols = sqlda->sqld;
	if (cols == 0) {
		return Qnil;
	}

	ary = rb_ary_new();
	for (count = 0; count < cols; count++) {
		VALUE field;
		VALUE name, type_code, sql_type, sql_subtype, display_size, internal_size, precision, scale, nullable;

		var = &sqlda->sqlvar[count];
		dtp = var->sqltype & ~1;

		if (var->aliasname_length) { /* aliasname always present? */
			name = rb_str_new(var->aliasname, var->aliasname_length);
		} else {
			name = rb_str_new(var->sqlname, var->sqlname_length);
		}
		if (downcase_names && no_lowercase(name)) {
			rb_funcall(name, id_downcase_bang, 0);
		}
		rb_str_freeze(name);
		type_code = INT2NUM((int)(var->sqltype & ~1));
		sql_type = fb_sql_type_from_code(dtp, var->sqlsubtype);
		sql_subtype = INT2FIX(var->sqlsubtype);
		display_size = INT2NUM((int)var->sqllen);
		if (dtp == SQL_VARYING) {
			internal_size = INT2NUM((int)(var->sqllen + sizeof(short)));
		} else {
			internal_size = INT2NUM((int)var->sqllen);
		}
		precision = precision_from_sqlvar(var);
		scale = INT2NUM((int)var->sqlscale);
		nullable = (var->sqltype & 1) ? Qtrue : Qfalse;

		field = rb_struct_new(rb_sFbField, name, sql_type, sql_subtype, display_size, internal_size, precision, scale, nullable, type_code);
		rb_ary_push(ary, field);
	}
	rb_ary_freeze(ary);
	return ary;
}

static VALUE fb_cursor_fields_hash(VALUE fields_ary)
{
	int i;
	VALUE hash = rb_hash_new();

	for (i = 0; i < RARRAY_LEN(fields_ary); i++) {
		VALUE field = rb_ary_entry(fields_ary, i);
		VALUE name = rb_struct_aref(field, LONG2NUM(0));
		rb_hash_aset(hash, name, field);
	}

	return hash;
}

static void fb_cursor_fetch_prep(struct FbCursor *fb_cursor)
{
	struct FbConnection *fb_connection;
	long cols;
	long count;
	XSQLVAR *var;
	short dtp;
	long length;
	long alignment;
	long offset;

	fb_cursor_check(fb_cursor);

	TypedData_Get_Struct(fb_cursor->connection, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_connection_check(fb_connection);

	/* Check if open cursor */
	if (!fb_cursor->open) {
		rb_raise(rb_eFbError, "The cursor has not been opened. Use execute(query)");
	}
	/* Describe output SQLDA */
	isc_dsql_describe(fb_connection->isc_status, &fb_cursor->stmt, 1, fb_cursor->o_sqlda);
	fb_error_check(fb_connection->isc_status);

	/* Set the output SQLDA */
	cols = fb_cursor->o_sqlda->sqld;
	for (var = fb_cursor->o_sqlda->sqlvar, offset = 0, count = 0; count < cols; var++, count++) {
		length = alignment = var->sqllen;
		dtp = var->sqltype & ~1;

		if (dtp == SQL_TEXT) {
			alignment = 1;
		} else if (dtp == SQL_VARYING) {
			length += sizeof(short);
			alignment = sizeof(short);
		}
		offset = FB_ALIGN(offset, alignment);
		var->sqldata = (char*)(fb_cursor->o_buffer + offset);
		offset += length;
		offset = FB_ALIGN(offset, sizeof(short));
		var->sqlind = (short*)(fb_cursor->o_buffer + offset);
		offset += sizeof(short);
	}
}

static VALUE fb_cursor_fetch(struct FbCursor *fb_cursor)
{
	struct FbConnection *fb_connection;
	long cols;
	VALUE ary;
	long count;
	XSQLVAR *var;
	long dtp;
	VALUE val;
	VARY *vary;
	struct tm tms;

	isc_blob_handle blob_handle;
	ISC_QUAD blob_id;
	unsigned short actual_seg_len;
	static char blob_items[] = {
		isc_info_blob_max_segment,
		isc_info_blob_num_segments,
		isc_info_blob_total_length
	};
	char blob_info[32];
	char *p, item;
	short length;
	unsigned short max_segment = 0;
	ISC_LONG num_segments = 0;
	ISC_LONG total_length = 0;

	TypedData_Get_Struct(fb_cursor->connection, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_connection_check(fb_connection);

	if (fb_cursor->eof) {
		rb_raise(rb_eFbError, "Cursor is past end of data.");
	}
	/* Fetch one row */
	if (fb_cursor->exec_procedure) {
		if (fb_cursor->fetched) {
			fb_cursor->eof = Qtrue;
			return Qnil;
		}
		fb_cursor->fetched = 1;
	} else {
		if (fb_stmt_fetch(fb_connection, &fb_cursor->stmt, fb_cursor->o_sqlda) == SQLCODE_NOMORE) {
			fb_cursor->eof = Qtrue;
			return Qnil;
		}
		fb_error_check(fb_connection->isc_status);
	}

	/* Create the result tuple object */
	cols = fb_cursor->o_sqlda->sqld;
	ary = rb_ary_new2(cols);

	/* Create the result objects for each columns */
	for (count = 0; count < cols; count++) {
		var = &fb_cursor->o_sqlda->sqlvar[count];
		dtp = var->sqltype & ~1;

		/* Check if column is null */
		if ((var->sqltype & 1) && (*var->sqlind < 0)) {
			val = Qnil;
		} else {
			/* Set the column value to the result tuple */

			switch (dtp) {
				case SQL_TEXT:
					val = rb_str_new(var->sqldata, var->sqllen);
					#if HAVE_RUBY_ENCODING_H
					if (!NIL_P(fb_connection->encoding)) {
						rb_funcall(val, id_force_encoding, 1, fb_connection->encoding);
					}
					#endif
					break;

				case SQL_VARYING:
					vary = (VARY*)var->sqldata;
					val = rb_str_new(vary->vary_string, vary->vary_length);
					#if HAVE_RUBY_ENCODING_H
					if (!NIL_P(fb_connection->encoding)) {
						rb_funcall(val, id_force_encoding, 1, fb_connection->encoding);
					}
					#endif
					break;

				case SQL_SHORT:
					if (var->sqlscale < 0) {
						val = sql_decimal_to_bigdecimal((long long)*(ISC_SHORT*)var->sqldata, var->sqlscale);
					} else {
						val = INT2NUM((int)*(short*)var->sqldata);
					}
					break;

				case SQL_LONG:
					if (var->sqlscale < 0) {
						val = sql_decimal_to_bigdecimal((long long)*(ISC_LONG*)var->sqldata, var->sqlscale);
					} else {
						val = INT2NUM(*(ISC_LONG*)var->sqldata);
					}
					break;

				case SQL_FLOAT:
					val = rb_float_new((double)*(float*)var->sqldata);
					break;

				case SQL_DOUBLE:
					val = rb_float_new(*(double*)var->sqldata);
					break;

				case SQL_INT64:
					if (var->sqlscale < 0) {
						val = sql_decimal_to_bigdecimal(*(ISC_INT64*)var->sqldata, var->sqlscale);
					} else {
						val = LL2NUM(*(ISC_INT64*)var->sqldata);
					}
					break;

				case SQL_TIMESTAMP:
					isc_decode_timestamp((ISC_TIMESTAMP *)var->sqldata, &tms);
					val = fb_mktime(&tms, "local");
					break;

				case SQL_TYPE_TIME:
					isc_decode_sql_time((ISC_TIME *)var->sqldata, &tms);
					tms.tm_year = 100;
					tms.tm_mon = 0;
					tms.tm_mday = 1;
					val = fb_mktime(&tms, "utc");
					break;

				case SQL_TYPE_DATE:
					isc_decode_sql_date((ISC_DATE *)var->sqldata, &tms);
					val = fb_mkdate(&tms);
					break;

				case SQL_BLOB:
					blob_handle = 0;
					blob_id = *(ISC_QUAD *)var->sqldata;
					isc_open_blob2(fb_connection->isc_status, &fb_connection->db, &fb_connection->transact, &blob_handle, &blob_id, 0, NULL);
					fb_error_check(fb_connection->isc_status);
					isc_blob_info(
						fb_connection->isc_status, &blob_handle,
						sizeof(blob_items), blob_items,
						sizeof(blob_info), blob_info);
					fb_error_check(fb_connection->isc_status);
					for (p = blob_info; *p != isc_info_end; p += length) {
						item = *p++;
						length = (short) isc_vax_integer(p,2);
						p += 2;
						switch (item) {
							case isc_info_blob_max_segment:
								max_segment = isc_vax_integer(p,length);
								break;
							case isc_info_blob_num_segments:
								num_segments = isc_vax_integer(p,length);
								break;
							case isc_info_blob_total_length:
								total_length = isc_vax_integer(p,length);
								break;
						}
					}
					val = rb_str_new(NULL,total_length);
					for (p = RSTRING_PTR(val); num_segments > 0; num_segments--, p += actual_seg_len) {
						isc_get_segment(fb_connection->isc_status, &blob_handle, &actual_seg_len, max_segment, p);
						fb_error_check(fb_connection->isc_status);
					}
					#if HAVE_RUBY_ENCODING_H
					if (!NIL_P(fb_connection->encoding)) {
						rb_funcall(val, id_force_encoding, 1, fb_connection->encoding);
					}
					#endif
					isc_close_blob(fb_connection->isc_status, &blob_handle);
					fb_error_check(fb_connection->isc_status);
					break;

				case SQL_ARRAY:
					rb_warn("ARRAY not supported (yet)");
					val = Qnil;
					break;

				case SQL_INT128: {
					VALUE unscaled = fb_int128_read(var->sqldata);
					if (var->sqlscale < 0) {
						/* NUMERIC/DECIMAL de hasta 38 digitos -> BigDecimal */
						VALUE div = rb_funcall(INT2FIX(10), rb_intern("**"), 1, INT2FIX(-var->sqlscale));
						val = rb_funcall(rb_funcall(rb_cObject, rb_intern("BigDecimal"), 1, unscaled),
										 rb_intern("/"), 1, div);
					} else {
						val = unscaled;
					}
					break;
				}

				case SQL_BOOLEAN:
					val = (*(bool*)var->sqldata) ? Qtrue : Qfalse;
					break;

				case SQL_TIMESTAMP_TZ:
				case SQL_TIMESTAMP_TZ_EX:
					/* En los tipos con zona el servidor manda el instante en UTC
					   (la zona va aparte, en el mismo campo de 12 bytes), asi que
					   se construye el Time en UTC: ponerlo como local desplazaba
					   el instante por el offset local. */
					isc_decode_timestamp((ISC_TIMESTAMP *)var->sqldata, &tms);
					val = fb_mktime(&tms, "utc");
					break;

				case SQL_TIME_TZ:
				case SQL_TIME_TZ_EX:
					isc_decode_sql_time((ISC_TIME *)var->sqldata, &tms);
					tms.tm_year = 100;
					tms.tm_mon = 0;
					tms.tm_mday = 1;
					val = fb_mktime(&tms, "utc");
					break;

				case SQL_DEC16:
				case SQL_DEC34:
					rb_warn("DECFLOAT not supported (yet)");
					val = Qnil;
					break;

				default:
					rb_raise(rb_eFbError, "Specified table includes unsupported datatype (%ld)", dtp);
					break;
			}
		}
		rb_ary_push(ary, val);
	}

	return ary;
}

static long cursor_rows_affected(struct FbCursor *fb_cursor, long statement_type)
{
	long inserted = 0, selected = 0, updated = 0, deleted = 0;
	char request[] = { isc_info_sql_records };
	char response[64], *r;
	ISC_STATUS isc_status[20];

	isc_dsql_sql_info(isc_status, &fb_cursor->stmt, sizeof(request), request, sizeof(response), response);
	fb_error_check(isc_status);
	if (response[0] != isc_info_sql_records) { return -1; }

	r = response + 3; /* skip past first cluster */
	while (*r != isc_info_end) {
		char count_type = *r++;
		short len = isc_vax_integer(r, sizeof(short));
		r += sizeof(short);
		switch (count_type) {
			case isc_info_req_insert_count:
				inserted = isc_vax_integer(r, len);
				break;
			case isc_info_req_select_count:
				selected = isc_vax_integer(r, len);
				break;
			case isc_info_req_update_count:
				updated = isc_vax_integer(r, len);
				break;
			case isc_info_req_delete_count:
				deleted = isc_vax_integer(r, len);
				break;
		}
		r += len;
	}
	switch (statement_type) {
		case isc_info_sql_stmt_select: return selected;
		case isc_info_sql_stmt_insert: return inserted;
		case isc_info_sql_stmt_update: return updated;
		case isc_info_sql_stmt_delete: return deleted;
		default: return inserted + selected + updated + deleted;
	}
}

/* call-seq:
 *   execute2(sql, *args) -> nil or rows affected
 *
 * This function is not published.
 */
static VALUE cursor_execute2(VALUE args)
{
	struct FbCursor *fb_cursor;
	struct FbConnection *fb_connection;
	char *sql;
	VALUE rb_sql;
	long statement;
	long length;
	long in_params;
	long cols;
	long rows_affected;
	VALUE result = Qnil;
	char isc_info_buff[16];
	char isc_info_stmt[] = { isc_info_sql_stmt_type };
	struct FbStmtCache *cache_entry;

	VALUE self = rb_ary_pop(args);
	TypedData_Get_Struct(self, struct FbCursor, &fbcursor_data_type, fb_cursor);
	TypedData_Get_Struct(fb_cursor->connection, struct FbConnection, &fbconnection_data_type, fb_connection);

	rb_sql = rb_ary_shift(args);
	sql = StringValuePtr(rb_sql);

	/* Cache: si este SQL ya esta preparado y libre, se reutiliza (ahorra
	   prepare + describe_bind + describe, tres idas y vueltas al servidor). */
	cache_entry = fb_stmt_cache_find(fb_connection, sql);
	if (cache_entry) {
		/* la sentencia recien asignada a este cursor no se usa */
		isc_dsql_free_statement(fb_connection->isc_status, &fb_cursor->stmt, DSQL_drop);
		fb_error_check_warn(fb_connection->isc_status);
		xfree(fb_cursor->i_sqlda);
		xfree(fb_cursor->o_sqlda);
		fb_cursor->stmt = cache_entry->stmt;
		fb_cursor->i_sqlda = cache_entry->i_sqlda;
		fb_cursor->o_sqlda = cache_entry->o_sqlda;
		fb_cursor->cache = cache_entry;
		cache_entry->in_use = 1;
		fb_cursor_pin(fb_connection, self);
		statement = cache_entry->statement_type;
	} else {
		/* Prepare query */
		isc_dsql_prepare(fb_connection->isc_status, &fb_connection->transact, &fb_cursor->stmt, 0, sql, fb_connection_dialect(fb_connection), fb_cursor->o_sqlda);
		fb_error_check(fb_connection->isc_status);

		/* Get the statement type */
		isc_dsql_sql_info(fb_connection->isc_status, &fb_cursor->stmt,
				sizeof(isc_info_stmt), isc_info_stmt,
				sizeof(isc_info_buff), isc_info_buff);
		fb_error_check(fb_connection->isc_status);

		if (isc_info_buff[0] == isc_info_sql_stmt_type) {
			length = isc_vax_integer(&isc_info_buff[1], 2);
			statement = isc_vax_integer(&isc_info_buff[3], (short)length);
		} else {
			statement = 0;
		}
		/* Describe the parameters */
		isc_dsql_describe_bind(fb_connection->isc_status, &fb_cursor->stmt, 1, fb_cursor->i_sqlda);
		fb_error_check(fb_connection->isc_status);

		isc_dsql_describe(fb_connection->isc_status, &fb_cursor->stmt, 1, fb_cursor->o_sqlda);
		fb_error_check(fb_connection->isc_status);

		/* Get the number of parameters and reallocate the SQLDA */
		in_params = fb_cursor->i_sqlda->sqld;
		if (fb_cursor->i_sqlda->sqln < in_params) {
			xfree(fb_cursor->i_sqlda);
			fb_cursor->i_sqlda = sqlda_alloc(in_params);
			/* Describe again */
			isc_dsql_describe_bind(fb_connection->isc_status, &fb_cursor->stmt, 1, fb_cursor->i_sqlda);
			fb_error_check(fb_connection->isc_status);
		}

		/* El SQLDA de salida tiene que tener ya su tamano definitivo ANTES de
		   entrar en la cache: la entrada guarda el puntero a los dos SQLDA
		   (fb_stmt_cache_add) y, si despues se reemplazara por uno mayor, la
		   entrada quedaria apuntando a memoria liberada. Con mas de
		   SQLDA_COLSINIT columnas eso daba, en la segunda ejecucion del mismo
		   SQL, un resultado incorrecto (query devolvia 0 en lugar de las filas)
		   o directamente la caida del proceso (doble free / desbordamiento). */
		cols = fb_cursor->o_sqlda->sqld;
		if (fb_cursor->o_sqlda->sqln < cols) {
			xfree(fb_cursor->o_sqlda);
			fb_cursor->o_sqlda = sqlda_alloc(cols);
			/* Describe again */
			isc_dsql_describe(fb_connection->isc_status, &fb_cursor->stmt, 1, fb_cursor->o_sqlda);
			fb_error_check(fb_connection->isc_status);
		}

		/* Registrar en la cache. El DDL invalida lo cacheado; si la cache esta
		   llena y todo en uso, fb_cursor->cache queda NULL y este cursor es
		   dueno de su sentencia, exactamente como antes. */
		if (statement == isc_info_sql_stmt_ddl)
			fb_stmt_cache_invalidate(fb_connection);
		else {
			fb_cursor->cache = fb_stmt_cache_add(fb_connection, fb_cursor, sql, statement, self);
			if (fb_cursor->cache)
				fb_cursor_pin(fb_connection, self);
		}
	}

	/* siempre, tambien en el camino de cache (antes quedaba sin inicializar) */
	in_params = fb_cursor->i_sqlda->sqld;

	/* El timeout se aplica siempre: la sentencia cacheada pudo prepararse antes
	   de que se cambiara command_timeout. */
#ifdef HAVE_FB_DSQL_SET_TIMEOUT
	if (fb_connection->command_timeout_ms > 0) {
		fb_dsql_set_timeout(fb_connection->isc_status, &fb_cursor->stmt,
							(ISC_ULONG) fb_connection->command_timeout_ms);
		fb_error_check(fb_connection->isc_status);
	}
#endif

    /* Get the size of parameters buffer and reallocate it */
	if (in_params) {
		length = calculate_buffsize(fb_cursor->i_sqlda);
		if (length > fb_cursor->i_buffer_size) {
			fb_cursor->i_buffer = xrealloc(fb_cursor->i_buffer, length);
			memset(fb_cursor->i_buffer, 0, length);
			fb_cursor->i_buffer_size = length;
		}
	}

    /* Execute the SQL statement if it is not query */
	if (!fb_cursor->o_sqlda->sqld) {
		if (statement == isc_info_sql_stmt_start_trans) {
			rb_raise(rb_eFbError, "use Fb::Connection#transaction()");
		} else if (statement == isc_info_sql_stmt_commit) {
			rb_raise(rb_eFbError, "use Fb::Connection#commit()");
		} else if (statement == isc_info_sql_stmt_rollback) {
			rb_raise(rb_eFbError, "use Fb::Connection#rollback()");
		} else if (in_params) {
			long batch_rows = fb_cursor_execute_withparams(fb_cursor, RARRAY_LEN(args), RARRAY_PTR(args));
			if (batch_rows >= 0) {
				/* el batch ya conto las filas afectadas */
				result = INT2NUM((int) batch_rows);
				return result;
			}
		} else {
			fb_stmt_execute2(fb_connection, &fb_cursor->stmt, NULL, NULL);
			fb_error_check(fb_connection->isc_status);
		}
		rows_affected = cursor_rows_affected(fb_cursor, statement);
		result = INT2NUM((int)rows_affected);
	} else {
		/* Open cursor if the SQL statement is query */
		/* Get the number of columns and reallocate the SQLDA. Ya se hizo al
		   preparar (antes de registrar en la cache); esto queda como red de
		   seguridad por si un SQLDA cacheado viniera corto. */
		cols = fb_cursor->o_sqlda->sqld;
		if (fb_cursor->o_sqlda->sqln < cols) {
			xfree(fb_cursor->o_sqlda);
			fb_cursor->o_sqlda = sqlda_alloc(cols);
			/* Describe again */
			isc_dsql_describe(fb_connection->isc_status, &fb_cursor->stmt, 1, fb_cursor->o_sqlda);
			fb_error_check(fb_connection->isc_status);
			/* Si el SQLDA venia de la cache, la entrada lo comparte: hay que
			   dejarla apuntando al nuevo, no al liberado. */
			if (fb_cursor->cache)
				fb_cursor->cache->o_sqlda = fb_cursor->o_sqlda;
		}

		if (in_params) {
			fb_cursor_set_inputparams(fb_cursor, RARRAY_LEN(args), RARRAY_PTR(args));
		}

		int is_query = (statement == isc_info_sql_stmt_select || statement == isc_info_sql_stmt_select_for_upd);
		fb_cursor->exec_procedure = !is_query;
		fb_cursor->fetched = 0;

		if (fb_cursor->exec_procedure) {
			/* Allocate output buffer first, as isc_dsql_execute2 will write to it immediately */
			length = calculate_buffsize(fb_cursor->o_sqlda);
			if (length > fb_cursor->o_buffer_size) {
				fb_cursor->o_buffer = xrealloc(fb_cursor->o_buffer, length);
				fb_cursor->o_buffer_size = length;
			}
			/* Set output sqldata/sqlind pointers */
			fb_cursor->open = Qtrue; /* Temporarily set to true so fetch_prep doesn't raise error */
			fb_cursor_fetch_prep(fb_cursor);

			/* Execute and retrieve output parameters immediately */
			fb_stmt_execute2(fb_connection, &fb_cursor->stmt, in_params ? fb_cursor->i_sqlda : NULL, fb_cursor->o_sqlda);
			fb_error_check(fb_connection->isc_status);
		} else {
			/* Open cursor */
			fb_stmt_execute2(fb_connection, &fb_cursor->stmt, in_params ? fb_cursor->i_sqlda : NULL, NULL);
			fb_error_check(fb_connection->isc_status);
			fb_cursor->open = Qtrue;

			/* Get the size of results buffer and reallocate it */
			length = calculate_buffsize(fb_cursor->o_sqlda);
			if (length > fb_cursor->o_buffer_size) {
				fb_cursor->o_buffer = xrealloc(fb_cursor->o_buffer, length);
				fb_cursor->o_buffer_size = length;
			}
		}

		/* Set the description attributes */
		fb_cursor->fields_ary = fb_cursor_fields_ary(fb_cursor->o_sqlda, fb_connection->downcase_names);
		fb_cursor->fields_hash = fb_cursor_fields_hash(fb_cursor->fields_ary);
	}
	return result;
}

/* call-seq:
 *   execute(sql, *args) -> nil or rows affected
 *
 * This function is no longer published.
 */
static VALUE cursor_execute(int argc, VALUE* argv, VALUE self)
{
	struct FbCursor *fb_cursor;
	struct FbConnection *fb_connection;
	VALUE args;

	if (argc < 1) {
		rb_raise(rb_eArgError, "At least 1 argument required.");
	}

	args = rb_ary_new4(argc, argv);
	rb_ary_push(args, self);

	TypedData_Get_Struct(self, struct FbCursor, &fbcursor_data_type, fb_cursor);
	TypedData_Get_Struct(fb_cursor->connection, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_connection_check(fb_connection);

	if (fb_cursor->open) {
		isc_dsql_free_statement(fb_connection->isc_status, &fb_cursor->stmt, DSQL_close);
		fb_error_check(fb_connection->isc_status);
		fb_cursor->open = Qfalse;
	}

	if (!fb_connection->transact) {
		VALUE result;
		int state;

		fb_connection_transaction_start(fb_connection, Qnil);
		fb_cursor->auto_transact = fb_connection->transact;

		result = rb_protect(cursor_execute2, args, &state);
		if (state) {
			fb_connection_rollback(fb_connection);
			return rb_funcall(rb_mKernel, rb_intern("raise"), 0);
		} else if (result != Qnil) {
			fb_connection_commit(fb_connection);
			return result;
		} else {
			return result;
		}
	} else {
		return cursor_execute2(args);
	}
}

static VALUE fb_hash_from_ary(VALUE fields, VALUE row)
{
	VALUE hash = rb_hash_new();
	int i;
	for (i = 0; i < RARRAY_LEN(fields); i++) {
		VALUE field = rb_ary_entry(fields, i);
		VALUE name = rb_struct_aref(field, LONG2NUM(0));
		VALUE v = rb_ary_entry(row, i);
		rb_hash_aset(hash, name, v);
	}
	return hash;
}

static int hash_format(int argc, VALUE *argv)
{
	if (argc == 0 || argv[0] == ID2SYM(rb_intern("array"))) {
		return 0;
	} else if (argv[0] == ID2SYM(rb_intern("hash"))) {
		return 1;
	} else {
		rb_raise(rb_eFbError, "Unknown format");
	}
}

/* call-seq:
 *   fetch() -> Array
 *   fetch(:array) -> Array
 *   fetch(:hash) -> Hash
 *
 * Reads and returns a single row from the open cursor in either an Array or a Hash,
 * where the column names or aliases from the query form the keys.
 * If the +downcase_names+ attribute of the associated connection evaluates to true,
 * the keys are lower case, except where the column name was mixed case to begin with.
 */
static VALUE cursor_fetch(int argc, VALUE* argv, VALUE self)
{
	VALUE ary;
	struct FbCursor *fb_cursor;

	int hash_row = hash_format(argc, argv);

	TypedData_Get_Struct(self, struct FbCursor, &fbcursor_data_type, fb_cursor);
	fb_cursor_fetch_prep(fb_cursor);

	ary = fb_cursor_fetch(fb_cursor);
	if (NIL_P(ary)) return Qnil;
	return hash_row ? fb_hash_from_ary(fb_cursor->fields_ary, ary) : ary;
}

/* call-seq:
 *   fetchall() -> Array of Arrays
 *   fetchall(:array) -> Array of Arrays
 *   fetchall(:hash) -> Array of Hashes
 *
 * Returns the remainder of the rows from the open cursor, with each row represented
 * by either an Array or a Hash, where the column names or aliases from the query form the keys.
 * If the +downcase_names+ attribute of the associated connection evaluates to true,
 * the keys are lower case, except where the column name was mixed case to begin with.
 */
static VALUE cursor_fetchall(int argc, VALUE* argv, VALUE self)
{
	VALUE ary, row;
	struct FbCursor *fb_cursor;

	int hash_rows = hash_format(argc, argv);

	TypedData_Get_Struct(self, struct FbCursor, &fbcursor_data_type, fb_cursor);
	fb_cursor_fetch_prep(fb_cursor);

	ary = rb_ary_new();
	for (;;) {
		row = fb_cursor_fetch(fb_cursor);
		if (NIL_P(row)) break;
		if (hash_rows) {
			rb_ary_push(ary, fb_hash_from_ary(fb_cursor->fields_ary, row));
		} else {
			rb_ary_push(ary, row);
		}
	}

	return ary;
}

/* call-seq:
 *   each() {|Array| } -> nil
 *   each(:array) {|Array| } -> nil
 *   each(:hash) {|Hash| } -> nil
 *
 * Iterates the rows from the open cursor, passing each one to a block in either
 * an Array or a Hash, where the column names or aliases from the query form the keys.
 * If the +downcase_names+ attribute of the associated connection evaluates to true,
 * the keys are lower case, except where the column name was mixed case to begin with.
 */
static VALUE cursor_each(int argc, VALUE* argv, VALUE self)
{
	VALUE row;
	struct FbCursor *fb_cursor;

	int hash_rows = hash_format(argc, argv);

	TypedData_Get_Struct(self, struct FbCursor, &fbcursor_data_type, fb_cursor);
	fb_cursor_fetch_prep(fb_cursor);

	for (;;) {
		row = fb_cursor_fetch(fb_cursor);
		if (NIL_P(row)) break;
		if (hash_rows) {
			rb_yield(fb_hash_from_ary(fb_cursor->fields_ary, row));
		} else {
			rb_yield(row);
		}
	}

	return Qnil;
}

/* call-seq:
 *   close(sql, *args) -> nil
 *
 * Closes the cursor.  If a transaction was automatically started for this cursor, the transaction is commited.
 */
static VALUE cursor_close(VALUE self)
{
	struct FbCursor *fb_cursor;
	struct FbConnection *fb_connection;

	TypedData_Get_Struct(self, struct FbCursor, &fbcursor_data_type, fb_cursor);
	TypedData_Get_Struct(fb_cursor->connection, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_cursor_check(fb_cursor);

	/* Close the cursor */
	if (fb_cursor->stmt) {
		/* Si este cursor es el dueno de la entrada (su campo stmt es la direccion
		   que Firebird se guardo para esa sentencia), tiene que seguir anclado
		   hasta que la entrada se destruya: ver fb_stmt_cache_destroy. */
		int keep_pin = (fb_cursor->cache != NULL &&
			((struct FbStmtCache *) fb_cursor->cache)->owner == self);
		isc_dsql_free_statement(fb_connection->isc_status, &fb_cursor->stmt, DSQL_close);
		fb_error_check_warn(fb_connection->isc_status);
		if (fb_cursor->cache) {
			/* la sentencia queda en la cache para reutilizarla */
			fb_stmt_cache_release(fb_connection, fb_cursor);
		} else {
			isc_dsql_free_statement(fb_connection->isc_status, &fb_cursor->stmt, DSQL_drop);
			fb_error_check(fb_connection->isc_status);
			/* handle muerto: que nadie lo vuelva a dropear (un segundo drop escribe
			   sobre memoria ya liberada por el cliente -> segfault en el GC) */
			fb_cursor->stmt = 0;
		}
		if (!keep_pin)
			fb_cursor_unpin(fb_connection, self);
		fb_cursor->open = Qfalse;
		/* Solo si hay transaccion activa Y la abrio este cursor: si otro cursor
		   de la misma transaccion automatica ya la comiteo, transact == 0 y
		   comitear de nuevo fallaba con "invalid transaction handle". */
		if (fb_connection->transact && fb_connection->transact == fb_cursor->auto_transact) {
			isc_commit_transaction(fb_connection->isc_status, &fb_connection->transact);
			fb_cursor->auto_transact = fb_connection->transact;
			fb_error_check(fb_connection->isc_status);
		}
	}
	fb_cursor->fields_ary = Qnil;
	fb_cursor->fields_hash = Qnil;
	return Qnil;
}

/* call-seq:
 *   drop() -> nil
 *
 * Drops the cursor.
 *
 * TODO: How is this different from close()?
 */
static VALUE cursor_drop(VALUE self)
{
	struct FbCursor *fb_cursor;
	struct FbConnection *fb_connection;
	int i;

	TypedData_Get_Struct(self, struct FbCursor, &fbcursor_data_type, fb_cursor);
	fb_cursor_drop(fb_cursor);
	fb_cursor->fields_ary = Qnil;
	fb_cursor->fields_hash = Qnil;

	/* reset the reference from connection */
	TypedData_Get_Struct(fb_cursor->connection, struct FbConnection, &fbconnection_data_type, fb_connection);
	for (i = 0; i < RARRAY_LEN(fb_connection->cursor); i++) {
		if (RARRAY_PTR(fb_connection->cursor)[i] == self) {
			RARRAY_PTR(fb_connection->cursor)[i] = Qnil;
		}
	}

	return Qnil;
}

/* call-seq:
 *   fields() -> Array
 *   fields(:array) -> Array
 *   fields(:hash) -> Hash
 *
 * Return an array of Field Structs or a hash indexed by field name.
 * If the +downcase_names+ attribute of the associated connection evaluates to true,
 * the keys are lower case, except where the column name was mixed case to begin with.
 */
static VALUE cursor_fields(int argc, VALUE* argv, VALUE self)
{
	struct FbCursor *fb_cursor;

	TypedData_Get_Struct(self, struct FbCursor, &fbcursor_data_type, fb_cursor);
	if (argc == 0 || argv[0] == ID2SYM(rb_intern("array"))) {
		return fb_cursor->fields_ary;
	} else if (argv[0] == ID2SYM(rb_intern("hash"))) {
		return fb_cursor->fields_hash;
	} else {
		rb_raise(rb_eFbError, "Unknown format");
	}
}

/* call-seq:
 *   error_code -> int
 *
 * Returns the sqlcode associated with the error.
 */
static VALUE error_error_code(VALUE error)
{
	return rb_iv_get(error, "error_code");
}

static char* dbp_create(long *length)
{
	char *dbp = ALLOC_N(char, 1);
	*dbp = isc_dpb_version1;
	*length = 1;
	return dbp;
}

static char* dbp_add_string(char *dbp, char isc_dbp_code, char *s, long *length)
{
	char *buf;
	long old_length = *length;
	long s_len = strlen(s);
	*length += 2 + s_len;
	REALLOC_N(dbp, char, *length);
	buf = dbp + old_length;
	*buf++ = isc_dbp_code;
	*buf++ = (char)s_len;
	memcpy(buf, s, s_len);
	return dbp;
}

static char* connection_create_dbp(VALUE self, long *length)
{
	char *dbp;
	VALUE username, password, charset, role;

	username = rb_iv_get(self, "@username");
	Check_Type(username, T_STRING);
	password = rb_iv_get(self, "@password");
	Check_Type(password, T_STRING);
	role = rb_iv_get(self, "@role");
	charset = rb_iv_get(self, "@charset");

	dbp = dbp_create(length);
	dbp = dbp_add_string(dbp, isc_dpb_user_name, StringValuePtr(username), length);
	dbp = dbp_add_string(dbp, isc_dpb_password, StringValuePtr(password), length);
	if (!NIL_P(charset)) {
		dbp = dbp_add_string(dbp, isc_dpb_lc_ctype, StringValuePtr(charset), length);
	}
	if (!NIL_P(role)) {
		dbp = dbp_add_string(dbp, isc_dpb_sql_role_name, StringValuePtr(role), length);
	}
	return dbp;
}

static const char* CONNECTION_PARMS[] = {
	"@database",
	"@username",
	"@password",
	"@charset",
	"@role",
	"@downcase_names",
	"@encoding",
	(char *)0
};

static VALUE connection_create(isc_db_handle handle, VALUE db)
{
	unsigned short dialect;
	unsigned short db_dialect;
	VALUE downcase_names;
	const char *parm;
	int i;
	struct FbConnection *fb_connection;
    VALUE connection = TypedData_Make_Struct(rb_cFbConnection, struct FbConnection, &fbconnection_data_type, fb_connection);
	fb_connection->db = handle;
	fb_connection->transact = 0;
	fb_connection->cursor = rb_ary_new();
	fb_connection->cached_cursors = rb_ary_new();
	dialect = SQL_DIALECT_CURRENT;
	db_dialect = fb_connection_db_SQL_Dialect(fb_connection);

	if (db_dialect < dialect) {
		dialect = db_dialect;
		/* TODO: downgrade warning */
	}

	fb_connection->dialect = dialect;
	fb_connection->db_dialect = db_dialect;
	downcase_names = rb_iv_get(db, "@downcase_names");
	fb_connection->downcase_names = RTEST(downcase_names);
	fb_connection->encoding = rb_iv_get(db, "@encoding");
	{
		VALUE timeout = rb_iv_get(db, "@command_timeout");
		fb_connection->command_timeout_ms = NIL_P(timeout) ? 0 : (long) (NUM2DBL(timeout) * 1000.0);
	}

	for (i = 0; (parm = CONNECTION_PARMS[i]); i++) {
		rb_iv_set(connection, parm, rb_iv_get(db, parm));
	}

	return connection;
}

static VALUE connection_names(VALUE self, const char *sql)
{
	VALUE row;
	VALUE query = rb_str_new2(sql);
	VALUE cursor = connection_execute(1, &query, self);
	VALUE names = rb_ary_new();
	struct FbConnection *fb_connection;
	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);

	while ((row = cursor_fetch(0, NULL, cursor)) != Qnil) {
		VALUE name = rb_ary_entry(row, 0);
		if (fb_connection->downcase_names && no_lowercase(name)) {
			rb_funcall(name, id_downcase_bang, 0);
		}
		rb_funcall(name, id_rstrip_bang, 0);
		rb_ary_push(names, name);
	}

	cursor_close(cursor);
	return names;
}

/* call-seq:
 *   table_names() -> array
 *
 * Returns sorted array of table names in connected database.
 */
static VALUE connection_table_names(VALUE self)
{
	const char *sql = "SELECT RDB$RELATION_NAME FROM RDB$RELATIONS "
				"WHERE (RDB$SYSTEM_FLAG <> 1 OR RDB$SYSTEM_FLAG IS NULL) AND RDB$VIEW_BLR IS NULL "
				"ORDER BY RDB$RELATION_NAME";
	return connection_names(self, sql);
}

/* call-seq:
 *   generator_names() -> array
 *
 * Returns sorted array of generator names in connected database.
 */
static VALUE connection_generator_names(VALUE self)
{
	const char *sql = "SELECT RDB$GENERATOR_NAME FROM RDB$GENERATORS "
				"WHERE (RDB$SYSTEM_FLAG IS NULL OR RDB$SYSTEM_FLAG <> 1) "
				"ORDER BY RDB$GENERATOR_NAME";
	return connection_names(self, sql);
}

/* call-seq:
 *   view_names() -> array
 *
 * Returns sorted array of view names in connected database.
 */
static VALUE connection_view_names(VALUE self)
{
	const char *sql = "SELECT RDB$RELATION_NAME, RDB$OWNER_NAME, RDB$VIEW_SOURCE FROM RDB$RELATIONS "
				"WHERE (RDB$SYSTEM_FLAG <> 1 OR RDB$SYSTEM_FLAG IS NULL) AND NOT RDB$VIEW_BLR IS NULL AND RDB$FLAGS = 1 "
				"ORDER BY RDB$RELATION_ID";
	return connection_names(self, sql);
}

/* call-seq:
 *   role_names() -> array
 *
 * Returns sorted array of role names in connected database.
 */
static VALUE connection_role_names(VALUE self)
{
	const char *sql = "SELECT * FROM RDB$ROLES WHERE RDB$SYSTEM_FLAG = 0 ORDER BY RDB$ROLE_NAME";
	return connection_names(self, sql);
}

/* call-seq:
 *   procedure_names() -> array
 *
 * Returns sorted array of stored procedure names in connected database.
 */
static VALUE connection_procedure_names(VALUE self)
{
	const char *sql = "SELECT RDB$PROCEDURE_NAME FROM RDB$PROCEDURES "
				"ORDER BY RDB$PROCEDURE_NAME";
	return connection_names(self, sql);
}

/* call-seq:
 *   trigger_names() -> array
 *
 * Returns sorted array of trigger names in connected database.
 */
static VALUE connection_trigger_names(VALUE self)
{
	const char *sql = "SELECT RDB$TRIGGER_NAME FROM RDB$TRIGGERS "
				"ORDER BY RDB$TRIGGER_NAME";
	return connection_names(self, sql);
}

/* call-seq:
 *   columns(table_name) -> array
 *
 * Returns array of objects describing each column of table_name.
 */

static VALUE connection_columns(VALUE self, VALUE table_name)
{
    int i;
    struct FbConnection *fb_connection;
    VALUE re_default = rb_reg_new("^\\s*DEFAULT\\s+", strlen("^\\s*DEFAULT\\s+"), IGNORECASE);
    VALUE re_rdb = rb_reg_new("^RDB\\$", strlen("^RDB\\$"), 0);
    VALUE empty = rb_str_new(NULL, 0);
    VALUE columns = rb_ary_new();
    const char *sql = "SELECT r.rdb$field_name NAME, r.rdb$field_source, f.rdb$field_type, f.rdb$field_sub_type, "
                "f.rdb$field_length, f.rdb$field_precision, f.rdb$field_scale SCALE, "
                "COALESCE(r.rdb$default_source, f.rdb$default_source), "
                "COALESCE(r.rdb$null_flag, f.rdb$null_flag) "
                "FROM rdb$relation_fields r "
                "JOIN rdb$fields f ON r.rdb$field_source = f.rdb$field_name "
                "WHERE UPPER(r.rdb$relation_name) = ? "
                "ORDER BY r.rdb$field_position";
    VALUE query = rb_str_new2(sql);
    VALUE upcase_table_name = rb_funcall(table_name, rb_intern("upcase"), 0);
    VALUE query_parms[] = { query, upcase_table_name };
    VALUE rs = connection_query(2, query_parms, self);
	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);
    for (i = 0; i < RARRAY_LEN(rs); i++) {
        VALUE row = rb_ary_entry(rs, i);
        VALUE name = rb_ary_entry(row, 0);
        VALUE domain = rb_ary_entry(row, 1);
        VALUE sql_type = rb_ary_entry(row, 2);
        VALUE sql_subtype = rb_ary_entry(row, 3);
        VALUE length = rb_ary_entry(row, 4);
        VALUE precision = rb_ary_entry(row, 5);
        VALUE scale = rb_ary_entry(row, 6);
        VALUE dflt = rb_ary_entry(row, 7);
        VALUE not_null = rb_ary_entry(row, 8);
        VALUE nullable;
        VALUE column;
        rb_funcall(name, id_rstrip_bang, 0);
        rb_funcall(domain, id_rstrip_bang, 0);
        if (fb_connection->downcase_names && no_lowercase(name)) {
          rb_funcall(name, id_downcase_bang, 0);
        }
        if (rb_funcall(re_rdb, rb_intern("match"), 1, domain) != Qnil) {
            domain = Qnil;
        }
        if (sql_subtype == Qnil) {
            sql_subtype = INT2NUM(0);
        }
        sql_type = sql_type_from_code(self, sql_type, sql_subtype);
        if (dflt != Qnil) {
            rb_funcall(dflt, id_sub_bang, 2, re_default, empty);
        }
        nullable = RTEST(not_null) ? Qfalse : Qtrue;
        column = rb_struct_new(rb_sFbColumn, name, domain, sql_type, sql_subtype, length, precision, scale, dflt, nullable);
        rb_ary_push(columns, column);
    }
    rb_ary_freeze(columns);
    return columns;
}

char *p(char *prompt, VALUE s)
{
	char *sz;
	if (TYPE(s) != T_STRING) {
		s = rb_funcall(s, rb_intern("to_s"), 0);
	}
	sz = StringValuePtr(s);
	printf("%s: %s\n", prompt, sz);
	return sz;
}

static VALUE connection_index_columns(VALUE self, VALUE index_name)
{
	const char *sql_columns = "SELECT * "
						"FROM RDB$INDEX_SEGMENTS "
						"WHERE RDB$INDEX_SEGMENTS.RDB$INDEX_NAME = ? "
						"ORDER BY RDB$INDEX_SEGMENTS.RDB$FIELD_POSITION";
	VALUE query_columns = rb_str_new2(sql_columns);
	VALUE query_parms[] = { query_columns, index_name };
	VALUE result = connection_query(2, query_parms, self);
	VALUE columns = rb_ary_new();
	int i;
	struct FbConnection *fb_connection;
	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);

	for (i = 0; i < RARRAY_LEN(result); i++) {
		VALUE row = rb_ary_entry(result, i);
		VALUE name = rb_ary_entry(row, 1);
		rb_funcall(name, id_rstrip_bang, 0);
		if (fb_connection->downcase_names && no_lowercase(name)) {
			rb_funcall(name, id_downcase_bang, 0);
		}
		rb_ary_push(columns, name);
	}
	return columns;
}

/* call-seq:
 *   indexes() -> Hash
 *
 * Returns a hash of indexes, keyed by index name.
 */
static VALUE connection_indexes(VALUE self)
{
	const char *sql_indexes = "SELECT RDB$INDICES.RDB$RELATION_NAME, RDB$INDICES.RDB$INDEX_NAME, RDB$INDICES.RDB$UNIQUE_FLAG, RDB$INDICES.RDB$INDEX_TYPE "
						"FROM RDB$INDICES "
						"  JOIN RDB$RELATIONS ON RDB$INDICES.RDB$RELATION_NAME = RDB$RELATIONS.RDB$RELATION_NAME "
						"WHERE (RDB$RELATIONS.RDB$SYSTEM_FLAG <> 1 OR RDB$RELATIONS.RDB$SYSTEM_FLAG IS NULL) ";
	VALUE query_indexes = rb_str_new2(sql_indexes);
	VALUE ary_indexes = connection_query(1, &query_indexes, self);
	VALUE indexes = rb_hash_new();
	int i;
	struct FbConnection *fb_connection;
	TypedData_Get_Struct(self, struct FbConnection, &fbconnection_data_type, fb_connection);

	for (i = 0; i < RARRAY_LEN(ary_indexes); i++) {
		VALUE index_struct;
		VALUE row = rb_ary_entry(ary_indexes, i);
		VALUE table_name = rb_ary_entry(row, 0);
		VALUE index_name = rb_ary_entry(row, 1);
		VALUE unique = rb_ary_entry(row, 2);
		VALUE descending = rb_ary_entry(row, 3);
		VALUE columns = connection_index_columns(self, index_name);

		rb_funcall(table_name, id_rstrip_bang, 0);
		rb_funcall(index_name, id_rstrip_bang, 0);

		if (fb_connection->downcase_names) {
			if (no_lowercase(table_name)) {
				rb_funcall(table_name, id_downcase_bang, 0);
			}
			if (no_lowercase(index_name)) {
				rb_funcall(index_name, id_downcase_bang, 0);
			}
		}

		rb_str_freeze(table_name);
		rb_str_freeze(index_name);

		unique = (unique == INT2FIX(1)) ? Qtrue : Qfalse;
		descending = (descending == INT2FIX(1)) ? Qtrue : Qfalse;

		index_struct = rb_struct_new(rb_sFbIndex, table_name, index_name, unique, descending, columns);
		rb_hash_aset(indexes, index_name, index_struct);
	}
	return indexes;
}

/*
static void define_attrs(VALUE klass, char **attrs)
{
	char *parm;
	while ((parm = *attrs))
	{
		rb_define_attr(klass, parm+1, 1, 1);
		attrs++;
	}
}
*/

static VALUE default_string(VALUE hash, const char *key, const char *def)
{
	VALUE sym = ID2SYM(rb_intern(key));
	VALUE val = rb_hash_aref(hash, sym);
	return NIL_P(val) ? rb_str_new2(def) : StringValue(val);
}

static VALUE default_int(VALUE hash, const char *key, int def)
{
	VALUE sym = ID2SYM(rb_intern(key));
	VALUE val = rb_hash_aref(hash, sym);
	return NIL_P(val) ? INT2NUM(def) : val;
}

static VALUE database_allocate_instance(VALUE klass)
{
    return TypedData_Wrap_Struct(klass, &fbdatabase_data_type, NULL);
}

static VALUE hash_from_connection_string(VALUE cs)
{
	VALUE hash = rb_hash_new();
	VALUE re_SemiColon = rb_reg_regcomp(rb_str_new2("\\s*;\\s*"));
	VALUE re_Equal = rb_reg_regcomp(rb_str_new2("\\s*=\\s*"));
	ID id_split = rb_intern("split");
	VALUE pairs = rb_funcall(cs, id_split, 1, re_SemiColon);
	int i;
	for (i = 0; i < RARRAY_LEN(pairs); i++) {
		VALUE pair = rb_ary_entry(pairs, i);
		VALUE keyValue = rb_funcall(pair, id_split, 1, re_Equal);
		if (RARRAY_LEN(keyValue) == 2) {
			VALUE key = rb_ary_entry(keyValue, 0);
			VALUE val = rb_ary_entry(keyValue, 1);
			rb_hash_aset(hash, rb_str_intern(key), val);
		}
	}
	return hash;
}

static void check_page_size(int page_size)
{
	if (page_size != 1024 && page_size != 2048 && page_size != 4096 && page_size != 8192 && page_size != 16384) {
		rb_raise(rb_eFbError, "Invalid page size: %d", page_size);
	}
}

/* call-seq:
 *   Database.new(options) -> Database
 *
 * Initialize Database with Hash of values:
 * :database:: Full Firebird connection string, e.g. 'localhost:/var/fbdata/drivertest.fdb' (required)
 * :username:: database username (default: 'sysdba')
 * :password:: database password (default: 'masterkey')
 * :charset:: character set to be used with the connection (default: 'NONE')
 * :role:: database role to connect using (default: nil)
 * :downcase_names:: Column names are reported in lowercase, unless they were originally mixed case (default: nil).
 * :page_size:: page size to use when creating a database (default: 4096)
 */
static VALUE database_initialize(int argc, VALUE *argv, VALUE self)
{
	VALUE parms, database;

	if (argc >= 1) {
		parms = argv[0];
		if (TYPE(parms) == T_STRING) {
			parms = hash_from_connection_string(parms);
		} else {
			Check_Type(parms, T_HASH);
		}
		database = rb_hash_aref(parms, ID2SYM(rb_intern("database")));
		if (NIL_P(database)) rb_raise(rb_eFbError, "Database must be specified.");
		rb_iv_set(self, "@database", database);
		rb_iv_set(self, "@username", default_string(parms, "username", "sysdba"));
		rb_iv_set(self, "@password", default_string(parms, "password", "masterkey"));
		rb_iv_set(self, "@charset", default_string(parms, "charset", "NONE"));
		rb_iv_set(self, "@role", rb_hash_aref(parms, ID2SYM(rb_intern("role"))));
		rb_iv_set(self, "@downcase_names", rb_hash_aref(parms, ID2SYM(rb_intern("downcase_names"))));
		rb_iv_set(self, "@encoding", default_string(parms, "encoding", "ASCII-8BIT"));
		rb_iv_set(self, "@page_size", default_int(parms, "page_size", 4096));
		rb_iv_set(self, "@command_timeout", rb_hash_aref(parms, ID2SYM(rb_intern("command_timeout"))));
	} else {
		rb_iv_set(self, "@encoding", rb_str_new2("ASCII-8BIT"));
	}
	return self;
}

/* call-seq:
 *   create() -> Database
 *   create() {|connection| } -> Database
 *
 * Create a database using the current database options.
 * If a block is provided, an open connection to the new database is passed to it
 * before being automatically closed.
 */
static VALUE database_create(VALUE self)
{
	ISC_STATUS isc_status[20];
	isc_db_handle handle = 0;
	isc_tr_handle local_transact = 0;
	VALUE parms, fmt, stmt;
	char *sql;

	VALUE database = rb_iv_get(self, "@database");
	VALUE username = rb_iv_get(self, "@username");
	VALUE password = rb_iv_get(self, "@password");
	VALUE page_size = rb_iv_get(self, "@page_size");
	VALUE charset = rb_iv_get(self, "@charset");

	check_page_size(NUM2INT(page_size));

	parms = rb_ary_new3(5, database, username, password, page_size, charset);

	fmt = rb_str_new2("CREATE DATABASE '%s' USER '%s' PASSWORD '%s' PAGE_SIZE = %d DEFAULT CHARACTER SET %s;");
	stmt = rb_funcall(fmt, rb_intern("%"), 1, parms);
	sql = StringValuePtr(stmt);

	if (isc_dsql_execute_immediate(isc_status, &handle, &local_transact, 0, sql, 3, NULL) != 0) {
		fb_error_check(isc_status);
	}
	if (handle) {
		if (rb_block_given_p()) {
			VALUE connection = connection_create(handle, self);
			rb_ensure(rb_yield,connection,connection_close,connection);
		} else {
			isc_detach_database(isc_status, &handle);
			fb_error_check(isc_status);
		}
	}

	return self;
}

/* call-seq:
 *   Database.create(options) -> Database
 *   Database.create(options) {|connection| } -> Database
 *
 * Create a database using the specified options (see: Database.new for details of options Hash).
 * If a block is provided, an open connection to the new database is passed to it
 * before being automatically closed.
 */
static VALUE database_s_create(int argc, VALUE *argv, VALUE klass)
{
	VALUE obj = database_allocate_instance(klass);
	database_initialize(argc, argv, obj);
	return database_create(obj);
}

/* call-seq:
 *   connect() -> Connection
 *   connect() {|connection| } -> nil
 *
 * Connect to the database specified by the current database options.
 *
 * If a block is provided, the open connection is passed to it before being
 * automatically closed.
 */
static VALUE database_connect(VALUE self)
{
	ISC_STATUS isc_status[20];
	char *dbp;
	long length;
	isc_db_handle handle = 0;
	VALUE database = rb_iv_get(self, "@database");

	Check_Type(database, T_STRING);
	dbp = connection_create_dbp(self, &length);
	isc_attach_database(isc_status, 0, StringValuePtr(database), &handle, length, dbp);
	xfree(dbp);
	fb_error_check(isc_status);
	{
		VALUE connection = connection_create(handle, self);
		if (rb_block_given_p()) {
			return rb_ensure(rb_yield, connection, connection_close, connection);
			return Qnil;
		} else {
			return connection;
		}
	}
}

/* call-seq:
 *   Database.connect(options) -> Connection
 *   Database.connect(options) {|connection| } -> nil
 *
 * Connect to a database using the options given (see: Database.new for details of options Hash).
 *
 * If a block is provided, the open connection is passed to it before being
 * automatically closed.
 */
static VALUE database_s_connect(int argc, VALUE *argv, VALUE klass)
{
	VALUE obj = database_allocate_instance(klass);
	database_initialize(argc, argv, obj);
	return database_connect(obj);
}

/* call-seq:
 *   drop() -> nil
 *
 * Drop the database specified by the current database options.
 */
static VALUE database_drop(VALUE self)
{
	struct FbConnection *fb_connection;

	VALUE connection = database_connect(self);
	TypedData_Get_Struct(connection, struct FbConnection, &fbconnection_data_type, fb_connection);
	isc_drop_database(fb_connection->isc_status, &fb_connection->db);
	fb_error_check(fb_connection->isc_status);
	/* fb_connection_remove(fb_connection); */
	return Qnil;
}

/* call-seq:
 *   Database.drop(options) -> nil
 *
 * Drop the database specified by the options given (see: Database.new for details of options Hash).
 */
static VALUE database_s_drop(int argc, VALUE *argv, VALUE klass)
{
	VALUE obj = database_allocate_instance(klass);
	database_initialize(argc, argv, obj);
	return database_drop(obj);
}

/* Helper: activate bigdecimal gem (needed for Ruby 3.4+ where it's not a default gem) */
static VALUE fb_activate_gem(VALUE arg)
{
	rb_funcall(rb_mKernel, rb_intern("gem"), 1, rb_str_new2("bigdecimal"));
	return Qnil;
}

static VALUE fb_gem_error(VALUE arg, VALUE exc)
{
	return Qnil;  /* gem activation failure is harmless on older Ruby */
}

void Init_fb()
{
	/* Ruby 3.4+ requires explicit gem activation before require for bundled gems */
	rb_rescue(fb_activate_gem, Qnil, fb_gem_error, Qnil);
	rb_funcall(rb_mKernel, rb_intern("require"), 1, rb_str_new2("bigdecimal"));

	rb_mFb = rb_define_module("Fb");

	rb_cFbDatabase = rb_define_class_under(rb_mFb, "Database", rb_cObject);
	rb_undef_alloc_func(rb_cFbDatabase);
    rb_define_alloc_func(rb_cFbDatabase, database_allocate_instance);
    rb_define_method(rb_cFbDatabase, "initialize", database_initialize, -1);
	rb_define_attr(rb_cFbDatabase, "database", 1, 1);
	rb_define_attr(rb_cFbDatabase, "username", 1, 1);
	rb_define_attr(rb_cFbDatabase, "password", 1, 1);
	rb_define_attr(rb_cFbDatabase, "charset", 1, 1);
	rb_define_attr(rb_cFbDatabase, "role", 1, 1);
	rb_define_attr(rb_cFbDatabase, "downcase_names", 1, 1);
	rb_define_attr(rb_cFbDatabase, "encoding", 1, 1);
	rb_define_attr(rb_cFbDatabase, "page_size", 1, 1);
	rb_define_attr(rb_cFbDatabase, "command_timeout", 1, 1);
    rb_define_method(rb_cFbDatabase, "create", database_create, 0);
	rb_define_singleton_method(rb_cFbDatabase, "create", database_s_create, -1);
	rb_define_method(rb_cFbDatabase, "connect", database_connect, 0);
	rb_define_singleton_method(rb_cFbDatabase, "connect", database_s_connect, -1);
	rb_define_method(rb_cFbDatabase, "drop", database_drop, 0);
	rb_define_singleton_method(rb_cFbDatabase, "drop", database_s_drop, -1);

	rb_cFbConnection = rb_define_class_under(rb_mFb, "Connection", rb_cObject);
	rb_undef_alloc_func(rb_cFbConnection);
	rb_undef_method(CLASS_OF(rb_cFbConnection), "new");
	rb_define_attr(rb_cFbConnection, "database", 1, 1);
	rb_define_attr(rb_cFbConnection, "username", 1, 1);
	rb_define_attr(rb_cFbConnection, "password", 1, 1);
	rb_define_attr(rb_cFbConnection, "charset", 1, 1);
	rb_define_attr(rb_cFbConnection, "role", 1, 1);
	rb_define_attr(rb_cFbConnection, "downcase_names", 1, 1);
	rb_define_attr(rb_cFbConnection, "encoding", 1, 1);
	rb_define_method(rb_cFbConnection, "to_s", connection_to_s, 0);
	rb_define_method(rb_cFbConnection, "execute", connection_execute, -1);
	rb_define_method(rb_cFbConnection, "query", connection_query, -1);
	rb_define_method(rb_cFbConnection, "transaction", connection_transaction, -1);
	rb_define_method(rb_cFbConnection, "transaction_started", connection_transaction_started, 0);
	rb_define_method(rb_cFbConnection, "commit", connection_commit, 0);
	rb_define_method(rb_cFbConnection, "rollback", connection_rollback, 0);
	rb_define_method(rb_cFbConnection, "encoding=", connection_set_encoding, 1);
	rb_define_method(rb_cFbConnection, "command_timeout", connection_command_timeout, 0);
	rb_define_method(rb_cFbConnection, "command_timeout=", connection_set_command_timeout, 1);
	rb_define_method(rb_cFbConnection, "cancel", connection_cancel, 0);
	rb_define_method(rb_cFbConnection, "close", connection_close, 0);
	rb_define_method(rb_cFbConnection, "drop", connection_drop, 0);
	rb_define_method(rb_cFbConnection, "open?", connection_is_open, 0);
	rb_define_method(rb_cFbConnection, "dialect", connection_dialect, 0);
	rb_define_method(rb_cFbConnection, "db_dialect", connection_db_dialect, 0);
	rb_define_method(rb_cFbConnection, "table_names", connection_table_names, 0);
	rb_define_method(rb_cFbConnection, "generator_names", connection_generator_names, 0);
	rb_define_method(rb_cFbConnection, "view_names", connection_view_names, 0);
	rb_define_method(rb_cFbConnection, "role_names", connection_role_names, 0);
	rb_define_method(rb_cFbConnection, "procedure_names", connection_procedure_names, 0);
	rb_define_method(rb_cFbConnection, "trigger_names", connection_trigger_names, 0);
	rb_define_method(rb_cFbConnection, "indexes", connection_indexes, 0);
	rb_define_method(rb_cFbConnection, "columns", connection_columns, 1);
	/* rb_define_method(rb_cFbConnection, "cursor", connection_cursor, 0); */

	rb_cFbCursor = rb_define_class_under(rb_mFb, "Cursor", rb_cObject);
	rb_undef_alloc_func(rb_cFbCursor);
	rb_undef_method(CLASS_OF(rb_cFbCursor), "new");
	/* rb_define_method(rb_cFbCursor, "execute", cursor_execute, -1); */
	rb_define_method(rb_cFbCursor, "fields", cursor_fields, -1);
	rb_define_method(rb_cFbCursor, "fetch", cursor_fetch, -1);
	rb_define_method(rb_cFbCursor, "fetchall", cursor_fetchall, -1);
	rb_define_method(rb_cFbCursor, "each", cursor_each, -1);
	rb_define_method(rb_cFbCursor, "close", cursor_close, 0);
	rb_define_method(rb_cFbCursor, "drop", cursor_drop, 0);

	rb_cFbSqlType = rb_define_class_under(rb_mFb, "SqlType", rb_cObject);
	rb_undef_alloc_func(rb_cFbSqlType);
	rb_undef_method(CLASS_OF(rb_cFbSqlType), "new");
	rb_define_singleton_method(rb_cFbSqlType, "from_code", sql_type_from_code, 2);

	rb_eFbError = rb_define_class_under(rb_mFb, "Error", rb_eStandardError);
	rb_define_method(rb_eFbError, "error_code", error_error_code, 0);

	/* constantes de INT128: 2**128 y 2**64-1 */
	fb_int128_modulus = rb_funcall(INT2FIX(2), rb_intern("**"), 1, INT2FIX(128));
	rb_global_variable(&fb_int128_modulus);
	fb_int128_lomask = rb_funcall(INT2FIX(2), rb_intern("**"), 1, INT2FIX(64));
	fb_int128_lomask = rb_funcall(fb_int128_lomask, rb_intern("-"), 1, INT2FIX(1));
	rb_global_variable(&fb_int128_lomask);

	rb_sFbField = rb_struct_define("FbField", "name", "sql_type", "sql_subtype", "display_size", "internal_size", "precision", "scale", "nullable", "type_code", NULL);
	rb_sFbIndex = rb_struct_define("FbIndex", "table_name", "index_name", "unique", "descending", "columns", NULL);
	rb_sFbColumn = rb_struct_define("FbColumn", "name", "domain", "sql_type", "sql_subtype", "length", "precision", "scale", "default", "nullable", NULL);

	rb_require("date");
	rb_require("time"); /* Needed as of Ruby 1.8.5 */
	rb_cDate = rb_const_get(rb_cObject, rb_intern("Date"));

	id_matches = rb_intern("=~");
	id_downcase_bang = rb_intern("downcase!");
	re_lowercase = rb_reg_regcomp(rb_str_new2("[[:lower:]]"));
	rb_global_variable(&re_lowercase);
	id_rstrip_bang = rb_intern("rstrip!");
    id_sub_bang = rb_intern("sub!");
    id_force_encoding = rb_intern("force_encoding");
}
