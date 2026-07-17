#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lua_State lua_State;
typedef int (*lua_CFunction) (lua_State *L);
typedef double lua_Number;
typedef int64_t lua_Integer;

typedef struct lua_Debug lua_Debug;
struct lua_Debug {
    int event;
    const char *name;
    const char *namewhat;
    const char *what;
    const char *source;
    int currentline;
    int linedefined;
    int lastlinedefined;
    unsigned char nups;
    unsigned char nparams;
    char isvararg;
    char istailcall;
    char short_src[60];
};

typedef void (*lua_Hook) (lua_State *L, lua_Debug *ar);

inline int lua_getstack(lua_State *L, int level, lua_Debug *ar) { return 0; }
inline int lua_getinfo(lua_State *L, const char *what, lua_Debug *ar) { return 0; }

#define LUA_OK 0
#define LUA_YIELD 1
#define LUA_ERRRUN 2
#define LUA_ERRSYNTAX 3
#define LUA_ERRMEM 4
#define LUA_ERRGCMM 5
#define LUA_ERRERR 6

#define LUA_GCSTOP 0
#define LUA_GCRESTART 1
#define LUA_GCCOLLECT 2
#define LUA_GCCOUNT 3
#define LUA_GCCOUNTB 4
#define LUA_GCSTEP 5
#define LUA_GCSETPAUSE 6
#define LUA_GCSETSTEPMUL 7

inline size_t lua_rawlen(lua_State *L, int idx) { return 0; }
inline const char *lua_pushfstring(lua_State *L, const char *fmt, ...) { return ""; }

#define LUA_TNONE (-1)
#define LUA_TNIL 0
#define LUA_TBOOLEAN 1
#define LUA_TLIGHTUSERDATA 2
#define LUA_TNUMBER 3
#define LUA_TSTRING 4
#define LUA_TTABLE 5
#define LUA_TFUNCTION 6
#define LUA_TUSERDATA 7
#define LUA_TTHREAD 8

#define LUA_REGISTRYINDEX (-1000000)
#define LUA_NOREF (-2)
#define LUA_REFNIL (-1)
#define LUA_MULTRET (-1)
#define lua_upvalueindex(i) (LUA_REGISTRYINDEX - (i))

#define LUA_MASKCALL (1 << 0)
#define LUA_MASKRET (1 << 1)
#define LUA_MASKLINE (1 << 2)
#define LUA_MASKCOUNT (1 << 3)

inline lua_State* luaL_newstate(void) { return nullptr; }
inline void lua_close(lua_State* L) {}
inline int lua_gettop(lua_State *L) { return 0; }
inline int lua_absindex(lua_State *L, int idx) { return idx; }
inline void lua_settop(lua_State *L, int idx) {}
inline void lua_pushvalue(lua_State *L, int idx) {}
inline void lua_remove(lua_State *L, int idx) {}
inline void lua_insert(lua_State *L, int idx) {}
inline void lua_replace(lua_State *L, int idx) {}
inline void lua_pop(lua_State *L, int n) {}

inline int lua_type(lua_State *L, int idx) { return LUA_TNIL; }
inline const char* lua_typename(lua_State *L, int tp) { return "nil"; }
inline int lua_isnoneornil(lua_State *L, int idx) { return 1; }
inline int lua_isnil(lua_State *L, int idx) { return lua_type(L, idx) == LUA_TNIL; }
inline int lua_isnumber(lua_State *L, int idx) { return 0; }
inline int lua_isstring(lua_State *L, int idx) { return 0; }
inline int lua_iscfunction(lua_State *L, int idx) { return 0; }
inline int lua_isinteger(lua_State *L, int idx) { return 0; }
inline int lua_isuserdata(lua_State *L, int idx) { return 0; }
inline int lua_isboolean(lua_State *L, int idx) { return 0; }
inline int lua_isfunction(lua_State *L, int idx) { return 0; }
inline int lua_istable(lua_State *L, int idx) { return 0; }
inline int lua_toboolean(lua_State *L, int idx) { return 0; }
inline lua_Number lua_tonumberx(lua_State *L, int idx, int *isnum) { if (isnum) *isnum = 0; return 0; }
inline lua_Number lua_tonumber(lua_State *L, int idx) { return 0; }
inline lua_Integer lua_tointegerx(lua_State *L, int idx, int *isnum) { if (isnum) *isnum = 0; return 0; }
inline lua_Integer lua_tointeger(lua_State *L, int idx) { return 0; }
inline const char* lua_tolstring(lua_State *L, int idx, size_t *len) { if (len) *len = 0; return ""; }
inline const char* lua_tostring(lua_State *L, int idx) { return ""; }
inline void* lua_touserdata(lua_State *L, int idx) { return nullptr; }
inline lua_State* lua_tothread(lua_State *L, int idx) { return nullptr; }
inline const void* lua_topointer(lua_State *L, int idx) { return nullptr; }

inline void lua_pushnil(lua_State *L) {}
inline void lua_pushnumber(lua_State *L, lua_Number n) {}
inline void lua_pushinteger(lua_State *L, lua_Integer n) {}
inline void lua_pushlstring(lua_State *L, const char *s, size_t len) {}
inline const char* lua_pushstring(lua_State *L, const char *s) { return s; }
#define lua_pushliteral(L, s) lua_pushstring(L, s)
inline void lua_pushcclosure(lua_State *L, lua_CFunction fn, int n) {}
inline void lua_pushcfunction(lua_State *L, lua_CFunction f) {}
inline void lua_pushboolean(lua_State *L, int b) {}
inline void lua_pushlightuserdata(lua_State *L, void *p) {}
inline int lua_pushthread(lua_State *L) { return 0; }

inline void lua_getglobal(lua_State *L, const char *name) {}
inline void lua_gettable(lua_State *L, int idx) {}
inline void lua_getfield(lua_State *L, int idx, const char *k) {}
inline void lua_rawget(lua_State *L, int idx) {}
inline void lua_rawgeti(lua_State *L, int idx, lua_Integer n) {}
inline void lua_createtable(lua_State *L, int narr, int nrec) {}
inline void lua_newtable(lua_State *L) {}
inline void* lua_newuserdatauv(lua_State *L, size_t sz, int nuvalue) { return nullptr; }
inline void* lua_newuserdata(lua_State *L, size_t sz) { return nullptr; }
inline int lua_getmetatable(lua_State *L, int objindex) { return 0; }

inline void lua_setglobal(lua_State *L, const char *name) {}
inline void lua_settable(lua_State *L, int idx) {}
inline void lua_setfield(lua_State *L, int idx, const char *k) {}
inline void lua_rawset(lua_State *L, int idx) {}
inline void lua_rawseti(lua_State *L, int idx, lua_Integer n) {}
inline int lua_setmetatable(lua_State *L, int objindex) { return 0; }

inline void lua_sethook(lua_State *L, lua_Hook func, int mask, int count) {}
inline lua_Hook lua_gethook(lua_State *L) { return nullptr; }
inline int lua_gethookmask(lua_State *L) { return 0; }
inline int lua_gethookcount(lua_State *L) { return 0; }

inline int lua_pcallk(lua_State *L, int nargs, int nresults, int errfunc, int64_t ctx, lua_CFunction k) { return 0; }
inline int lua_pcall(lua_State *L, int nargs, int nresults, int errfunc) { return 0; }
inline void lua_call(lua_State *L, int nargs, int nresults) {}
inline int lua_status(lua_State *L) { return 0; }
inline int lua_gc(lua_State *L, int what, ...) { return 0; }
inline int lua_error(lua_State *L) { return 0; }
inline int lua_next(lua_State *L, int idx) { return 0; }

#ifdef __cplusplus
}
#endif
