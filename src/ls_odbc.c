/*
** LuaSQL, ODBC driver
** Authors: Pedro Rabinovitch, Roberto Ierusalimschy, Diego Nehab,
** Tomas Guisasola
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(WIN32)
#include <windows.h>
#include <sqlext.h>
#elif defined(INFORMIX)
#include <infxcli.h>
#endif

#include <lua.h>
#include <lauxlib.h>

#include "luasql.h"
#include "ls_odbc.h"

#define LUASQL_ENVIRONMENT_ODBC "ODBC environment"
#define LUASQL_CONNECTION_ODBC "ODBC connection"
#define LUASQL_CURSOR_ODBC "ODBC cursor"


typedef struct {
	short closed;
	unsigned conn_counter;    /* active connections counter */
    SQLHENV henv;             /* environment handle */
} env_data;


typedef struct {
	short closed;
	unsigned cur_counter;     /* active cursors counter */
    int env;                  /* reference to environment */
    int auto_commit;          /* 0 for manual commit */
    SQLHDBC hdbc;             /* database connection handle */
} conn_data;


typedef struct {
	short closed;
    int conn;                 /* reference to connection */
    int numcols;              /* number of columns */
    SQLHSTMT hstmt;           /* statement handle */
	int colinfo;              /* reference to column information table */
} cur_data;

/* we are lazy */
#define hENV SQL_HANDLE_ENV
#define hSTMT SQL_HANDLE_STMT
#define hDBC SQL_HANDLE_DBC
#define error(a) ((a) != SQL_SUCCESS && (a) != SQL_SUCCESS_WITH_INFO) 


/*
** Check for valid environment.
*/
static env_data *getenvironment (lua_State *L) {
	env_data *env = (env_data *)luaL_checkudata (L, 1, LUASQL_ENVIRONMENT_ODBC);
	luaL_argcheck (L, env != NULL, 1, "environment expected");
	luaL_argcheck (L, !env->closed, 1, "environment is closed");
	return env;
}


/*
** Check for valid connection.
*/
static conn_data *getconnection (lua_State *L) {
	conn_data *conn = (conn_data *)luaL_checkudata (L, 1, LUASQL_CONNECTION_ODBC);
	luaL_argcheck (L, conn != NULL, 1, "connection expected");
	luaL_argcheck (L, !conn->closed, 1, "connection is closed");
	return conn;
}


/*
** Check for valid cursor.
*/
static cur_data *getcursor (lua_State *L) {
	cur_data *cursor = (cur_data *)luaL_checkudata (L, 1, LUASQL_CURSOR_ODBC);
	luaL_argcheck (L, cursor != NULL, 1, "cursor expected");
	luaL_argcheck (L, !cursor->closed, 1, "cursor is closed");
	return cursor;
}


/*
** Pushes 1 and returns 1
*/
static int pass(lua_State *L) {
    lua_pushnumber(L, 1);
    return 1;
}


/*
** Fails with error message from ODBC
** Inputs: 
**   type: type of handle used in operation
**   handle: handle used in operation
*/
static int fail(lua_State *L,  const SQLSMALLINT type, const SQLHANDLE handle) {
    SQLCHAR State[6];
    SQLINTEGER NativeError;
    SQLSMALLINT MsgSize, i;
    SQLRETURN ret;
    char Msg[SQL_MAX_MESSAGE_LENGTH];
    luaL_Buffer b;
    lua_pushnil(L);

    luaL_buffinit(L, &b);
    i = 1;
    while (1) {
        ret = SQLGetDiagRec(type, handle, i, State, &NativeError, Msg, 
                sizeof(Msg), &MsgSize);
        if (ret == SQL_NO_DATA) break;
        luaL_addlstring(&b, Msg, MsgSize);
        luaL_putchar(&b, '\n');
        i++;
    } 
    luaL_pushresult(&b);
    return 2;
}

/*
** Returns the name of an equivalent lua type for a SQL type.
*/
static const char *sqltypetolua (const SQLSMALLINT type) {
    switch (type) {
        case SQL_UNKNOWN_TYPE: case SQL_CHAR: case SQL_VARCHAR: 
        case SQL_TYPE_DATE: case SQL_TYPE_TIME: case SQL_TYPE_TIMESTAMP: 
        case SQL_DATE: case SQL_INTERVAL: case SQL_TIMESTAMP: 
        case SQL_LONGVARCHAR:
            return "string";
        case SQL_BIGINT: case SQL_TINYINT: case SQL_NUMERIC: 
        case SQL_DECIMAL: case SQL_INTEGER: case SQL_SMALLINT: 
        case SQL_FLOAT: case SQL_REAL: case SQL_DOUBLE:
            return "number";
        case SQL_BINARY: case SQL_VARBINARY: case SQL_LONGVARBINARY:
            return "binary";	/* !!!!!! nao seria string? */
        case SQL_BIT:
            return "boolean";
        default:
            assert(0);
            return NULL;
    }
}


/*
** Retrieves data from the i_th column in the current row
** Input
**   types: index in stack of column types table
**   hstmt: statement handle
**   i: column number
** Returns:
**   0 if successfull, non-zero otherwise;
*/
static int push_column(lua_State *L, int colinfo, const SQLHSTMT hstmt, 
        SQLUSMALLINT i) {
    const char *tname;
    char type;
    /* get column type from types table */
	lua_rawgeti (L, LUA_REGISTRYINDEX, colinfo);	/* ColInfo table */
	lua_rawgeti (L, -1, i);	/* name of the column */
	lua_rawget (L, -2);	/* typename of the column */
    tname = lua_tostring(L, -1);
    if (!tname)
		return luasql_faildirect(L, LUASQL_PREFIX"Invalid type in table.");
    type = tname[1];
    lua_pop(L, 2); /* type and ColInfo table */
    /* deal with data according to type */
    switch (type) {
        /* nUmber */
        case 'u': { 
              double num;
              SQLINTEGER got;
              SQLRETURN rc = SQLGetData(hstmt, i, SQL_C_DOUBLE, &num, 0, &got);
              if (error(rc)) return fail(L, hSTMT, hstmt);
              if (got == SQL_NULL_DATA) lua_pushnil(L);
              else lua_pushnumber(L, num);
              return 0;
          }
                  /* bOol */
        case 'o': { 
              char b;
              SQLINTEGER got;
              SQLRETURN rc = SQLGetData(hstmt, i, SQL_C_BIT, &b, 0, &got);
              if (error(rc)) return fail(L, hSTMT, hstmt);
              if (got == SQL_NULL_DATA) lua_pushnil(L);
              else lua_pushstring(L, b ? "true" : "false");
              return 0;
          }
        /* sTring */
        case 't': 
        /* bInary */
        case 'i': { 
              SQLSMALLINT stype = (type == 't') ? SQL_C_CHAR : SQL_C_BINARY;
              SQLINTEGER got;
              char *buffer;
              luaL_Buffer b;
              SQLRETURN rc;
              luaL_buffinit(L, &b);
              buffer = luaL_prepbuffer(&b);
              rc = SQLGetData(hstmt, i, stype, buffer, LUAL_BUFFERSIZE, &got);
              if (got == SQL_NULL_DATA) {
                  lua_pushnil(L);
                  return 0;
              }
              /* concat intermediary chunks */
              while (rc == SQL_SUCCESS_WITH_INFO) {
                  if (got >= LUAL_BUFFERSIZE || got == SQL_NO_TOTAL) {
                      got = LUAL_BUFFERSIZE;
                      /* get rid of null termination in string block */
                      if (stype == SQL_C_CHAR) got--;
                  }
                  luaL_addsize(&b, got);
                  buffer = luaL_prepbuffer(&b);
                  rc = SQLGetData(hstmt, i, stype, buffer, 
                          LUAL_BUFFERSIZE, &got);
              }
              /* concat last chunk */
              if (rc == SQL_SUCCESS) {
                  if (got >= LUAL_BUFFERSIZE || got == SQL_NO_TOTAL) {
                      got = LUAL_BUFFERSIZE;
                      /* get rid of null termination in string block */
                      if (stype == SQL_C_CHAR) got--;
                  }
                  luaL_addsize(&b, got);
              }
              if (rc == SQL_ERROR) return fail(L, hSTMT, hstmt);
              /* return everything we got */
              luaL_pushresult(&b);
              return 0;
          }
    }
    return 0;
}

/*
** Get another row of the given cursor.
*/
static int cur_fetch (lua_State *L) {
    cur_data *cur = (cur_data *) getcursor (L);
    SQLHSTMT hstmt = cur->hstmt;
    int row, types, names;
    int ret; 
    SQLRETURN rc = SQLFetch(cur->hstmt); 
    if (rc == SQL_NO_DATA) {
        lua_pushnil(L);
        return 1;
    } else if (error(rc)) fail(L, hSTMT, hstmt);

    /* push column type and name table */
/*
    lua_getref(L, curdata->coltypes); types = lua_gettop(L);
    lua_getref(L, curdata->colnames); names = lua_gettop(L);
*/
	if (lua_istable (L, 2)) {
		SQLUSMALLINT i;
		const char *opts = luaL_optstring (L, 3, "n");
		if (strchr (opts, 'n') != NULL)
			/* Copy values to numerical indices */
			for (i = 1; i <= cur->numcols; i++) {
				ret = push_column (L, cur->colinfo, hstmt, i);
				if (ret)
					return ret;
				lua_rawseti (L, 2, i);
			}
		if (strchr (opts, 'a') != NULL)
			/* Copy values to alphanumerical indices */
			for (i = 1; i <= cur->numcols; i++) {
				lua_rawgeti (L, LUA_REGISTRYINDEX, cur->colinfo);
				lua_rawgeti (L, -1, i);
				ret = push_column (L, cur->colinfo, hstmt, i);
				if (ret)
					return ret;
				lua_rawseti (L, 2, i);
			}
		lua_pushvalue (L, 2);
		return 1;	/* return table */
	}
	else {
		SQLUSMALLINT i;
		for (i = 1; i <= cur->numcols; i++) {
			ret = push_column (L, cur->colinfo, hstmt, i);
			if (ret)
				return ret;
		}
		return cur->numcols;
	}
}

/*
** Closes a cursor.
*/
static int cur_close (lua_State *L) {
	conn_data *conn;
	cur_data *cur = (cur_data *) luaL_checkudata (L, 1, LUASQL_CURSOR_ODBC);
	SQLHSTMT hstmt = cur->hstmt;
	SQLRETURN ret;
	if (cur->closed)
		return 0;

	/* Decrement parent's cursor counter. */
	lua_rawgeti (L, LUA_REGISTRYINDEX, cur->conn);
	conn = (conn_data *)lua_touserdata (L, -1);
	conn->cur_counter--;
	/* Nullify structure fields. */
	cur->closed = 1;
	ret = SQLCloseCursor(hstmt);
    if (error(ret))
		return fail(L, hSTMT, hstmt);
	ret = SQLFreeHandle(hSTMT, hstmt);
	if (error(ret))
		return fail(L, hSTMT, hstmt);
	luaL_unref (L, LUA_REGISTRYINDEX, cur->conn);
	luaL_unref (L, LUA_REGISTRYINDEX, cur->colinfo);
	cur->conn = LUA_NOREF;
    return pass(L);
}


/*
** Create a table with the names and the types of the fields.
** The names are stored at the position they appear in the result;
** the types are stored in entries named by the corresponding field.
*/
/*
static int sqlCurGetColInfo (lua_State *L) {
	cur_data *cur = (cur_data *) getcursor (L);
	SQLCHAR buffer[256];
	SQLSMALLINT namelen;
	SQLSMALLINT datatype;
	SQLRETURN ret;

    lua_newtable(L);
    for (i = 1; i <= cur->numcols; i++) {
        ret = SQLDescribeCol(cur->hstmt, i, buffer, sizeof(buffer), 
                &namelen, &datatype, NULL, NULL, NULL);
        if (ret == SQL_ERROR) return fail(L, hSTMT, cur->hstmt);
        lua_pushstring (L, buffer);
		lua_pushvalue (L, -1);
		lua_rawseti (L, -3, i);
        lua_pushstring(L, sqltypetolua(datatype));
		lua_rawset (L, -3);
    }
	return 1;
}
*/
static int cur_colinfo (lua_State *L) {
	cur_data *cur = (cur_data *) getcursor (L);
	lua_rawgeti (L, LUA_REGISTRYINDEX, cur->colinfo);
	return 1;
}


/*
** Creates a table with column information.
*/
static void create_colinfo (lua_State *L, const SQLHSTMT hstmt, int numcols) {
	SQLCHAR buffer[256];
	SQLSMALLINT namelen;
	SQLSMALLINT datatype;
	SQLRETURN ret;
	int i;

    lua_newtable(L);
    for (i = 1; i <= numcols; i++) {
        ret = SQLDescribeCol(hstmt, i, buffer, sizeof(buffer), 
                &namelen, &datatype, NULL, NULL, NULL);
        /*if (ret == SQL_ERROR) return fail(L, hSTMT, hstmt);*/
        lua_pushstring (L, buffer);
		lua_pushvalue (L, -1);
		lua_rawseti (L, -3, i);
        lua_pushstring(L, sqltypetolua(datatype));
		lua_rawset (L, -3);
    }
}


/*
** Creates a cursor table and leave it on the top of the stack.
*/
static int create_cursor (lua_State *L, conn_data *conn, 
        const SQLHSTMT hstmt, const SQLSMALLINT numcols) {
    SQLCHAR buffer[256];
    SQLSMALLINT namelen;
    SQLSMALLINT datatype;
    SQLRETURN ret;
    int names, types;
    short i;
    /* allocate cursor userdatum */
    cur_data *cur = (cur_data *) lua_newuserdata(L, sizeof(cur_data));
	luasql_setmeta (L, LUASQL_CURSOR_ODBC);

    /* fill with inherited stuff */
	cur->closed = 0;
    cur->hstmt = hstmt;
    cur->numcols = numcols;
	lua_pushvalue (L, 1);
    cur->conn = luaL_ref (L, LUA_REGISTRYINDEX);

	/* make and store column information table */
	create_colinfo (L, hstmt, numcols);
	cur->colinfo = luaL_ref (L, LUA_REGISTRYINDEX);

    return 1;
}


/*
** Executes a SQL statement.
** Returns
**   cursor object: if there are results or
**   row count: number of rows affected by statement if no results
*/
static int conn_execute (lua_State *L) {
	conn_data *conn = (conn_data *) getconnection (L);
	const char *statement = luaL_checkstring(L, 2);
	SQLHDBC hdbc = conn->hdbc;
	SQLHSTMT hstmt;
	SQLSMALLINT numcols;
	SQLUINTEGER colsize;
	SQLINTEGER strlen;
	SQLUSMALLINT i;
	const char *param;
	size_t len;
	SQLRETURN ret;
	ret = SQLAllocHandle(hSTMT, hdbc, &hstmt);
	if (error(ret))
		return fail(L, hDBC, hdbc);
	ret = SQLPrepare(hstmt, (char *) statement, SQL_NTS);
	if (error(ret))
		return fail(L, hSTMT, hstmt);
	/* execute the statement */
	ret = SQLExecute (hstmt);
	if (error(ret))
		return fail(L, hSTMT, hstmt);
	/* determine the number of results */
	ret = SQLNumResultCols (hstmt, &numcols);
	if (error(ret)) {
		ret = fail(L, hSTMT, hstmt);
		SQLFreeHandle(hSTMT, hstmt);
		return ret;
	}
	if (numcols > 0)
    	/* if there is a results table (e.g., SELECT) */
		return create_cursor (L, conn, hstmt, numcols);
	else {
		/* if action has no results (e.g., UPDATE) */
		SQLINTEGER numrows;
		ret = SQLRowCount(hstmt, &numrows);
		if (error(ret)) {
			ret = fail(L, hSTMT, hstmt);
			SQLFreeHandle(hSTMT, hstmt);
			return ret;
		}
		lua_pushnumber(L, numrows);
		return 1;
	}
}

/*
** Closes a connection.
*/
static int conn_close (lua_State *L) {            
	SQLRETURN ret;
	env_data *env;
    conn_data *conn = (conn_data *) luaL_checkudata (L, 1, LUASQL_CONNECTION_ODBC);
	if (conn->closed)
		return 0;
	if (conn->cur_counter > 0)
		return luaL_error (L, LUASQL_PREFIX"unexpected error (ConnClose)");

	/* Decrement parent's connection counter. */
	lua_rawgeti (L, LUA_REGISTRYINDEX, conn->env);
	env = (env_data *)lua_touserdata (L, -1);
	env->conn_counter--;
	/* Nullify structure fields. */
	conn->closed = 1;
	luaL_unref (L, LUA_REGISTRYINDEX, conn->env);
	conn->env = LUA_NOREF;
	ret = SQLDisconnect(conn->hdbc);
	if (error(ret))
		return fail(L, hDBC, conn->hdbc);
	ret = SQLFreeHandle(hDBC, conn->hdbc);
	if (error(ret))
		return fail(L, hDBC, conn->hdbc);
	conn->hdbc = NULL;
    return pass(L);
}

/*------------------------------------------------------------------*\
* Returns a list with the names of the tables in the data source.
\*------------------------------------------------------------------*/
static int sqlConnTableList(lua_State *L) {
    conn_data *conndata = (conn_data *) lua_touserdata(L, 1);
    SQLHSTMT hstmt;
    int got, index, list;
    SQLUSMALLINT size;
    char *buffer;
    SQLRETURN ret = SQLAllocHandle(hSTMT, conndata->hdbc, &hstmt);
    if (error(ret))
		return fail(L, hDBC, conndata->hdbc);
    ret = SQLTables(hstmt, NULL, 0, NULL, 0, NULL, 0, "TABLE", SQL_NTS );
    if (error(ret))
		return fail(L, hSTMT, hstmt);
    ret = SQLGetInfo(conndata->hdbc, SQL_MAX_TABLE_NAME_LEN, 
            (SQLPOINTER) &size, sizeof(size), NULL);
    if (error(ret))
		return fail(L, hSTMT, hstmt);
    size = size > 0 ? size : 256;
    buffer = (char *) malloc(size);
    if (!buffer) luasql_faildirect(L, LUASQL_PREFIX"allocation error.");
    /* create list */
    lua_newtable(L); list = lua_gettop(L);
    /* store fields */
    index = 1;
    while (1) {    
        /* ask for next table name */
        ret = SQLFetch(hstmt);
        if (ret == SQL_NO_DATA) break;
        if (error(ret)) {
            SQLFreeHandle(hSTMT, hstmt);
            return fail(L, hSTMT, hstmt);
        }
        lua_pushnumber(L, index);
        ret = SQLGetData(hstmt, 3, SQL_C_CHAR, buffer, size, &got);
        lua_pushlstring(L, buffer, got);
        /* save result on table name list */
        lua_settable(L, list);
        index++;
    }
    free(buffer);
    SQLFreeHandle(hSTMT, hstmt);
    /* return the table, it is already on top of stack */
    return 1;
}

/*
** Rolls back a transaction. 
*/
static int conn_commit (lua_State *L) {
	conn_data *conn = (conn_data *) getconnection (L);
	SQLRETURN ret = SQLEndTran(hDBC, conn->hdbc, SQL_COMMIT);
	if (error(ret))
		return fail(L, hSTMT, conn->hdbc);
	else
		return pass(L);
}

/*
** Rollback the current transaction. 
*/
static int conn_rollback (lua_State *L) {
	conn_data *conn = (conn_data *) getconnection (L);
	SQLRETURN ret = SQLEndTran(hDBC, conn->hdbc, SQL_ROLLBACK);
	if (error(ret))
		return fail(L, hSTMT, conn->hdbc);
	else
		return pass(L);
}

/*
** Sets the auto commit mode
*/
static int conn_setautocommit (lua_State *L) {
	conn_data *conn = (conn_data *) lua_touserdata(L, 1);
	SQLRETURN ret;
	if (lua_toboolean (L, 2)) {
		ret = SQLSetConnectAttr(conn->hdbc, SQL_ATTR_AUTOCOMMIT,
			(SQLPOINTER) SQL_AUTOCOMMIT_ON, 0);
	} else {
		ret = SQLSetConnectAttr(conn->hdbc, SQL_ATTR_AUTOCOMMIT,
			(SQLPOINTER) SQL_AUTOCOMMIT_OFF, 0);
	}
	if (error(ret))
		return fail(L, hSTMT, conn->hdbc);
	else
		return pass(L);
}


/*
** Create a new Connection object and push it on top of the stack.
*/
static int create_connection (lua_State *L, env_data *env, SQLHDBC hdbc) {
	conn_data *conn = (conn_data *) lua_newuserdata(L, sizeof(conn_data));
	/* set auto commit mode */
	SQLRETURN ret = SQLSetConnectAttr(hdbc, SQL_ATTR_AUTOCOMMIT, 
		(SQLPOINTER) SQL_AUTOCOMMIT_ON, 0);
	if (error(ret))
		return fail(L, hDBC, hdbc);

	luasql_setmeta (L, LUASQL_CONNECTION_ODBC);

	/* fill in structure */
	conn->closed = 0;
	conn->cur_counter = 0;
	conn->hdbc = hdbc;
	lua_pushvalue (L, 1);
	conn->env = luaL_ref (L, LUA_REGISTRYINDEX);
	return 1;
}


/*
** Creates and returns a connection object
** Lua Input: source [, user [, pass]]
**   source: data source
**   user, pass: data source authentication information
** Lua Returns:
**   connection object if successfull
**   nil and error message otherwise.
*/
static int env_connect (lua_State *L) {
	env_data *env = (env_data *) getenvironment (L);
	const char *sourcename = luaL_checkstring (L, 2);
	const char *username = luaL_optstring (L, 3, NULL);
	const char *password = luaL_optstring (L, 4, NULL);
	SQLHDBC hdbc;
	SQLRETURN ret = SQLSetEnvAttr (env->henv, SQL_ATTR_ODBC_VERSION, 
		(void*)SQL_OV_ODBC3, 0);
	if (error(ret))
		return luasql_faildirect (L, LUASQL_PREFIX"error setting SQL version.");
	/* tries to allocate connection handle */
	ret = SQLAllocHandle (hDBC, env->henv, &hdbc);
	if (error(ret))
		return luasql_faildirect (L, LUASQL_PREFIX"connection allocation error.");
	/* tries to connect handle */
	ret = SQLConnect (hdbc, (char *) sourcename, SQL_NTS, 
		(char *) username, SQL_NTS, (char *) password, SQL_NTS);
	if (error(ret)) {
		ret = fail(L, hDBC, hdbc);
		SQLFreeHandle(hDBC, hdbc);
		return ret;
	}
	/* success, return connection object */
	env->conn_counter++;
	return create_connection (L, env, hdbc);
}

/*
** Closes an environment object
*/
static int env_close (lua_State *L) {
	SQLRETURN ret;
	env_data *env = (env_data *)luaL_checkudata(L, 1, LUASQL_ENVIRONMENT_ODBC);
	if (env->closed)
		return 0;
	if (env->conn_counter > 0)
		luaL_error (L, LUASQL_PREFIX"unexpected error (EnvClose)");

	ret = SQLFreeHandle (hENV, env->henv);
	env->closed = 1;
	if (error(ret)) {
		int ret = fail(L, hENV, env->henv);
		env->henv = NULL;
		return ret;
	}
	return pass(L);
}

/*
** Create metatables for each class of object.
*/
static void createmetatables (lua_State *L) {
	struct luaL_reg environment_methods[] = {
		{"close", env_close},
		{"connect", env_connect},
		{NULL, NULL},
	};
	struct luaL_reg connection_methods[] = {
		{"close", conn_close},
		/*{"TableList", sqlConnTableList},*/
		{"commit", conn_commit},
		{"rollback", conn_rollback},
		{"execute", conn_execute},
		{"setautocommit", conn_setautocommit},
		{NULL, NULL},
	};
	struct luaL_reg cursor_methods[] = {
		{"close", cur_close},
		{"fetch", cur_fetch},
		{"colinfo", cur_colinfo},
		/*{"numrows", sqlCurNumRows},*/
		{NULL, NULL},
	};
	luasql_createmeta (L, LUASQL_ENVIRONMENT_ODBC, environment_methods);
	luasql_createmeta (L, LUASQL_CONNECTION_ODBC, connection_methods);
	luasql_createmeta (L, LUASQL_CURSOR_ODBC, cursor_methods);
}


/*
** Creates an Environment and returns it.
*/
static int create_environment (lua_State *L) {
	SQLRETURN ret;
	env_data *env = (env_data *)lua_newuserdata (L, sizeof (env_data));
	luasql_setmeta (L, LUASQL_ENVIRONMENT_ODBC);
	ret = SQLAllocHandle(hENV, SQL_NULL_HANDLE, &env->henv);
	if (error(ret))
		return luasql_faildirect (L, LUASQL_PREFIX"error creating environment.");

	/* fill in structure */
	env->closed = 0;
	env->conn_counter = 0;
	return 1;
}


/*
** Creates the metatables for the objects and registers the
** driver open method.
*/
LUASQL_API int luasql_libopen_odbc(lua_State *L) {
	lua_getglobal(L, LUASQL_TABLENAME); 
	if (lua_isnil (L, -1)) {
		lua_newtable (L);
		lua_pushvalue (L, -1);
		lua_setglobal (L, LUASQL_TABLENAME);
	}
	lua_pushstring(L, "odbc"); 
	lua_pushcfunction(L, create_environment); 
	lua_settable(L, -3); 

	createmetatables (L);

	return 0;
} 