#pragma once
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct luaL_Reg {
    const char *name;
    lua_CFunction func;
} luaL_Reg;

inline int luaL_loadfilex(lua_State *L, const char *filename, const char *mode) { return 0; }
inline int luaL_loadfile(lua_State *L, const char *filename) { return 0; }
inline int luaL_loadstring(lua_State *L, const char *s) { return 0; }
inline void luaL_checktype(lua_State *L, int arg, int t) {}
inline void luaL_checkany(lua_State *L, int arg) {}
inline const char* luaL_checklstring(lua_State *L, int arg, size_t *l) { if (l) *l = 0; return ""; }
inline const char* luaL_checkstring(lua_State *L, int arg) { return ""; }
inline lua_Number luaL_checknumber(lua_State *L, int arg) { return 0; }
inline lua_Integer luaL_checkinteger(lua_State *L, int arg) { return 0; }
inline int luaL_error(lua_State *L, const char *fmt, ...) { return 0; }
inline int luaL_newmetatable(lua_State *L, const char *tname) { return 1; }
inline void luaL_setfuncs(lua_State *L, const luaL_Reg *l, int nup) {}
inline int luaL_ref(lua_State *L, int t) { return 0; }
inline void luaL_unref(lua_State *L, int t, int ref) {}
inline void* luaL_checkudata(lua_State *L, int ud, const char *tname) { return nullptr; }
inline int luaL_getmetatable(lua_State *L, const char *tname) { return 0; }
inline void* luaL_testudata(lua_State *L, int ud, const char *tname) { return nullptr; }
inline const char* luaL_tolstring(lua_State *L, int idx, size_t *len) { if (len) *len = 0; return ""; }
inline int luaL_len(lua_State *L, int idx) { return 0; }
inline void luaL_traceback(lua_State *L, lua_State *L1, const char *msg, int level) {}
inline int luaL_dostring(lua_State *L, const char *str) { return 0; }

#ifdef __cplusplus
}
#endif
