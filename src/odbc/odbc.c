/* MDB Tools - A library for reading MS Access database file
 * Copyright (C) 2000 Brian Bruns
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_ICONV
/* Disable ODBC wide char if iconv is not available */
#define ENABLE_ODBC_W
#endif

#ifdef ENABLE_ODBC_W
#define SQL_NOUNICODEMAP
#define UNICODE
#endif //ENABLE_ODBC_W

#include <sql.h>
#include <sqlext.h>

#include <mdbodbc.h>

#include <string.h>
#include <stdio.h>

#include "connectparams.h"

/* #define TRACE(x) fprintf(stderr,"Function %s\n", x); */
#define TRACE(x)
#define DEBUG(x) fprintf(stderr,"DEBUG: %s\n", x);
/* #define DEBUG(x) */

#ifdef ENABLE_ODBC_W
static iconv_t iconv_in,iconv_out;
#endif //ENABLE_ODBC_W

static SQLSMALLINT _odbc_get_client_type(MdbColumn *col);
static int _odbc_fix_literals(struct _hstmt *stmt);
//static int _odbc_get_server_type(int clt_type);
static int _odbc_get_string_size(int size, SQLCHAR *str);
static SQLRETURN SQL_API _SQLAllocConnect(SQLHENV henv, SQLHDBC *phdbc);
static SQLRETURN SQL_API _SQLAllocEnv(SQLHENV *phenv);
static SQLRETURN SQL_API _SQLAllocStmt(SQLHDBC hdbc, SQLHSTMT *phstmt);
static SQLRETURN SQL_API _SQLFreeConnect(SQLHDBC hdbc);
static SQLRETURN SQL_API _SQLFreeEnv(SQLHENV henv);
static SQLRETURN SQL_API _SQLFreeStmt(SQLHSTMT hstmt, SQLUSMALLINT fOption);

static void bind_columns (struct _hstmt*);
static void unbind_columns (struct _hstmt*);

#define FILL_FIELD(f,v,s) mdb_fill_temp_field(f,v,s,0,0,0,0)

#ifndef MIN
#define MIN(a,b) (a>b ? b : a)
#endif
#define _MAX_ERROR_LEN 255
static char lastError[_MAX_ERROR_LEN+1];
static char sqlState[6];

static int refCount = 0;
static GPtrArray *handles = NULL;

typedef struct {
	SQLCHAR *type_name;
	SQLSMALLINT data_type;
	SQLINTEGER column_size;
	SQLCHAR *literal_prefix;
	SQLCHAR *literal_suffix;
	SQLCHAR *create_params;
	SQLSMALLINT nullable;
	SQLSMALLINT case_sensitive;
	SQLSMALLINT searchable;
	SQLSMALLINT *unsigned_attribute;
	SQLSMALLINT fixed_prec_scale;
	SQLSMALLINT auto_unique_value;
	SQLCHAR *local_type_name;
	SQLSMALLINT minimum_scale;
	SQLSMALLINT maximum_scale;
	SQLSMALLINT sql_data_type;
	SQLSMALLINT *sql_datetime_sub;
	SQLSMALLINT *num_prec_radix;
	SQLSMALLINT *interval_precision;
} TypeInfo;

TypeInfo type_info[] = {
	{(SQLCHAR*)"text", SQL_VARCHAR, 255, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_FALSE, NULL, 0, 255, SQL_VARCHAR, NULL, NULL, NULL},
	{(SQLCHAR*)"memo", SQL_VARCHAR, 4096, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_FALSE, NULL, 0, 4096, SQL_VARCHAR, NULL, NULL, NULL},
	{(SQLCHAR*)"text", SQL_CHAR, 255, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_FALSE, NULL, 0, 255, SQL_CHAR, NULL, NULL, NULL},
	{(SQLCHAR*)"numeric", SQL_NUMERIC, 255, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_FALSE, NULL, 0, 255, SQL_NUMERIC, NULL, NULL, NULL},
	{(SQLCHAR*)"numeric", SQL_DECIMAL, 255, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_FALSE, NULL, 0, 255, SQL_DECIMAL, NULL, NULL, NULL},
	{(SQLCHAR*)"long integer", SQL_INTEGER, 4, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_TRUE, NULL, 0, 4, SQL_INTEGER, NULL, NULL, NULL},
	{(SQLCHAR*)"integer", SQL_SMALLINT, 4, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_TRUE, NULL, 0, 4, SQL_SMALLINT, NULL, NULL, NULL},
	{(SQLCHAR*)"integer", SQL_SMALLINT, 4, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_TRUE, NULL, 0, 4, SQL_SMALLINT, NULL, NULL, NULL},
	{(SQLCHAR*)"single", SQL_REAL, 4, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_TRUE, NULL, 0, 4, SQL_REAL, NULL, NULL, NULL},
	{(SQLCHAR*)"double", SQL_DOUBLE, 8, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_TRUE, NULL, 0, 8, SQL_FLOAT, NULL, NULL, NULL},
	{(SQLCHAR*)"datetime", SQL_DATETIME, 8, NULL, NULL, NULL, SQL_TRUE, SQL_TRUE, SQL_TRUE, NULL, SQL_FALSE, SQL_TRUE, NULL, 0, 8, SQL_DATETIME, NULL, NULL, NULL}
};

#define NUM_TYPE_INFO_COLS 19
#define MAX_TYPE_INFO 11

#ifdef ENABLE_ODBC_W
void __attribute__ ((constructor)) my_init(){
	TRACE("my_init");
	int endian = 1;
	const char* wcharset;
	if (sizeof(SQLWCHAR) == 2)
		if (*(char*)&endian == 1)
			wcharset = "UCS-2LE";
		else
			wcharset = "UCS-2BE";
	else if (sizeof(SQLWCHAR) == 4)
		if (*(char*)&endian == 1)
			wcharset = "UCS-4LE";
		else
			wcharset = "UCS-4BE";
	else
		fprintf(stderr, "Unsupported SQLWCHAR width %zd\n", sizeof(SQLWCHAR));
//fprintf(stderr,"charset %s\n", wcharset);
	//fprintf(stderr, "SQLWCHAR width %d\n", sizeof(SQLWCHAR));
/*
#if __SIZEOF_WCHAR_T__ == 4 || __WCHAR_MAX__ > 0x10000
	#define WCHAR_CHARSET "UCS-4LE"
#else
	#define WCHAR_CHARSET "UCS-2LE"
#endif
*/
	iconv_out = iconv_open(wcharset, "UTF-8");
	iconv_in = iconv_open("UTF-8", wcharset);
}

void __attribute__ ((destructor)) my_fini(){
	TRACE("my_fini");
	if(iconv_out != (iconv_t)-1)iconv_close(iconv_out);
	if(iconv_in != (iconv_t)-1)iconv_close(iconv_in);
}

static int unicode2ascii(char *_in, size_t *_lin, char *_out, size_t *_lout){
	char *in=_in, *out=_out;
	size_t lin=*_lin, lout=*_lout;
	int ret = iconv(iconv_in, &in, &lin, &out, &lout);
	*_lin -= lin;
	*_lout -= lout;
	return ret;
}

static int ascii2unicode(char *_in, size_t *_lin, char *_out, size_t *_lout){
	//fprintf(stderr,"ascii2unicode %08x %08x %08x %08x\n",_in,_lin,_out,_lout);
	char *in=_in, *out=_out;
	size_t lin=*_lin, lout=*_lout;
	//fprintf(stderr,"ascii2unicode %zd %zd\n",lin,lout);
	int ret = iconv(iconv_out, &in, &lin, &out, &lout);
	*_lin -= lin;
	*_lout -= lout;
	return ret;
}

static int sqlwlen(SQLWCHAR *p){
	int r=0;
	for(;*p;r++)
		p++;
	return r;
}
#endif // ENABLE_ODBC_W

/* The SQL engine is presently non-reenterrant and non-thread safe.	 
   See _SQLExecute for details.
*/

static void LogError (const char* error)
{
   /*
	* Someday, I might make this store more than one error.
	*/
   strncpy (lastError, error, _MAX_ERROR_LEN);
   lastError[_MAX_ERROR_LEN] = '\0'; /* in case we had a long message */
}

static void drv_init (void)
{
	TRACE("drv_init");
	handles = g_ptr_array_new();
}

static void drv_exit ()
{
	unsigned int i;
	struct _handle *handle;
	/* struct _hstmt *stmt; */
	struct _hdbc *dbc;
	struct _henv *env;

	TRACE("drv_exit");

	if (handles) {
		for (i=0; i<handles->len; i++) {
			handle = (struct _handle *) g_ptr_array_index(handles, i);
			switch(handle->type) {
				case SQL_HANDLE_STMT:
					DEBUG("Stmt not freed!");
					/* stmt = (struct _hstmt *) handle; */
					/* Not sure what else needs to be done here; should
					   be taken care of by just freeing the handle*/
					break;

				case SQL_HANDLE_DBC:
					DEBUG("Dbc not freed!");
					dbc = (struct _hdbc *) handle;
					FreeConnectParams(dbc->params);
					break;

				case SQL_HANDLE_ENV:
					DEBUG("Env not freed!");
					env = (struct _henv *) handle;
					mdb_sql_exit(env->sql);
					g_free(env->sql);
					break;
			}

			g_free(handle);
		}

		g_ptr_array_free(handles, TRUE);
		handles = NULL;
	}
}

static SQLRETURN do_connect (
   SQLHDBC hdbc,
   char *database)
{
	struct _hdbc *dbc = (struct _hdbc *) hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;

	if (mdb_sql_open(env->sql, database))
		return SQL_SUCCESS;
	else
		return SQL_ERROR;
}

static SQLRETURN SQL_API _SQLDriverConnect(
	SQLHDBC			   hdbc,
	SQLHWND			   hwnd,
	SQLCHAR		   *szConnStrIn,
	SQLSMALLINT		   cbConnStrIn,
	SQLCHAR		   *szConnStrOut,
	SQLSMALLINT		   cbConnStrOutMax,
	SQLSMALLINT	   *pcbConnStrOut,
	SQLUSMALLINT	   fDriverCompletion)
{
	char* dsn = NULL;
	char* database = NULL;
	ConnectParams* params;
	SQLRETURN ret;

	TRACE("_SQLDriverConnect");
	strcpy (lastError, "");

	struct _hdbc *dbc = (struct _hdbc *) hdbc;
	params = dbc->params;

	if ((dsn = ExtractDSN (params, (gchar*)szConnStrIn))) {
		if (!LookupDSN (params, dsn)){
			LogError ("Could not find DSN in odbc.ini");
			return SQL_ERROR;
		}
		SetConnectString (params, (gchar*)szConnStrIn);
		if (!(database = GetConnectParam (params, "Database"))){
			LogError ("Could not find Database parameter");
			return SQL_ERROR;
		}
		ret = do_connect (hdbc, database);
		return ret;
	}

	if ((database = ExtractDBQ (params, (gchar*)szConnStrIn))) {
		ret = do_connect (hdbc, database);
		return ret;
	}
	LogError ("Could not find DSN nor DBQ in connect string");
	return SQL_ERROR;
}

SQLRETURN SQL_API SQLDriverConnect(
	SQLHDBC			hdbc,
	SQLHWND			hwnd,
	SQLCHAR		   *szConnStrIn,
	SQLSMALLINT		cbConnStrIn,
	SQLCHAR		   *szConnStrOut,
	SQLSMALLINT		cbConnStrOutMax,
	SQLSMALLINT	   *pcbConnStrOut,
	SQLUSMALLINT	fDriverCompletion)
#ifdef HAVE_ATTRIBUTE_ALIAS
__attribute__((alias("_SQLDriverConnect")));
#else
{ return _SQLDriverConnect(hdbc, hwnd, szConnStrIn, cbConnStrIn, szConnStrOut, cbConnStrOutMax, pcbConnStrOut, fDriverCompletion); }
#endif

#ifdef ENABLE_ODBC_W
SQLRETURN SQL_API SQLDriverConnectW(
	SQLHDBC			   hdbc,
	SQLHWND			   hwnd,
	SQLWCHAR		   *szConnStrIn,
	SQLSMALLINT		   cbConnStrIn,
	SQLWCHAR		   *szConnStrOut,
	SQLSMALLINT		   cbConnStrOutMax,
	SQLSMALLINT		  *pcbConnStrOut,
	SQLUSMALLINT	   fDriverCompletion)
{
	TRACE("SQLDriverConnectW");
	if(cbConnStrIn==SQL_NTS)cbConnStrIn=sqlwlen(szConnStrIn);
	{
		size_t l = cbConnStrIn*sizeof(SQLWCHAR), z = (cbConnStrIn+1)*3;
		SQLCHAR *tmp = malloc(z);
		SQLRETURN ret;
		unicode2ascii((char*)szConnStrIn, &l, (char*)tmp, &z);
		tmp[z] = 0;
		ret = _SQLDriverConnect(hdbc,hwnd,tmp,SQL_NTS,NULL,0,pcbConnStrOut,fDriverCompletion);
		free(tmp);
		if (szConnStrOut && cbConnStrOutMax>0)
			szConnStrOut[0] = 0;
		if (pcbConnStrOut)
			*pcbConnStrOut = 0;
		return ret;
	}
}
#endif // ENABLE_ODBC_W

SQLRETURN SQL_API SQLBrowseConnect(
	SQLHDBC			   hdbc,
	SQLCHAR			  *szConnStrIn,
	SQLSMALLINT		   cbConnStrIn,
	SQLCHAR			  *szConnStrOut,
	SQLSMALLINT		   cbConnStrOutMax,
	SQLSMALLINT		  *pcbConnStrOut)
{
	TRACE("SQLBrowseConnect");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLColumnPrivileges(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLCHAR			  *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLCHAR			  *szTableName,
	SQLSMALLINT		   cbTableName,
	SQLCHAR			  *szColumnName,
	SQLSMALLINT		   cbColumnName)
{
	TRACE("SQLColumnPrivileges");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLDescribeParam(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   ipar,
	SQLSMALLINT		  *pfSqlType,
	SQLULEN			  *pcbParamDef,
	SQLSMALLINT		  *pibScale,
	SQLSMALLINT		  *pfNullable)
{
	TRACE("SQLDescribeParam");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLExtendedFetch(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   fFetchType,
	SQLLEN			   irow,
	SQLULEN			  *pcrow,
	SQLUSMALLINT	  *rgfRowStatus)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	struct _hdbc *dbc = (struct _hdbc *) stmt->hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;

	TRACE("SQLExtendedFetch");
	if (fFetchType!=SQL_FETCH_NEXT) {
		LogError("Fetch type not supported in SQLExtendedFetch");
		return SQL_ERROR;
	}
	if (pcrow)
		*pcrow=1;
	if (rgfRowStatus)
		*rgfRowStatus = SQL_SUCCESS; /* what is the row status value? */
	
	bind_columns(stmt);

	if (mdb_fetch_row(env->sql->cur_table)) {
		stmt->rows_affected++;
		return SQL_SUCCESS;
	} else {
		return SQL_NO_DATA_FOUND;
	}
}

SQLRETURN SQL_API SQLForeignKeys(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szPkCatalogName,
	SQLSMALLINT		   cbPkCatalogName,
	SQLCHAR			  *szPkSchemaName,
	SQLSMALLINT		   cbPkSchemaName,
	SQLCHAR			  *szPkTableName,
	SQLSMALLINT		   cbPkTableName,
	SQLCHAR			  *szFkCatalogName,
	SQLSMALLINT		   cbFkCatalogName,
	SQLCHAR			  *szFkSchemaName,
	SQLSMALLINT		   cbFkSchemaName,
	SQLCHAR			  *szFkTableName,
	SQLSMALLINT		   cbFkTableName)
{
	TRACE("SQLForeignKeys");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLMoreResults(
	SQLHSTMT		   hstmt)
{
	TRACE("SQLMoreResults");
	return SQL_NO_DATA;
}

SQLRETURN SQL_API SQLNativeSql(
	SQLHDBC			   hdbc,
	SQLCHAR			  *szSqlStrIn,
	SQLINTEGER		   cbSqlStrIn,
	SQLCHAR			  *szSqlStr,
	SQLINTEGER		   cbSqlStrMax,
	SQLINTEGER		  *pcbSqlStr)
{
	TRACE("SQLNativeSql");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLNumParams(
	SQLHSTMT		   hstmt,
	SQLSMALLINT		  *pcpar)
{
	TRACE("SQLNumParams");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLParamOptions(
	SQLHSTMT		   hstmt,
	SQLULEN			   crow,
	SQLULEN			  *pirow)
{
	TRACE("SQLParamOptions");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLPrimaryKeys(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLCHAR			  *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLCHAR			  *szTableName,
	SQLSMALLINT		   cbTableName)
{
	TRACE("SQLPrimaryKeys");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLProcedureColumns(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLCHAR			  *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLCHAR			  *szProcName,
	SQLSMALLINT		   cbProcName,
	SQLCHAR			  *szColumnName,
	SQLSMALLINT		   cbColumnName)
{
	TRACE("SQLProcedureColumns");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLProcedures(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLCHAR			  *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLCHAR			  *szProcName,
	SQLSMALLINT		   cbProcName)
{
	TRACE("SQLProcedures");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetPos(
	SQLHSTMT		   hstmt,
	SQLSETPOSIROW	   irow,
	SQLUSMALLINT	   fOption,
	SQLUSMALLINT	   fLock)
{
	TRACE("SQLSetPos");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLTablePrivileges(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLCHAR			  *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLCHAR			  *szTableName,
	SQLSMALLINT		   cbTableName)
{
	TRACE("SQLTablePrivileges");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLDrivers(
	SQLHENV			   henv,
	SQLUSMALLINT	   fDirection,
	SQLCHAR			  *szDriverDesc,
	SQLSMALLINT		   cbDriverDescMax,
	SQLSMALLINT		  *pcbDriverDesc,
	SQLCHAR			  *szDriverAttributes,
	SQLSMALLINT		   cbDrvrAttrMax,
	SQLSMALLINT		  *pcbDrvrAttr)
{
	TRACE("SQLDrivers");
	return SQL_SUCCESS;
}
SQLRETURN SQL_API SQLSetEnvAttr (
	SQLHENV EnvironmentHandle,
	SQLINTEGER Attribute,
	SQLPOINTER Value,
	SQLINTEGER StringLength)
{
	TRACE("SQLSetEnvAttr");
	return SQL_SUCCESS;
}


SQLRETURN SQL_API SQLBindParameter(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   ipar,
	SQLSMALLINT		   fParamType,
	SQLSMALLINT		   fCType,
	SQLSMALLINT		   fSqlType,
	SQLULEN			   cbColDef,
	SQLSMALLINT		   ibScale,
	SQLPOINTER		   rgbValue,
	SQLLEN			   cbValueMax,
	SQLLEN			  *pcbValue)
{

	TRACE("SQLBindParameter");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLAllocHandle(
	SQLSMALLINT HandleType,
	SQLHANDLE InputHandle,
	SQLHANDLE * OutputHandle)
{
	SQLRETURN result;
	struct _handle *handle;

	TRACE("SQLAllocHandle");
	switch(HandleType) {
		case SQL_HANDLE_STMT:
			result =  _SQLAllocStmt(InputHandle,OutputHandle);
			break;
		case SQL_HANDLE_DBC:
			result =  _SQLAllocConnect(InputHandle,OutputHandle);
			break;
		case SQL_HANDLE_ENV:
			result = _SQLAllocEnv(OutputHandle);
			break;
		default:
			return SQL_ERROR;
	}
	/* Add the handle to the open handles list */
	handle = (struct _handle *) *OutputHandle;
	handle->type = HandleType;
	g_ptr_array_add(handles, (gpointer) handle);

	return result;
}
static SQLRETURN SQL_API _SQLAllocConnect(
	SQLHENV			   henv,
	SQLHDBC			  *phdbc)
{
	struct _henv *env;
	struct _hdbc *dbc;

	TRACE("_SQLAllocConnect");

	env = (struct _henv *) henv;
	dbc = (struct _hdbc *) g_malloc0(sizeof(struct _hdbc));

	dbc->henv=env;
	dbc->params = NewConnectParams ();

	*phdbc=dbc;

	return SQL_SUCCESS;
}
SQLRETURN SQL_API SQLAllocConnect(
	SQLHENV			   henv,
	SQLHDBC			  *phdbc)
{
	TRACE("SQLAllocConnect");
	return _SQLAllocConnect(henv, phdbc);
}

static SQLRETURN SQL_API _SQLAllocEnv(
	SQLHENV			  *phenv)
{
	struct _henv *env;

	TRACE("_SQLAllocEnv");

	//If this is our first instantiation of the driver
	//Allocate a list of handles so we're not losing memory
	if(!handles)
		drv_init();

	refCount++;

	env = (SQLHENV) g_malloc0(sizeof(struct _henv));
	env->sql = mdb_sql_init();

	*phenv=env;

	return SQL_SUCCESS;
}
SQLRETURN SQL_API SQLAllocEnv(
	SQLHENV			  *phenv)
{
	TRACE("SQLAllocEnv");
	return _SQLAllocEnv(phenv);
}

static SQLRETURN SQL_API _SQLAllocStmt(
	SQLHDBC			   hdbc,
	SQLHSTMT		  *phstmt)
{
	struct _hstmt *stmt;

	TRACE("_SQLAllocStmt");

	stmt = (SQLHSTMT) g_malloc0(sizeof(struct _hstmt));
	stmt->hdbc=hdbc;

	*phstmt = stmt;

	return SQL_SUCCESS;
}
SQLRETURN SQL_API SQLAllocStmt(
	SQLHDBC			   hdbc,
	SQLHSTMT		  *phstmt)
{
	TRACE("SQLAllocStmt");
	return _SQLAllocStmt(hdbc,phstmt);
}

SQLRETURN SQL_API SQLBindCol(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   icol,
	SQLSMALLINT		   fCType,
	SQLPOINTER		   rgbValue,
	SQLLEN			   cbValueMax,
	SQLLEN			  *pcbValue)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	struct _sql_bind_info *cur, *newitem;

	TRACE("SQLBindCol");
	/* find available item in list */
	cur = stmt->bind_head;
	while (cur) {
		if (cur->column_number==icol) {
			break;
		}
		cur = cur->next;
	}
	/* if this is a repeat */
	if (cur) {
		cur->column_bindtype = fCType;
		cur->column_bindlen = cbValueMax;
		cur->varaddr = (char *) rgbValue;
	} else {
		/* didn't find it create a new one */
		newitem = (struct _sql_bind_info *) g_malloc0(sizeof(struct _sql_bind_info));
		newitem->column_number = icol;
		newitem->column_bindtype = fCType;
		newitem->column_bindlen = cbValueMax;
		newitem->column_lenbind = (int *)pcbValue;
		newitem->varaddr = (char *) rgbValue;
		/* if there's no head yet */
		if (! stmt->bind_head) {
			stmt->bind_head = newitem;
		} else {
			/* find the tail of the list */
			cur = stmt->bind_head;
			while (cur->next) {
				cur = cur->next;
			}
			cur->next = newitem;
		}
	}

	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLCancel(
	SQLHSTMT		   hstmt)
{
	TRACE("SQLCancel");
	return SQL_SUCCESS;
}

static SQLRETURN SQL_API _SQLConnect(
	SQLHDBC			   hdbc,
	SQLCHAR			  *szDSN,
	SQLSMALLINT		   cbDSN,
	SQLCHAR			  *szUID,
	SQLSMALLINT		   cbUID,
	SQLCHAR			  *szAuthStr,
	SQLSMALLINT		   cbAuthStr)
{
	char* database = NULL;
	ConnectParams* params;
	SQLRETURN ret;

	TRACE("_SQLConnect");
	strcpy (lastError, "");

	params = ((struct _hdbc *) hdbc)->params;

	params->dsnName = g_string_assign (params->dsnName, (gchar*)szDSN);

	if (!LookupDSN (params, (gchar*)szDSN))
	{
		LogError ("Could not find DSN in odbc.ini");
		return SQL_ERROR;
	}
	else if (!(database = GetConnectParam (params, "Database")))
	{
		LogError ("Could not find Database parameter");
		return SQL_ERROR;
	}

	ret = do_connect (hdbc, database);
	return ret;
}

SQLRETURN SQL_API SQLConnect(
	SQLHDBC			   hdbc,
	SQLCHAR			  *szDSN,
	SQLSMALLINT		   cbDSN,
	SQLCHAR			  *szUID,
	SQLSMALLINT		   cbUID,
	SQLCHAR			  *szAuthStr,
	SQLSMALLINT		   cbAuthStr)
#ifdef HAVE_ATTRIBUTE_ALIAS
__attribute__((alias("_SQLConnect")));
#else
{ return _SQLConnect(hdbc, szDSN, cbDSN, szUID, cbUID, szAuthStr, cbAuthStr); }
#endif

#ifdef ENABLE_ODBC_W
SQLRETURN SQL_API SQLConnectW(
	SQLHDBC			   hdbc,
	SQLWCHAR		   *szDSN,
	SQLSMALLINT		   cbDSN,
	SQLWCHAR		   *szUID,
	SQLSMALLINT		   cbUID,
	SQLWCHAR		   *szAuthStr,
	SQLSMALLINT		   cbAuthStr)
{
	if(cbDSN==SQL_NTS)cbDSN=sqlwlen(szDSN);
	if(cbUID==SQL_NTS)cbUID=sqlwlen(szUID);
	if(cbAuthStr==SQL_NTS)cbAuthStr=sqlwlen(szAuthStr);
	{
		SQLCHAR *tmp1=calloc(cbDSN*4,1),*tmp2=calloc(cbUID*4,1),*tmp3=calloc(cbAuthStr*4,1);
		size_t l1=cbDSN*4,z1=cbDSN*2;
		size_t l2=cbUID*4,z2=cbUID*2;
		size_t l3=cbAuthStr*4,z3=cbAuthStr*2;
		SQLRETURN ret;
		unicode2ascii((char*)szDSN, &z1, (char*)tmp1, &l1);
		unicode2ascii((char*)szUID, &z2, (char*)tmp2, &l2);
		unicode2ascii((char*)szAuthStr, &z3, (char*)tmp3, &l3);
		ret = SQLConnect(hdbc, tmp1, l1, tmp2, l2, tmp3, l3);
		free(tmp1),free(tmp2),free(tmp3);
		return ret;
	}
}
#endif //ENABLE_ODBC_W

static SQLRETURN SQL_API _SQLDescribeCol(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   icol,
	SQLCHAR			  *szColName,
	SQLSMALLINT		   cbColNameMax,
	SQLSMALLINT		  *pcbColName,
	SQLSMALLINT		  *pfSqlType,
	SQLULEN			  *pcbColDef, /* precision */
	SQLSMALLINT		  *pibScale,
	SQLSMALLINT		  *pfNullable)
{
	int namelen, i;
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	struct _hdbc *dbc = (struct _hdbc *) stmt->hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;
	MdbSQL *sql = env->sql;
	MdbSQLColumn *sqlcol;
	MdbColumn *col;
	MdbTableDef *table;

	TRACE("_SQLDescribeCol");
	if (icol<1 || icol>sql->num_columns) {
		return SQL_ERROR;
	}
	sqlcol = g_ptr_array_index(sql->columns,icol - 1);
	table = sql->cur_table;
	for (i=0;i<table->num_cols;i++) {
		col=g_ptr_array_index(table->columns,i);
		if (!strcasecmp(sqlcol->name, col->name)) {
			break;
		}
	}
	if (i==table->num_cols) {
		fprintf(stderr, "Column %s lost\n", (char*)sqlcol->name);
		return SQL_ERROR;
	}

	if (szColName) {
		namelen = MIN(cbColNameMax,strlen(sqlcol->name));
		strncpy((char*)szColName, sqlcol->name, namelen);
		szColName[namelen]='\0';
		if (pcbColName)
			*pcbColName=namelen;
	} else {
		if (pcbColName)
			*pcbColName = strlen(sqlcol->name);
	}
	if (pfSqlType) {
		*pfSqlType = _odbc_get_client_type(col);
	}
	if (pcbColDef) {
		*pcbColDef = col->col_size;
	}
	if (pibScale) {
		/* FIX ME */
		*pibScale = 0;
	}
	if (pfNullable) {
		*pfNullable = !col->is_fixed;
	}

	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLDescribeCol(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   icol,
	SQLCHAR			  *szColName,
	SQLSMALLINT		   cbColNameMax,
	SQLSMALLINT		  *pcbColName,
	SQLSMALLINT		  *pfSqlType,
	SQLULEN			  *pcbColDef, /* precision */
	SQLSMALLINT		  *pibScale,
	SQLSMALLINT		  *pfNullable)
#ifdef HAVE_ATTRIBUTE_ALIAS
__attribute__((alias("_SQLDescribeCol")));
#else
{ return _SQLDescribeCol(hstmt, icol, szColName, cbColNameMax, pcbColName, pfSqlType, pcbColDef, pibScale, pfNullable); }
#endif

#ifdef ENABLE_ODBC_W
SQLRETURN SQL_API SQLDescribeColW(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   icol,
	SQLWCHAR		   *szColName,
	SQLSMALLINT		   cbColNameMax,
	SQLSMALLINT		  *pcbColName,
	SQLSMALLINT		  *pfSqlType,
	SQLULEN			  *pcbColDef, /* precision */
	SQLSMALLINT		  *pibScale,
	SQLSMALLINT		  *pfNullable)
{
	if(cbColNameMax==SQL_NTS)cbColNameMax=sqlwlen(szColName);
	{
		SQLCHAR *tmp=calloc(cbColNameMax*4,1);
		size_t l=cbColNameMax*4;
		SQLRETURN ret = _SQLDescribeCol(hstmt, icol, tmp, cbColNameMax*4, (SQLSMALLINT*)&l, pfSqlType, pcbColDef, pibScale, pfNullable);
		ascii2unicode((char*)tmp, &l, (char*)szColName, (size_t*)pcbColName);
		*pcbColName/=sizeof(SQLWCHAR);
		free(tmp);
		return ret;
	}
}
#endif //ENABLE_ODBC_W

static SQLRETURN SQL_API _SQLColAttributes(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   icol,
	SQLUSMALLINT	   fDescType,
	SQLPOINTER		   rgbDesc,
	SQLSMALLINT		   cbDescMax,
	SQLSMALLINT		  *pcbDesc,
	SQLLEN			  *pfDesc)
{
	int namelen, i;
	struct _hstmt *stmt;
	struct _hdbc *dbc;
	struct _henv *env;
	MdbSQL *sql;
	MdbSQLColumn *sqlcol;
	MdbColumn *col;
	MdbTableDef *table;

	TRACE("_SQLColAttributes");
	stmt = (struct _hstmt *) hstmt;
	dbc = (struct _hdbc *) stmt->hdbc;
	env = (struct _henv *) dbc->henv;
	sql = env->sql;

	/* dont check column index for these */
	switch(fDescType) {
		case SQL_COLUMN_COUNT:
			return SQL_SUCCESS;
			break;
	}

	if (icol<1 || icol>sql->num_columns) {
		return SQL_ERROR;
	}

	/* find the column */
	sqlcol = g_ptr_array_index(sql->columns,icol - 1);
	table = sql->cur_table;
	for (i=0;i<table->num_cols;i++) {
		col=g_ptr_array_index(table->columns,i);
		if (!strcasecmp(sqlcol->name, col->name)) {
			break;
			}
	}
	if (i==table->num_cols) {
		return SQL_ERROR;
	}

	// fprintf(stderr,"fDescType = %d\n", fDescType);
	switch(fDescType) {
		case SQL_COLUMN_NAME:
		case SQL_COLUMN_LABEL:
			namelen = MIN(cbDescMax,strlen(sqlcol->name));
			strncpy(rgbDesc, sqlcol->name, namelen);
			((char *)rgbDesc)[namelen]='\0';
			break;
		case SQL_COLUMN_TYPE:
			*pfDesc = SQL_CHAR;
			break;
		case SQL_COLUMN_LENGTH:
			break;
		//case SQL_COLUMN_DISPLAY_SIZE:
		case SQL_DESC_DISPLAY_SIZE:
			*pfDesc = mdb_col_disp_size(col);
			break;
	}
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLColAttributes(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   icol,
	SQLUSMALLINT	   fDescType,
	SQLPOINTER		   rgbDesc,
	SQLSMALLINT		   cbDescMax,
	SQLSMALLINT		  *pcbDesc,
	SQLLEN			  *pfDesc)
#ifdef HAVE_ATTRIBUTE_ALIAS
__attribute__((alias("_SQLColAttributes")));
#else
{ return _SQLColAttributes(hstmt, icol, fDescType, rgbDesc, cbDescMax, pcbDesc, pfDesc); }
#endif

#ifdef ENABLE_ODBC_W
SQLRETURN SQL_API SQLColAttributesW(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   icol,
	SQLUSMALLINT	   fDescType,
	SQLPOINTER		   rgbDesc,
	SQLSMALLINT		   cbDescMax,
	SQLSMALLINT		  *pcbDesc,
	SQLLEN			  *pfDesc)
{
	if (fDescType!=SQL_COLUMN_NAME && fDescType!=SQL_COLUMN_LABEL)
		return _SQLColAttributes(hstmt,icol,fDescType,rgbDesc,cbDescMax,pcbDesc,pfDesc);
	else{
		SQLCHAR *tmp=calloc(cbDescMax*4,1);
		size_t l=cbDescMax*4;
		SQLRETURN ret=_SQLColAttributes(hstmt,icol,fDescType,tmp,cbDescMax*4,(SQLSMALLINT*)&l,pfDesc);
		ascii2unicode((char*)tmp, &l, (char*)rgbDesc, (size_t*)pcbDesc);
		*pcbDesc/=sizeof(SQLWCHAR);
		free(tmp);
		return ret;
	}
}
#endif //ENABLE_ODBC_W

SQLRETURN SQL_API SQLDisconnect(
	SQLHDBC			   hdbc)
{
	struct _hdbc *dbc;
	struct _henv *env;

	TRACE("SQLDisconnect");

	dbc = (struct _hdbc *) hdbc;
	env = (struct _henv *) dbc->henv;
	mdb_sql_close(env->sql);

	return SQL_SUCCESS;
}

static SQLRETURN SQL_API _SQLError(
	SQLHENV			   henv,
	SQLHDBC			   hdbc,
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szSqlState,
	SQLINTEGER		  *pfNativeError,
	SQLCHAR			  *szErrorMsg,
	SQLSMALLINT		   cbErrorMsgMax,
	SQLSMALLINT		  *pcbErrorMsg)
{
	SQLRETURN result = SQL_NO_DATA_FOUND;
   
	TRACE("_SQLError");
	//if(pfNativeError)fprintf(stderr,"NativeError %05d\n", *pfNativeError);
	if (strlen (lastError) > 0)
	{
		strcpy ((char*)szSqlState, "08001");
		strcpy ((char*)szErrorMsg, lastError);
		if (pcbErrorMsg)
			*pcbErrorMsg = strlen (lastError);
		if (pfNativeError)
			*pfNativeError = 1;

		result = SQL_SUCCESS;
		strcpy (lastError, "");
	}

	return result;
}

SQLRETURN SQL_API SQLError(
	SQLHENV			   henv,
	SQLHDBC			   hdbc,
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szSqlState,
	SQLINTEGER		  *pfNativeError,
	SQLCHAR			  *szErrorMsg,
	SQLSMALLINT		   cbErrorMsgMax,
	SQLSMALLINT		  *pcbErrorMsg)
#ifdef HAVE_ATTRIBUTE_ALIAS
__attribute__((alias("_SQLError")));
#else
{ return _SQLError(henv, hdbc, hstmt, szSqlState, pfNativeError, szErrorMsg, cbErrorMsgMax, pcbErrorMsg); }
#endif

#ifdef ENABLE_ODBC_W
SQLRETURN SQL_API SQLErrorW(
	SQLHENV			   henv,
	SQLHDBC			   hdbc,
	SQLHSTMT		   hstmt,
	SQLWCHAR		  *szSqlState,
	SQLINTEGER		  *pfNativeError,
	SQLWCHAR		  *szErrorMsg,
	SQLSMALLINT		   cbErrorMsgMax,
	SQLSMALLINT		  *pcbErrorMsg)
{
	SQLCHAR szSqlState8[6];
	SQLCHAR szErrorMsg8[3*cbErrorMsgMax+1];
	SQLSMALLINT pcbErrorMsg8;
	SQLRETURN result;

	TRACE("SQLErrorW");

	result = _SQLError(henv, hdbc, hstmt, szSqlState8, pfNativeError, szErrorMsg8, 3*cbErrorMsgMax+1, &pcbErrorMsg8);
	if (result == SQL_SUCCESS) {
		size_t l=6, z=6*sizeof(SQLWCHAR);
		ascii2unicode((char*)szSqlState8, &l, (char*)szSqlState, &z);
		l = cbErrorMsgMax;
		ascii2unicode((char*)szErrorMsg8, (size_t*)&pcbErrorMsg8, (char*)szErrorMsg, &l);
		if (pcbErrorMsg)
			*pcbErrorMsg = l;
	}
	return result;
}
#endif // ENABLE_ODBC_W

static SQLRETURN SQL_API _SQLExecute( SQLHSTMT hstmt)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	struct _hdbc *dbc = (struct _hdbc *) stmt->hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;

	TRACE("_SQLExecute");
   
	/* fprintf(stderr,"query = %s\n",stmt->query); */
	_odbc_fix_literals(stmt);

	mdb_sql_reset(env->sql);

	mdb_sql_run_query(env->sql, stmt->query);
	if (mdb_sql_has_error(env->sql)) {
		LogError("Couldn't parse SQL\n");
		mdb_sql_reset(env->sql);
		return SQL_ERROR;
	} else {
		return SQL_SUCCESS;
	}
}

static SQLRETURN SQL_API _SQLExecDirect(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szSqlStr,
	SQLINTEGER		   cbSqlStr)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;

	TRACE("_SQLExecDirect");
	strcpy(stmt->query, (char*)szSqlStr);

	return _SQLExecute(hstmt);
}

SQLRETURN SQL_API SQLExecDirect(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szSqlStr,
	SQLINTEGER		   cbSqlStr)
#ifdef HAVE_ATTRIBUTE_ALIAS
__attribute__((alias("_SQLExecDirect")));
#else
{ return _SQLExecDirect(hstmt, szSqlStr, cbSqlStr); }
#endif

#ifdef ENABLE_ODBC_W
SQLRETURN SQL_API SQLExecDirectW(
	SQLHSTMT		   hstmt,
	SQLWCHAR		   *szSqlStr,
	SQLINTEGER		   cbSqlStr)
{
	TRACE("SQLExecDirectW");
	if(cbSqlStr==SQL_NTS)cbSqlStr=sqlwlen(szSqlStr);
	{
		SQLCHAR *tmp=calloc(cbSqlStr*4,1);
		size_t l=cbSqlStr*4,z=cbSqlStr*2;
		SQLRETURN ret;
		unicode2ascii((char*)szSqlStr, &z, (char*)tmp, &l);
		ret = _SQLExecDirect(hstmt, tmp, l);
		free(tmp);
		return ret;
	}
}
#endif // ENABLE_ODBC_W

SQLRETURN SQL_API SQLExecute(
	SQLHSTMT		   hstmt)
{
	TRACE("SQLExecute");
	return _SQLExecute(hstmt);
}

static void
bind_columns(struct _hstmt *stmt)
{
	struct _hdbc *dbc = (struct _hdbc *) stmt->hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;
	struct _sql_bind_info *cur;

	TRACE("bind_columns");

	if (stmt->rows_affected==0) {
		cur = stmt->bind_head;
		while (cur) {
			if (cur->column_number>0 &&
				cur->column_number <= env->sql->num_columns) {
				mdb_sql_bind_column(env->sql, cur->column_number,
									cur->varaddr, cur->column_lenbind);
			} else {
				/* log error ? */
			}
			cur = cur->next;
		}
	}
}

static void
unbind_columns(struct _hstmt *stmt)
{
	struct _sql_bind_info *cur, *next;

	TRACE("unbind_columns");

	//Free the memory allocated for bound columns
	cur = stmt->bind_head;
	while(cur) {
		next = cur->next;
		g_free(cur);
		cur = next;
	}
	stmt->bind_head = NULL;
}

SQLRETURN SQL_API SQLFetch(
	SQLHSTMT		   hstmt)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	struct _hdbc *dbc = (struct _hdbc *) stmt->hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;

	TRACE("SQLFetch");
	/* if we bound columns, transfer them to res_info now that we have one */
	bind_columns(stmt);

	if (mdb_fetch_row(env->sql->cur_table)) {
		stmt->rows_affected++;
		return SQL_SUCCESS;
	} else {
		return SQL_NO_DATA_FOUND;
	}
}

SQLRETURN SQL_API SQLFreeHandle(
	SQLSMALLINT HandleType,
	SQLHANDLE Handle)
{
	TRACE("SQLFreeHandle");

	g_ptr_array_remove(handles, (gpointer) Handle);

	switch(HandleType) {
		case SQL_HANDLE_STMT:
			_SQLFreeStmt(Handle,SQL_DROP);
			break;
		case SQL_HANDLE_DBC:
			_SQLFreeConnect(Handle);
			break;
		case SQL_HANDLE_ENV:
			_SQLFreeEnv(Handle);
			break;
	}
   return SQL_SUCCESS;
}

static SQLRETURN SQL_API _SQLFreeConnect(
	SQLHDBC			   hdbc)
{
	struct _hdbc *dbc = (struct _hdbc *) hdbc;

	TRACE("_SQLFreeConnect");

	FreeConnectParams(dbc->params);
	g_free(dbc);

	return SQL_SUCCESS;
}
SQLRETURN SQL_API SQLFreeConnect(
	SQLHDBC			   hdbc)
{
	TRACE("SQLFreeConnect");
	return _SQLFreeConnect(hdbc);
}

static SQLRETURN SQL_API _SQLFreeEnv(
	SQLHENV			   henv)
{
	TRACE("_SQLFreeEnv");

	struct _henv *env;

	/* Should we assume that env->sql has been properly disconnected? */
	env = (struct _henv *) henv;

	mdb_sql_exit(env->sql);
	g_free(env->sql);
	g_free(env);

	refCount--;
	if (refCount == 0)
		drv_exit();

	return SQL_SUCCESS;
}
SQLRETURN SQL_API SQLFreeEnv(
	SQLHENV			   henv)
{
	TRACE("SQLFreeEnv");
	return _SQLFreeEnv(henv);
}

static SQLRETURN SQL_API _SQLFreeStmt(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   fOption)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	struct _hdbc *dbc = (struct _hdbc *) stmt->hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;
	MdbSQL *sql = env->sql;

	TRACE("_SQLFreeStmt");
	if (fOption==SQL_DROP) {
		mdb_sql_reset(sql);
		unbind_columns(stmt);
		g_free(stmt);

	} else if (fOption==SQL_CLOSE) {
		stmt->rows_affected = 0;
	} else if (fOption==SQL_UNBIND) {
		unbind_columns(stmt);
	} else if (fOption==SQL_RESET_PARAMS) {
		/* Bound parameters not currently implemented */
	} else {
	}
	return SQL_SUCCESS;
}
SQLRETURN SQL_API SQLFreeStmt(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   fOption)
{
	TRACE("SQLFreeStmt");
	return _SQLFreeStmt(hstmt, fOption);
}

SQLRETURN SQL_API SQLGetStmtAttr (
	SQLHSTMT StatementHandle,
	SQLINTEGER Attribute,
	SQLPOINTER Value,
	SQLINTEGER BufferLength,
	SQLINTEGER * StringLength)
{
	TRACE("SQLGetStmtAttr");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetCursorName(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCursor,
	SQLSMALLINT		   cbCursorMax,
	SQLSMALLINT		  *pcbCursor)
{
	TRACE("SQLGetCursorName");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLNumResultCols(
	SQLHSTMT		   hstmt,
	SQLSMALLINT		  *pccol)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	struct _hdbc *dbc = (struct _hdbc *) stmt->hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;

	TRACE("SQLNumResultCols");
	*pccol = env->sql->num_columns;
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLPrepare(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szSqlStr,
	SQLINTEGER		   cbSqlStr)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	int sqllen = _odbc_get_string_size(cbSqlStr, szSqlStr);

	TRACE("SQLPrepare");

	strncpy(stmt->query, (char*) szSqlStr, sqllen);
	stmt->query[sqllen]='\0';

	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLRowCount(
	SQLHSTMT		   hstmt,
	SQLLEN			  *pcrow)
{
struct _hstmt *stmt=(struct _hstmt *)hstmt;

	TRACE("SQLRowCount");
	*pcrow = stmt->rows_affected;
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetCursorName(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCursor,
	SQLSMALLINT		   cbCursor)
{
	TRACE("SQLSetCursorName");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLTransact(
	SQLHENV			   henv,
	SQLHDBC			   hdbc,
	SQLUSMALLINT	   fType)
{
	TRACE("SQLTransact");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetParam(			  /*	  Use SQLBindParameter */
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   ipar,
	SQLSMALLINT		   fCType,
	SQLSMALLINT		   fSqlType,
	SQLULEN			   cbParamDef,
	SQLSMALLINT		   ibScale,
	SQLPOINTER		   rgbValue,
	SQLLEN			  *pcbValue)
{
	TRACE("SQLSetParam");
	return SQL_SUCCESS;
}

static SQLRETURN SQL_API _SQLColumns(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLCHAR			  *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLCHAR			  *szTableName,
	SQLSMALLINT		   cbTableName,
	SQLCHAR			  *szColumnName,
	SQLSMALLINT		   cbColumnName)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	struct _hdbc *dbc = (struct _hdbc *) stmt->hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;
	MdbSQL *sql = env->sql;
	MdbHandle *mdb = sql->mdb;
	MdbTableDef *ttable;
	MdbField fields[18];
	unsigned char row_buffer[MDB_PGSIZE];
	int row_size;
	unsigned int i, j, k;
	MdbCatalogEntry *entry;
	MdbTableDef *table;
	MdbColumn *col;
	unsigned int ts2, ts3, ts5;
	unsigned char t2[MDB_BIND_SIZE],
				  t3[MDB_BIND_SIZE],
				  t5[MDB_BIND_SIZE];
	SQLSMALLINT nullable;  /* SQL_NULLABLE or SQL_NO_NULLS */
	SQLSMALLINT datatype;  /* For datetime, use concise data type */
	SQLSMALLINT sqldatatype;  /* For datetime, use nonconcise data type */
	SQLINTEGER ordinal;

	TRACE("_SQLColumns");

	mdb_read_catalog(mdb, MDB_ANY);

	ttable = mdb_create_temp_table(mdb, "#columns");
	mdb_sql_add_temp_col(sql, ttable, 0, "TABLE_CAT", MDB_TEXT, 128, 0);
	mdb_sql_add_temp_col(sql, ttable, 1, "TABLE_SCHEM", MDB_TEXT, 128, 0);
	mdb_sql_add_temp_col(sql, ttable, 2, "TABLE_NAME", MDB_TEXT, 128, 0);
	mdb_sql_add_temp_col(sql, ttable, 3, "COLUMN_NAME", MDB_TEXT, 128, 0);
	mdb_sql_add_temp_col(sql, ttable, 4, "DATA_TYPE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 5, "TYPE_NAME", MDB_TEXT, 128, 0);
	mdb_sql_add_temp_col(sql, ttable, 6, "COLUMN_SIZE", MDB_LONGINT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 7, "BUFFER_LENGTH", MDB_LONGINT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 8, "DECIMAL_DIGITS", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 9, "NUM_PREC_RADIX", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 10, "NULLABLE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 11, "REMARKS", MDB_TEXT, 254, 0);
	mdb_sql_add_temp_col(sql, ttable, 12, "COLUMN_DEF", MDB_TEXT, 254, 0);
	mdb_sql_add_temp_col(sql, ttable, 13, "SQL_DATA_TYPE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 14, "SQL_DATETIME_SUB", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 15, "CHAR_OCTET_LENGTH", MDB_LONGINT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 16, "ORDINAL_POSITION", MDB_LONGINT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 17, "IS_NULLABLE", MDB_TEXT, 254, 0);
	mdb_temp_columns_end(ttable);

	for (i=0; i<mdb->num_catalog; i++) {
			entry = g_ptr_array_index(mdb->catalog, i);
		/* TODO: Do more advanced matching */
		if (strcasecmp((char*)szTableName, entry->object_name) != 0)
			continue;
		table = mdb_read_table(entry);
		mdb_read_columns(table);
		for (j=0; j<table->num_cols; j++) {
			col = g_ptr_array_index(table->columns, j);

			ts2 = mdb_ascii2unicode(mdb, table->name, 0, (char*)t2, MDB_BIND_SIZE);
			ts3 = mdb_ascii2unicode(mdb, col->name, 0, (char*)t3, MDB_BIND_SIZE);
			ts5 = mdb_ascii2unicode(mdb, "FIX ME", 0,  (char*)t5, MDB_BIND_SIZE);
			nullable = SQL_NO_NULLS;
			datatype = _odbc_get_client_type(col);
			sqldatatype = _odbc_get_client_type(col);
			ordinal = j+1;

			/* Set all fields to NULL */
			for (k=0; k<18; k++) {
				FILL_FIELD(&fields[k], NULL, 0);
			}

			FILL_FIELD(&fields[2], t2, ts2);
			FILL_FIELD(&fields[3], t3, ts3);
			FILL_FIELD(&fields[4], &datatype, 0);
			FILL_FIELD(&fields[5], t5, ts5);
			FILL_FIELD(&fields[10], &nullable, 0);
			FILL_FIELD(&fields[13], &sqldatatype, 0);
			FILL_FIELD(&fields[16], &ordinal, 0);

			row_size = mdb_pack_row(ttable, row_buffer, 18, fields);
			mdb_add_row_to_pg(ttable, row_buffer, row_size);
			ttable->num_rows++;
		}
		mdb_free_tabledef(table);
	}
	sql->cur_table = ttable;

	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLColumns(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLCHAR			  *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLCHAR			  *szTableName,
	SQLSMALLINT		   cbTableName,
	SQLCHAR			  *szColumnName,
	SQLSMALLINT		   cbColumnName)
#ifdef HAVE_ATTRIBUTE_ALIAS
__attribute__((alias("_SQLColumns")));
#else
{ return _SQLColumns(hstmt, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName, cbTableName, szColumnName, cbColumnName); }
#endif

#ifdef ENABLE_ODBC_W
SQLRETURN SQL_API SQLColumnsW(
	SQLHSTMT		   hstmt,
	SQLWCHAR		   *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLWCHAR		   *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLWCHAR		   *szTableName,
	SQLSMALLINT		   cbTableName,
	SQLWCHAR		   *szColumnName,
	SQLSMALLINT		   cbColumnName)
{
	if(cbTableName==SQL_NTS)cbTableName=sqlwlen(szTableName);
	{
		SQLCHAR *tmp=calloc(cbTableName*4,1);
		size_t l=cbTableName*4,z=cbTableName*2;
		SQLRETURN ret;
		unicode2ascii((char*)szTableName, &z, (char*)tmp, &l);
		ret = _SQLColumns(hstmt, NULL, 0, NULL, 0, tmp, l, NULL, 0);
		free(tmp);
		return ret;
	}
}
#endif //ENABLE_ODBC_W

SQLRETURN SQL_API SQLGetConnectOption(
	SQLHDBC			   hdbc,
	SQLUSMALLINT	   fOption,
	SQLPOINTER		   pvParam)
{
	TRACE("SQLGetConnectOption");
	return SQL_SUCCESS;
}

static SQLRETURN SQL_API _SQLGetData(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   icol,
	SQLSMALLINT		   fCType,
	SQLPOINTER		   rgbValue,
	SQLLEN			   cbValueMax,
	SQLLEN			   *pcbValue)
{
	struct _hstmt *stmt;
	struct _hdbc *dbc;
	struct _henv *env;
	MdbSQL *sql;
	MdbHandle *mdb;
	MdbSQLColumn *sqlcol;
	MdbColumn *col;
	MdbTableDef *table;
	int i;

	TRACE("_SQLGetData");
	stmt = (struct _hstmt *) hstmt;
	dbc = (struct _hdbc *) stmt->hdbc;
	env = (struct _henv *) dbc->henv;
	sql = env->sql;
	mdb = sql->mdb;

	if (icol<1 || icol>sql->num_columns) {
		strcpy(sqlState, "07009");
		return SQL_ERROR;
	}

	sqlcol = g_ptr_array_index(sql->columns,icol - 1);
	table = sql->cur_table;
	for (i=0;i<table->num_cols;i++) {
		col=g_ptr_array_index(table->columns,i);
		if (!strcasecmp(sqlcol->name, col->name)) {
			break;
		}
	}
	if (i==table->num_cols)
		return SQL_ERROR;

	if (icol!=stmt->icol) {
		stmt->icol=icol;
		stmt->pos=0;
	}

	if (!rgbValue) {
		strcpy(sqlState, "HY009");
		return SQL_ERROR;
	}

	if (col->col_type == MDB_BOOL) {
		// bool cannot be null
		*(BOOL*)rgbValue = col->cur_value_len ? 0 : 1;
		if (pcbValue)
			*pcbValue = 1;
		return SQL_SUCCESS;
	}
	if (col->cur_value_len == 0) {
		/* When NULL data is retrieved, non-null pcbValue is
		   required */
		if (!pcbValue) {
			strcpy(sqlState, "22002");
			return SQL_ERROR;
		}
		*pcbValue = SQL_NULL_DATA;
		return SQL_SUCCESS;
	}

	switch(col->col_type) {
		case MDB_BYTE:
			*(SQLSMALLINT*)rgbValue = mdb_get_byte(mdb->pg_buf, col->cur_value_start);
			if (pcbValue)
				*pcbValue = sizeof(SQLSMALLINT);
			break;
		case MDB_INT:
			*(SQLSMALLINT*)rgbValue = (SQLSMALLINT)mdb_get_int16(mdb->pg_buf, col->cur_value_start);
			if (pcbValue)
				*pcbValue = sizeof(SQLSMALLINT);
			break;
		case MDB_LONGINT:
			*(SQLINTEGER*)rgbValue = mdb_get_int32(mdb->pg_buf, col->cur_value_start);
			if (pcbValue)
				*pcbValue = sizeof(SQLINTEGER);
			break;
		// case MDB_MONEY: TODO
		case MDB_FLOAT:
			*(float*)rgbValue = mdb_get_single(mdb->pg_buf, col->cur_value_start);
			if (pcbValue)
				*pcbValue = sizeof(float);
			break;
		case MDB_DOUBLE:
			*(double*)rgbValue = mdb_get_double(mdb->pg_buf, col->cur_value_start);
			if (pcbValue)
			  *pcbValue = sizeof(double);
			break;
		case MDB_DATETIME: ;
#if ODBCVER >= 0x0300
			struct tm tmp_t;
			mdb_date_to_tm(mdb_get_double(mdb->pg_buf, col->cur_value_start), &tmp_t);

			const char *format = mdb_col_get_prop(col, "Format");
			if (format && !strcmp(format, "Short Date")) {
				DATE_STRUCT sql_dt;
				sql_dt.year		= tmp_t.tm_year + 1900;
				sql_dt.month	= tmp_t.tm_mon + 1;
				sql_dt.day		= tmp_t.tm_mday;
				*(DATE_STRUCT*)rgbValue = sql_dt;
				if (pcbValue)
					*pcbValue = sizeof(DATE_STRUCT);
			} else {
				TIMESTAMP_STRUCT sql_ts;
				sql_ts.year		= tmp_t.tm_year + 1900;
				sql_ts.month	= tmp_t.tm_mon + 1;
				sql_ts.day		= tmp_t.tm_mday;
				sql_ts.hour		= tmp_t.tm_hour;
				sql_ts.minute	= tmp_t.tm_min;
				sql_ts.second	= tmp_t.tm_sec;
				sql_ts.fraction = 0;

				*(TIMESTAMP_STRUCT*)rgbValue = sql_ts;
				if (pcbValue)
					*pcbValue = sizeof(TIMESTAMP_STRUCT);
			}
			break;
#endif // returns text if old odbc
		default: ;
			char *str = mdb_col_to_string(mdb, mdb->pg_buf,
				col->cur_value_start, col->col_type, col->cur_value_len);
			int len = strlen(str);
			if (stmt->pos >= len) {
				free(str);
				return SQL_NO_DATA;
			}
			if (!cbValueMax) {
				if (pcbValue)
					*pcbValue = len;
				free(str);
				return SQL_SUCCESS_WITH_INFO;
			}
			if (len - stmt->pos > cbValueMax) {
				/* the buffer we were given is too small, so
				   truncate it to the size of the buffer */
				strncpy(rgbValue, str, cbValueMax);
				if (pcbValue)
					*pcbValue = cbValueMax;
				stmt->pos += cbValueMax;
				free(str);
				strcpy(sqlState, "01004"); // trunctated
				return SQL_SUCCESS_WITH_INFO;
			}
			strncpy(rgbValue, str + stmt->pos, len - stmt->pos);
			if (pcbValue)
				*pcbValue = len - stmt->pos;
			stmt->pos += len - stmt->pos;
			free(str);
			break;
	}
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetData(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   icol,
	SQLSMALLINT		   fCType,
	SQLPOINTER		   rgbValue,
	SQLLEN			   cbValueMax,
	SQLLEN			   *pcbValue)
#ifdef HAVE_ATTRIBUTE_ALIAS
__attribute__((alias("_SQLGetData")));
#else
{ return _SQLGetData(hstmt, icol, fCType, rgbValue, cbValueMax, pcbValue); }
#endif

#ifdef ENABLE_ODBC_W
SQLRETURN SQL_API SQLGetDataW(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   icol,
	SQLSMALLINT		   fCType,
	SQLPOINTER		   rgbValue,
	SQLLEN			   cbValueMax,
	SQLLEN			   *pcbValue)
{
	//todo: treat numbers correctly

	SQLCHAR *tmp=calloc(cbValueMax*4,1);
	size_t l=cbValueMax*4;
	SQLRETURN ret = _SQLGetData(hstmt, icol, fCType, tmp, cbValueMax*4, (SQLLEN*)&l);
	ascii2unicode((char*)tmp, &l, (char*)rgbValue, (size_t*)pcbValue);
	*pcbValue/=sizeof(SQLWCHAR);
	free(tmp);
	return ret;
}
#endif //ENABLE_ODBC_W

static void _set_func_exists(SQLUSMALLINT *pfExists, SQLUSMALLINT fFunction)
{
	SQLUSMALLINT	 *mod;
	mod = pfExists + (fFunction >> 4);
	*mod |= (1 << (fFunction & 0x0f));
}

SQLRETURN SQL_API SQLGetFunctions(
	SQLHDBC			   hdbc,
	SQLUSMALLINT	   fFunction,
	SQLUSMALLINT	  *pfExists)
{

	TRACE("SQLGetFunctions");
	switch (fFunction) {
#if ODBCVER >= 0x0300
		case SQL_API_ODBC3_ALL_FUNCTIONS:
			bzero(pfExists, SQL_API_ODBC3_ALL_FUNCTIONS_SIZE);
			_set_func_exists(pfExists,SQL_API_SQLALLOCCONNECT);
			_set_func_exists(pfExists,SQL_API_SQLALLOCENV);
			_set_func_exists(pfExists,SQL_API_SQLALLOCHANDLE);
			_set_func_exists(pfExists,SQL_API_SQLALLOCSTMT);
			_set_func_exists(pfExists,SQL_API_SQLBINDCOL);
			_set_func_exists(pfExists,SQL_API_SQLBINDPARAMETER);
			_set_func_exists(pfExists,SQL_API_SQLCANCEL);
			//_set_func_exists(pfExists,SQL_API_SQLCLOSECURSOR);
			_set_func_exists(pfExists,SQL_API_SQLCOLATTRIBUTE);
			_set_func_exists(pfExists,SQL_API_SQLCOLUMNS);
			_set_func_exists(pfExists,SQL_API_SQLCONNECT);
			//_set_func_exists(pfExists,SQL_API_SQLCOPYDESC);
			_set_func_exists(pfExists,SQL_API_SQLDATASOURCES);
			_set_func_exists(pfExists,SQL_API_SQLDESCRIBECOL);
			_set_func_exists(pfExists,SQL_API_SQLDISCONNECT);
			//_set_func_exists(pfExists,SQL_API_SQLENDTRAN);
			_set_func_exists(pfExists,SQL_API_SQLERROR);
			_set_func_exists(pfExists,SQL_API_SQLEXECDIRECT);
			_set_func_exists(pfExists,SQL_API_SQLEXECUTE);
			_set_func_exists(pfExists,SQL_API_SQLFETCH);
			//_set_func_exists(pfExists,SQL_API_SQLFETCHSCROLL);
			_set_func_exists(pfExists,SQL_API_SQLFREECONNECT);
			_set_func_exists(pfExists,SQL_API_SQLFREEENV);
			_set_func_exists(pfExists,SQL_API_SQLFREEHANDLE);
			_set_func_exists(pfExists,SQL_API_SQLFREESTMT);
			//_set_func_exists(pfExists,SQL_API_SQLGETCONNECTATTR);
			_set_func_exists(pfExists,SQL_API_SQLGETCONNECTOPTION);
			_set_func_exists(pfExists,SQL_API_SQLGETCURSORNAME);
			_set_func_exists(pfExists,SQL_API_SQLGETDATA);
			//_set_func_exists(pfExists,SQL_API_SQLGETDESCFIELD);
			//_set_func_exists(pfExists,SQL_API_SQLGETDESCREC);
			//_set_func_exists(pfExists,SQL_API_SQLGETDIAGFIELD);
			//_set_func_exists(pfExists,SQL_API_SQLGETDIAGREC);
			//_set_func_exists(pfExists,SQL_API_SQLGETENVATTR);
			_set_func_exists(pfExists,SQL_API_SQLGETFUNCTIONS);
			_set_func_exists(pfExists,SQL_API_SQLGETINFO);
			_set_func_exists(pfExists,SQL_API_SQLGETSTMTATTR);
			_set_func_exists(pfExists,SQL_API_SQLGETSTMTOPTION);
			_set_func_exists(pfExists,SQL_API_SQLGETTYPEINFO);
			_set_func_exists(pfExists,SQL_API_SQLNUMRESULTCOLS);
			_set_func_exists(pfExists,SQL_API_SQLPARAMDATA);
			_set_func_exists(pfExists,SQL_API_SQLPREPARE);
			_set_func_exists(pfExists,SQL_API_SQLPUTDATA);
			_set_func_exists(pfExists,SQL_API_SQLROWCOUNT);
			//_set_func_exists(pfExists,SQL_API_SQLSETCONNECTATTR);
			_set_func_exists(pfExists,SQL_API_SQLSETCONNECTOPTION);
			_set_func_exists(pfExists,SQL_API_SQLSETCURSORNAME);
			//_set_func_exists(pfExists,SQL_API_SQLSETDESCFIELD);
			//_set_func_exists(pfExists,SQL_API_SQLSETDESCREC);
			_set_func_exists(pfExists,SQL_API_SQLSETENVATTR);
			_set_func_exists(pfExists,SQL_API_SQLSETPARAM);
			//_set_func_exists(pfExists,SQL_API_SQLSETSTMTATTR);
			_set_func_exists(pfExists,SQL_API_SQLSETSTMTOPTION);
			_set_func_exists(pfExists,SQL_API_SQLSPECIALCOLUMNS);
			_set_func_exists(pfExists,SQL_API_SQLSTATISTICS);
			_set_func_exists(pfExists,SQL_API_SQLTABLES);
			_set_func_exists(pfExists,SQL_API_SQLTRANSACT);
			break;
#endif
		case SQL_API_ALL_FUNCTIONS:
			bzero(pfExists, 100); // 100 by spec
			_set_func_exists(pfExists,SQL_API_SQLALLOCCONNECT);
			_set_func_exists(pfExists,SQL_API_SQLALLOCENV);
			_set_func_exists(pfExists,SQL_API_SQLALLOCSTMT);
			_set_func_exists(pfExists,SQL_API_SQLBINDCOL);
			_set_func_exists(pfExists,SQL_API_SQLCANCEL);
			_set_func_exists(pfExists,SQL_API_SQLCOLATTRIBUTE);
			_set_func_exists(pfExists,SQL_API_SQLCOLUMNS);
			_set_func_exists(pfExists,SQL_API_SQLCONNECT);
			_set_func_exists(pfExists,SQL_API_SQLDATASOURCES);
			_set_func_exists(pfExists,SQL_API_SQLDESCRIBECOL);
			_set_func_exists(pfExists,SQL_API_SQLDISCONNECT);
			_set_func_exists(pfExists,SQL_API_SQLERROR);
			_set_func_exists(pfExists,SQL_API_SQLEXECDIRECT);
			_set_func_exists(pfExists,SQL_API_SQLEXECUTE);
			_set_func_exists(pfExists,SQL_API_SQLFETCH);
			_set_func_exists(pfExists,SQL_API_SQLFREECONNECT);
			_set_func_exists(pfExists,SQL_API_SQLFREEENV);
			_set_func_exists(pfExists,SQL_API_SQLFREEHANDLE);
			_set_func_exists(pfExists,SQL_API_SQLFREESTMT);
			_set_func_exists(pfExists,SQL_API_SQLGETCONNECTOPTION);
			_set_func_exists(pfExists,SQL_API_SQLGETCURSORNAME);
			_set_func_exists(pfExists,SQL_API_SQLGETDATA);
			_set_func_exists(pfExists,SQL_API_SQLGETFUNCTIONS);
			_set_func_exists(pfExists,SQL_API_SQLGETINFO);
			_set_func_exists(pfExists,SQL_API_SQLGETSTMTATTR);
			_set_func_exists(pfExists,SQL_API_SQLGETSTMTOPTION);
			_set_func_exists(pfExists,SQL_API_SQLGETTYPEINFO);
			_set_func_exists(pfExists,SQL_API_SQLNUMRESULTCOLS);
			_set_func_exists(pfExists,SQL_API_SQLPARAMDATA);
			_set_func_exists(pfExists,SQL_API_SQLPREPARE);
			_set_func_exists(pfExists,SQL_API_SQLPUTDATA);
			_set_func_exists(pfExists,SQL_API_SQLROWCOUNT);
			_set_func_exists(pfExists,SQL_API_SQLSETCONNECTOPTION);
			_set_func_exists(pfExists,SQL_API_SQLSETCURSORNAME);
			_set_func_exists(pfExists,SQL_API_SQLSETENVATTR);
			_set_func_exists(pfExists,SQL_API_SQLSETPARAM);
			_set_func_exists(pfExists,SQL_API_SQLSETSTMTOPTION);
			_set_func_exists(pfExists,SQL_API_SQLSPECIALCOLUMNS);
			_set_func_exists(pfExists,SQL_API_SQLSTATISTICS);
			_set_func_exists(pfExists,SQL_API_SQLTABLES);
			_set_func_exists(pfExists,SQL_API_SQLTRANSACT);
			break;

		case SQL_API_SQLALLOCCONNECT:
		case SQL_API_SQLALLOCENV:
		case SQL_API_SQLALLOCSTMT:
		case SQL_API_SQLBINDCOL:
		case SQL_API_SQLCANCEL:
		case SQL_API_SQLCOLATTRIBUTE:
		case SQL_API_SQLCOLUMNS:
		case SQL_API_SQLCONNECT:
		case SQL_API_SQLDATASOURCES:
		case SQL_API_SQLDESCRIBECOL:
		case SQL_API_SQLDISCONNECT:
		case SQL_API_SQLERROR:
		case SQL_API_SQLEXECDIRECT:
		case SQL_API_SQLEXECUTE:
		case SQL_API_SQLFETCH:
		case SQL_API_SQLFREECONNECT:
		case SQL_API_SQLFREEENV:
		case SQL_API_SQLFREEHANDLE:
		case SQL_API_SQLFREESTMT:
		case SQL_API_SQLGETCONNECTOPTION:
		case SQL_API_SQLGETCURSORNAME:
		case SQL_API_SQLGETDATA:
		case SQL_API_SQLGETFUNCTIONS:
		case SQL_API_SQLGETINFO:
		case SQL_API_SQLGETSTMTATTR:
		case SQL_API_SQLGETSTMTOPTION:
		case SQL_API_SQLGETTYPEINFO:
		case SQL_API_SQLNUMRESULTCOLS:
		case SQL_API_SQLPARAMDATA:
		case SQL_API_SQLPREPARE:
		case SQL_API_SQLPUTDATA:
		case SQL_API_SQLROWCOUNT:
		case SQL_API_SQLSETCONNECTOPTION:
		case SQL_API_SQLSETCURSORNAME:
		case SQL_API_SQLSETENVATTR:
		case SQL_API_SQLSETPARAM:
		case SQL_API_SQLSETSTMTOPTION:
		case SQL_API_SQLSPECIALCOLUMNS:
		case SQL_API_SQLSTATISTICS:
		case SQL_API_SQLTABLES:
		case SQL_API_SQLTRANSACT:
			*pfExists = 1; /* SQL_TRUE */
			break;

		default:
			*pfExists = 0; /* SQL_FALSE */
			break;
	}
	return SQL_SUCCESS;
}

static SQLRETURN SQL_API _SQLGetInfo(
	SQLHDBC			   hdbc,
	SQLUSMALLINT	   fInfoType,
	SQLPOINTER		   rgbInfoValue,
	SQLSMALLINT		   cbInfoValueMax,
	SQLSMALLINT		  *pcbInfoValue)
{
	TRACE("_SQLGetInfo");
	switch (fInfoType) {
	case SQL_MAX_STATEMENT_LEN:
		*((SQLUINTEGER *)rgbInfoValue) = (SQLUINTEGER) 65000;
		//pcbInfoValue seems to be coming in NULL sometimes
		if(pcbInfoValue)
			*pcbInfoValue = sizeof(SQLUINTEGER);
		break;
	case SQL_SCHEMA_USAGE:
		*((SQLSMALLINT *)rgbInfoValue) = (SQLSMALLINT) 0;
		if(pcbInfoValue)
			*pcbInfoValue = sizeof(SQLSMALLINT);
		break;
	case SQL_CATALOG_NAME_SEPARATOR:
		memcpy(rgbInfoValue,".",1);
		if(pcbInfoValue)
			*pcbInfoValue = 1;
		break;
	case SQL_CATALOG_LOCATION:
		*((SQLSMALLINT *)rgbInfoValue) = (SQLSMALLINT) 1;
		if(pcbInfoValue)
			*pcbInfoValue = sizeof(SQLSMALLINT);
		break;
	case SQL_IDENTIFIER_QUOTE_CHAR:
		memcpy(rgbInfoValue,"\"",1);
		if(pcbInfoValue)
			*pcbInfoValue = 1;
		break;
	case SQL_DBMS_NAME:
		if(pcbInfoValue)
			*pcbInfoValue = 8;
		break;
	}

	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetInfo(
	SQLHDBC			   hdbc,
	SQLUSMALLINT	   fInfoType,
	SQLPOINTER		   rgbInfoValue,
	SQLSMALLINT		   cbInfoValueMax,
	SQLSMALLINT		  *pcbInfoValue)
#ifdef HAVE_ATTRIBUTE_ALIAS
__attribute__((alias("_SQLGetInfo")));
#else
{ return _SQLGetInfo(hdbc, fInfoType, rgbInfoValue, cbInfoValueMax, pcbInfoValue); }
#endif

#ifdef ENABLE_ODBC_W
SQLRETURN SQL_API SQLGetInfoW(
	SQLHDBC			   hdbc,
	SQLUSMALLINT	   fInfoType,
	SQLPOINTER		   rgbInfoValue,
	SQLSMALLINT		   cbInfoValueMax,
	SQLSMALLINT	  *pcbInfoValue)
{
	TRACE("SQLGetInfoW");

	if(fInfoType==SQL_MAX_STATEMENT_LEN||fInfoType==SQL_SCHEMA_USAGE||fInfoType==SQL_CATALOG_LOCATION)
		return _SQLGetInfo(hdbc,fInfoType,rgbInfoValue,cbInfoValueMax,pcbInfoValue);

	SQLCHAR *tmp=calloc(cbInfoValueMax*4,1);
	size_t l=cbInfoValueMax*4;
	SQLRETURN ret = _SQLGetInfo(hdbc, fInfoType, tmp, cbInfoValueMax*4,(SQLSMALLINT*)&l);
	size_t pcb=cbInfoValueMax;
	ascii2unicode((char*)tmp, &l, (char*)rgbInfoValue, &pcb);
	pcb/=sizeof(SQLWCHAR);
	if(pcbInfoValue)*pcbInfoValue=pcb;
	free(tmp);
	return ret;
}
#endif //ENABLE_ODBC_W

SQLRETURN SQL_API SQLGetStmtOption(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   fOption,
	SQLPOINTER		   pvParam)
{
	TRACE("SQLGetStmtOption");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetTypeInfo(
	SQLHSTMT		   hstmt,
	SQLSMALLINT		   fSqlType)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	struct _hdbc *dbc = (struct _hdbc *) stmt->hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;
	MdbTableDef *ttable;
	MdbSQL *sql = env->sql;
	MdbHandle *mdb = sql->mdb;
	int row_size;
	unsigned char row_buffer[MDB_PGSIZE];
	unsigned int ts0, ts3, ts4, ts5, ts12;
	unsigned char t0[MDB_BIND_SIZE],
				  t3[MDB_BIND_SIZE],
				  t4[MDB_BIND_SIZE],
				  t5[MDB_BIND_SIZE],
				  t12[MDB_BIND_SIZE];
	int i;
	MdbField fields[NUM_TYPE_INFO_COLS];

	TRACE("SQLGetTypeInfo");
	
	ttable = mdb_create_temp_table(mdb, "#typeinfo");
	mdb_sql_add_temp_col(sql, ttable, 0, "TYPE_NAME", MDB_TEXT, 30, 0);
	mdb_sql_add_temp_col(sql, ttable, 1, "DATA_TYPE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 2, "COLUMN_SIZE", MDB_LONGINT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 3, "LITERAL_PREFIX", MDB_TEXT, 30, 0);
	mdb_sql_add_temp_col(sql, ttable, 4, "LITERAL_SUFFIX", MDB_TEXT, 30, 0);
	mdb_sql_add_temp_col(sql, ttable, 5, "CREATE_PARAMS", MDB_TEXT, 30, 0);
	mdb_sql_add_temp_col(sql, ttable, 6, "NULLABLE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 7, "CASE_SENSITIVE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 8, "SEARCHABLE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 9, "UNSIGNED_ATTRIBUTE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 10, "FIXED_PREC_SCALE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 11, "AUTO_UNIQUE_VALUE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 12, "LOCAL_TYPE_NAME", MDB_TEXT, 30, 0);
	mdb_sql_add_temp_col(sql, ttable, 13, "MINIMUM_SCALE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 14, "MAXIMUM_SCALE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 15, "SQL_DATA_TYPE", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 16, "SQL_DATETIME_SUB", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 17, "NUM_PREC_RADIX", MDB_INT, 0, 1);
	mdb_sql_add_temp_col(sql, ttable, 18, "INTERVAL_PRECISION", MDB_INT, 0, 1);
	mdb_temp_columns_end(ttable);

	for (i=0; i<MAX_TYPE_INFO; i++) {
		if (fSqlType && (fSqlType != type_info[i].data_type))
			continue;

		ts0 = mdb_ascii2unicode(mdb, (char*)type_info[i].type_name, 0, (char*)t0, MDB_BIND_SIZE);
		ts3 = mdb_ascii2unicode(mdb, (char*)type_info[i].literal_prefix, 0, (char*)t3, MDB_BIND_SIZE);
		ts4 = mdb_ascii2unicode(mdb, (char*)type_info[i].literal_suffix, 0, (char*)t4, MDB_BIND_SIZE);
		ts5 = mdb_ascii2unicode(mdb, (char*)type_info[i].create_params, 0, (char*)t5, MDB_BIND_SIZE);
		ts12 = mdb_ascii2unicode(mdb, (char*)type_info[i].local_type_name, 0, (char*)t12, MDB_BIND_SIZE);

		FILL_FIELD(&fields[0], t0, ts0);
		FILL_FIELD(&fields[1],&type_info[i].data_type, 0);
		FILL_FIELD(&fields[2],&type_info[i].column_size, 0);
		FILL_FIELD(&fields[3], t3, ts3);
		FILL_FIELD(&fields[4], t4, ts4);
		FILL_FIELD(&fields[5], t5, ts5);
		FILL_FIELD(&fields[6],&type_info[i].nullable, 0);
		FILL_FIELD(&fields[7],&type_info[i].case_sensitive, 0);
		FILL_FIELD(&fields[8],&type_info[i].searchable, 0);
		FILL_FIELD(&fields[9],type_info[i].unsigned_attribute, 0);
		FILL_FIELD(&fields[10],&type_info[i].fixed_prec_scale, 0);
		FILL_FIELD(&fields[11],&type_info[i].auto_unique_value, 0);
		FILL_FIELD(&fields[12], t12, ts12);
		FILL_FIELD(&fields[13],&type_info[i].minimum_scale, 0);
		FILL_FIELD(&fields[14],&type_info[i].maximum_scale, 0);
		FILL_FIELD(&fields[15],&type_info[i].sql_data_type, 0);
		FILL_FIELD(&fields[16],type_info[i].sql_datetime_sub, 0);
		FILL_FIELD(&fields[17],type_info[i].num_prec_radix, 0);
		FILL_FIELD(&fields[18],type_info[i].interval_precision, 0);

		row_size = mdb_pack_row(ttable, row_buffer, NUM_TYPE_INFO_COLS, fields);
		mdb_add_row_to_pg(ttable,row_buffer, row_size);
		ttable->num_rows++;
	}
	sql->cur_table = ttable;
	
	/* return _SQLExecute(hstmt); */
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLParamData(
	SQLHSTMT		   hstmt,
	SQLPOINTER		  *prgbValue)
{
	TRACE("SQLParamData");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLPutData(
	SQLHSTMT		   hstmt,
	SQLPOINTER		   rgbValue,
	SQLLEN			   cbValue)
{
	TRACE("SQLPutData");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetConnectOption(
	SQLHDBC			   hdbc,
	SQLUSMALLINT	   fOption,
	SQLULEN			   vParam)
{
	TRACE("SQLSetConnectOption");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetStmtOption(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   fOption,
	SQLULEN			   vParam)
{
	TRACE("SQLSetStmtOption");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSpecialColumns(
	SQLHSTMT		   hstmt,
	SQLUSMALLINT	   fColType,
	SQLCHAR			  *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLCHAR			  *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLCHAR			  *szTableName,
	SQLSMALLINT		   cbTableName,
	SQLUSMALLINT	   fScope,
	SQLUSMALLINT	   fNullable)
{
	TRACE("SQLSpecialColumns");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLStatistics(
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLCHAR			  *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLCHAR			  *szTableName,
	SQLSMALLINT		   cbTableName,
	SQLUSMALLINT	   fUnique,
	SQLUSMALLINT	   fAccuracy)
{
	TRACE("SQLStatistics");
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLTables( //sz* not used, so Unicode API not required.
	SQLHSTMT		   hstmt,
	SQLCHAR			  *szCatalogName,
	SQLSMALLINT		   cbCatalogName,
	SQLCHAR			  *szSchemaName,
	SQLSMALLINT		   cbSchemaName,
	SQLCHAR			  *szTableName,
	SQLSMALLINT		   cbTableName,
	SQLCHAR			  *szTableType,
	SQLSMALLINT		   cbTableType)
{
	struct _hstmt *stmt = (struct _hstmt *) hstmt;
	struct _hdbc *dbc = (struct _hdbc *) stmt->hdbc;
	struct _henv *env = (struct _henv *) dbc->henv;
	MdbSQL *sql = env->sql;
	MdbHandle *mdb = sql->mdb;
	MdbTableDef *ttable;
	MdbField fields[5];
	MdbCatalogEntry *entry;
	unsigned char row_buffer[MDB_PGSIZE];
	char *table_types[] = {"TABLE", "SYSTEM TABLE", "VIEW"};
	unsigned int i, j, row_size, ttype;
	unsigned int ts2, ts3;
	unsigned char t2[MDB_BIND_SIZE],
				  t3[MDB_BIND_SIZE];

	TRACE("SQLTables");

	mdb_read_catalog(mdb, MDB_ANY);

	ttable = mdb_create_temp_table(mdb, "#tables");
	mdb_sql_add_temp_col(sql, ttable, 0, "TABLE_CAT", MDB_TEXT, 128, 0);
	mdb_sql_add_temp_col(sql, ttable, 1, "TABLE_SCHEM", MDB_TEXT, 128, 0);
	mdb_sql_add_temp_col(sql, ttable, 2, "TABLE_NAME", MDB_TEXT, 128, 0);
	mdb_sql_add_temp_col(sql, ttable, 3, "TABLE_TYPE", MDB_TEXT, 128, 0);
	mdb_sql_add_temp_col(sql, ttable, 4, "REMARKS", MDB_TEXT, 254, 0);
	mdb_temp_columns_end(ttable);

	/* TODO: Sort the return list by TYPE, CAT, SCHEM, NAME */
	for (i=0; i<mdb->num_catalog; i++) {
			entry = g_ptr_array_index(mdb->catalog, i);

		if (mdb_is_user_table(entry))
			ttype = 0;
		else if (mdb_is_system_table(entry))
			ttype = 1;
		else if (entry->object_type == MDB_QUERY)
			ttype = 2;
		else
			continue;

		/* Set all fields to NULL */
		for (j=0; j<5; j++) {
			FILL_FIELD(&fields[j], NULL, 0);
		}

		ts2 = mdb_ascii2unicode(mdb, entry->object_name, 0, (char*)t2, MDB_BIND_SIZE);
		ts3 = mdb_ascii2unicode(mdb, table_types[ttype], 0, (char*)t3, MDB_BIND_SIZE);

		FILL_FIELD(&fields[2], t2, ts2);
		FILL_FIELD(&fields[3], t3, ts3);
		
		row_size = mdb_pack_row(ttable, row_buffer, 5, fields);
		mdb_add_row_to_pg(ttable, row_buffer, row_size);
		ttable->num_rows++;
	}
	sql->cur_table = ttable;

	return SQL_SUCCESS;
}


SQLRETURN SQL_API SQLDataSources(
	SQLHENV			   henv,
	SQLUSMALLINT	   fDirection,
	SQLCHAR			  *szDSN,
	SQLSMALLINT		   cbDSNMax,
	SQLSMALLINT		  *pcbDSN,
	SQLCHAR			  *szDescription,
	SQLSMALLINT		   cbDescriptionMax,
	SQLSMALLINT		  *pcbDescription)
{
	TRACE("SQLDataSources");
	return SQL_SUCCESS;
}

static int _odbc_fix_literals(struct _hstmt *stmt)
{
	char tmp[4096],begin_tag[11];
	char *s, *d, *p;
	int i, quoted = 0, find_end = 0;
	char quote_char;

	s=stmt->query;
	d=tmp;
	while (*s) {
		if (!quoted && (*s=='"' || *s=='\'')) {
			quoted = 1;
			quote_char = *s;
		} else if (quoted && *s==quote_char) {
			quoted = 0;
		}
		if (!quoted && find_end && *s=='}') {
			s++; /* ignore the end of tag */
		} else if (!quoted && *s=='{') {
			for (p=s,i=0;*p && *p!=' ';p++) i++;
			if (i>10) {
				/* garbage */
				*d++=*s++;
			} else {
				strncpy(begin_tag, s, i);
				begin_tag[i] = '\0';
				/* printf("begin tag %s\n", begin_tag); */
				s += i;
				find_end = 1;
			}
		} else {
			*d++=*s++;	
		}
	}
	*d='\0';
	strcpy(stmt->query,tmp);

	return 0;
}

static int _odbc_get_string_size(int size, SQLCHAR *str)
{
	if (!str) {
		return 0;
	}
	if (size==SQL_NTS) {
		return strlen((char*)str);
	} else {
		return size;
	}
	return 0;
}
/*
static int _odbc_get_server_type(int clt_type)
{
	switch (clt_type) {
	case SQL_CHAR:
	case SQL_VARCHAR:
	case SQL_BIT:
	case SQL_TINYINT:
	case SQL_SMALLINT:
	case SQL_INTEGER:
	case SQL_DOUBLE:
	case SQL_DECIMAL:
	case SQL_NUMERIC:
	case SQL_FLOAT:
	default:
		break;
	}
	return 0;
}*/
static SQLSMALLINT _odbc_get_client_type(MdbColumn *col)
{
	switch (col->col_type) {
		case MDB_BOOL:
			return SQL_BIT;
		case MDB_BYTE:
			return SQL_TINYINT;
		case MDB_INT:
			return SQL_SMALLINT;
		case MDB_LONGINT:
			return SQL_INTEGER;
		case MDB_MONEY:
			return SQL_DECIMAL;
		case MDB_FLOAT:
			return SQL_FLOAT;
		case MDB_DOUBLE:
			return SQL_DOUBLE;
		case MDB_DATETIME: ;
#if ODBCVER >= 0x0300
			const char *format = mdb_col_get_prop(col, "Format");
			if (format && !strcmp(format, "Short Date"))
				return SQL_TYPE_DATE;
			else
				return SQL_TYPE_TIMESTAMP;
#endif // returns text otherwise
		case MDB_TEXT:
			return SQL_VARCHAR;
		default:
			// fprintf(stderr,"Unknown type %d\n",srv_type);
			break;
	}
	return -1;
}
