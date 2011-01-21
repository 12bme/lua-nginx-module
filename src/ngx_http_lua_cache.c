/* vim:set ft=c ts=4 sw=4 et fdm=marker: */

#define DDEBUG 0
#include "ddebug.h"

#include <ngx_md5.h>
#include "ngx_http_lua_common.h"
#include "ngx_http_lua_cache.h"
#include "ngx_http_lua_clfactory.h"
#include "ngx_http_lua_util.h"


static void ngx_http_lua_clear_package_loaded(lua_State *L);



/**
 * Find code chunk associated with the given key in code cache,
 * and push it to the top of Lua stack if found.
 *
 * Stack layout before call:
 *         |     ...    | <- top
 *
 * Stack layout after call:
 *         | code chunk | <- top
 *         |     ...    |
 *
 * */
static ngx_int_t
ngx_http_lua_cache_load_code(lua_State *L, const char *ck)
{
    /*  get code cache table */
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_CODE_CACHE_KEY);    /*  sp++ */

    dd("Code cache table to load: %p", lua_topointer(L, -1));

    if (! lua_istable(L, -1)) {
        dd("Error: code cache table to load did not exist!!");
        return NGX_ERROR;
    }

    lua_getfield(L, -1, ck);    /*  sp++ */

    if (lua_isfunction(L, -1)) {
        /*  call closure factory to gen new closure */
        int rc = lua_pcall(L, 0, 1, 0);

        if (rc == 0) {
            /*  remove cache table from stack, leave code chunk at top of stack */
            lua_remove(L, -2);                        /*  sp-- */
            return NGX_OK;
        }
    }

    dd("Value associated with given key in code cache table is not code chunk: stack top=%d, top value type=%s\n",
            lua_gettop(L), lua_typename(L, -1));

    /*  remove cache table and value from stack */
    lua_pop(L, 2);                                /*  sp-=2 */

    return NGX_DECLINED;
}


/**
 * Store the closure factory at the top of Lua stack to code cache, and
 * associate it with the given key. Then generate new closure.
 *
 * Stack layout before call:
 *         | code factory | <- top
 *         |     ...      |
 *
 * Stack layout after call:
 *         | code chunk | <- top
 *         |     ...    |
 *
 * */
static ngx_int_t
ngx_http_lua_cache_store_code(lua_State *L, const char *ck)
{
    int rc;

    /*  get code cache table */
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_CODE_CACHE_KEY);    /*  sp++ */

    dd("Code cache table to store: %p", lua_topointer(L, -1));

    if (! lua_istable(L, -1)) {
        dd("Error: code cache table to load did not exist!!");
        return NGX_ERROR;
    }

    lua_pushvalue(L, -2);        /*  sp++ */
    lua_setfield(L, -2, ck);    /*  sp-- */

    /*  remove cache table, leave closure factory at top of stack */
    lua_pop(L, 1);                /*  sp-- */

    /*  call closure factory to generate new closure */
    rc = lua_pcall(L, 0, 1, 0);
    if (rc != 0) {
        dd("Error: failed to call closure factory!!");
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_http_lua_cache_loadbuffer(lua_State *L, const u_char *src, size_t src_len,
        const u_char *cache_key, const char *name, char **err,
        ngx_flag_t enabled)
{
    int          rc;

    dd("XXX cache key: [%s]", cache_key);

    if (! enabled) {
        ngx_http_lua_clear_package_loaded(L);
    }

    if (ngx_http_lua_cache_load_code(L, (char *) cache_key)
            == NGX_OK)
    {
        /*  code chunk loaded from cache, sp++ */
        dd("Code cache hit! cache key='%s', stack top=%d, script='%.*s'", cache_key, lua_gettop(L), src_len, src);
        return NGX_OK;
    }

    dd("Code cache missed! cache key='%s', stack top=%d, script='%.*s'", cache_key, lua_gettop(L), buf_len, buf);

    /*  load closure factory of inline script to the top of lua stack, sp++ */
    rc = ngx_http_lua_clfactory_loadbuffer(L, (char *) src, src_len, name);

    if (rc != 0) {
        /*  Oops! error occured when loading Lua script */
        if (rc == LUA_ERRMEM) {
            *err = "memory allocation error";

        } else {
            if (lua_isstring(L, -1)) {
                *err = (char *) lua_tostring(L, -1);
            } else {
                *err = "syntax error";
            }
        }

        return NGX_ERROR;
    }

    /*  store closure factory and gen new closure at the top of lua stack to code cache */
    rc = ngx_http_lua_cache_store_code(L, (char *) cache_key);

    if (rc != NGX_OK) {
        *err = "fail to genearte new closutre from the closutre factory";
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_http_lua_cache_loadfile(lua_State *L, const char *script, char **err,
        ngx_flag_t enabled)
{
    int              rc;

    u_char           cache_key[NGX_HTTP_LUA_FILE_KEY_LEN + 1];
    u_char          *p;

    /*  calculate digest of script file path */
    dd("code cache enabled: %d", (int) enabled);

    if (enabled) {
        p = ngx_copy(cache_key, NGX_HTTP_LUA_FILE_TAG, NGX_HTTP_LUA_FILE_TAG_LEN);

        /* FIXME, do not use ngx_strlen() because it's slow */
        p = ngx_http_lua_digest_hex(p, (u_char *) script, ngx_strlen(script));

        *p = '\0';

        dd("XXX cache key for file: [%s]", cache_key);

        if (ngx_http_lua_cache_load_code(L, (char *) cache_key) == NGX_OK) {
            /*  code chunk loaded from cache, sp++ */
            dd("Code cache hit! cache key='%s', stack top=%d, file path='%s'", cache_key, lua_gettop(L), script);
            return NGX_OK;
        }

        dd("Code cache missed! cache key='%s', stack top=%d, file path='%s'", cache_key, lua_gettop(L), script);
    }

    /*  load closure factory of script file to the top of lua stack, sp++ */
    rc = ngx_http_lua_clfactory_loadfile(L, script);

    if (rc != 0) {
        /*  Oops! error occured when loading Lua script */
        if (rc == LUA_ERRMEM) {
            *err = "memory allocation error";
        } else {
            if (lua_isstring(L, -1)) {
                *err = (char *) lua_tostring(L, -1);
            } else {
                *err = "syntax error";
            }
        }

        return NGX_ERROR;
    }

    if (enabled) {
        /*  store closure factory and gen new closure at the top of lua stack to code cache */
        rc = ngx_http_lua_cache_store_code(L, (char *) cache_key);

        if (rc != NGX_OK) {
            *err = "fail to genearte new closutre from the closutre factory";
            return NGX_ERROR;
        }

    } else {
        /*  call closure factory to generate new closure */
        rc = lua_pcall(L, 0, 1, 0);
        if (rc != 0) {
            dd("Error: failed to call closure factory!!");
            return NGX_ERROR;
        }

        ngx_http_lua_clear_package_loaded(L);
    }

    return NGX_OK;
}


static void
ngx_http_lua_clear_package_loaded(lua_State *L)
{
    dd("clear out package.loaded.* on the Lua land");
    lua_getglobal(L, "package"); /* package */

    lua_getfield(L, -1, "loaded"); /* package loaded */

    lua_pushnil(L); /* package loaded nil */

    while (lua_next(L, -2)) { /* package loaded key value */
        lua_pop(L, 1);  /* package loaded key */
        lua_pushvalue(L, -1);  /* package loaded key key */
        lua_pushnil(L); /* package loaded key key nil */
        lua_settable(L, -4);  /* package loaded key */
    }

    /* package loaded */
    lua_pop(L, 2);
}

