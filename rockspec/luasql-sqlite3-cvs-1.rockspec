package = "LuaSQL-SQLite3"
version = "cvs-1"
source = {
   url = "cvs://:pserver:anonymous:@cvs.luaforge.net:/cvsroot/luasql",
   cvs_tag = "HEAD",
}
description = {
   summary = "Database connectivity for Lua (SQLite3 driver)",
   detailed = [[
      LuaSQL is a simple interface from Lua to a DBMS. It enables a
      Lua program to connect to databases, execute arbitrary SQL statements
      and retrieve results in a row-by-row cursor fashion.
   ]],
   license = "MIT/X11",
   homepage = "http://www.keplerproject.org/luasql/"
}
dependencies = {
   "lua >= 5.1"
}
external_dependencies = {
   SQLITE = {
      header = "sqlite3.h"
   }
}
build = {
   type = "make",
   variables = {
      T="sqlite3",
      LIB_OPTION = "$(LIBFLAG) -L$(SQLITE_LIBDIR) -lsqlite3",
      CFLAGS = "$(CFLAGS) -I$(LUA_INCDIR) -I$(SQLITE_INCDIR)"
   },
   build_variables = {
      DRIVER_LIBS="",
   },
   install_variables = {
      LUA_LIBDIR = "$(LIBDIR)",
   },
   platforms = {
     "windows" = {
       build_variables = {
         LIB_OPTION = "$(SQLITE_LIB)",
         CFLAGS = "$(CFLAGS) /I$(LUA_INCDIR) /I$(SQLITE_INCDIR)"
       }
     }
   }
}