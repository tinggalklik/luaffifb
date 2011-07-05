#include "ffi.h"
#include <malloc.h>
#include <math.h>
#include <inttypes.h>

#include <windows.h>
#include <psapi.h>

/* returns the value as a ctype, pushes the user value onto the stack */
static void check_ctype(lua_State* L, int idx, ctype_t* ct)
{
    if (lua_isstring(L, idx)) {
        parser_t P;
        P.line = 1;
        P.prev = P.next = lua_tostring(L, idx);
        P.align_mask = DEFAULT_ALIGN_MASK;
        parse_type(L, &P, ct);
        parse_argument(L, &P, -1, ct, NULL);
        lua_remove(L, -2); /* remove the user value from parse_type */
        return;

    } else if (lua_getmetatable(L, idx)) {
        if (!lua_rawequal(L, -1, CTYPE_MT_UPVAL) && !lua_rawequal(L, -1, CDATA_MT_UPVAL)) {
            goto err;
        }

        lua_pop(L, 1); /* pop the metatable */
        *ct = *(ctype_t*) lua_touserdata(L, idx);
        lua_getuservalue(L, idx);
        return;

    } else if (lua_iscfunction(L, idx)) {
        /* cdata functions have a cdata as the first upvalue */
        if (!lua_getupvalue(L, idx, 1) || !lua_getmetatable(L, -1) || !lua_rawequal(L, -1, CDATA_MT_UPVAL)) {
            goto err;
        }

        lua_pop(L, 1); /* pop the metatable */
        *ct = *(ctype_t*) lua_touserdata(L, -1);
        lua_getuservalue(L, -1);
        lua_remove(L, -2); /* pop the ctype */
        return;
    }

err:
    luaL_error(L, "expected cdata, ctype or string for arg #%d", idx);
}

/* if the idx is a cdata returns the cdata_t* and pushes the user value onto
 * the stack, otherwise returns NULL and pushes nothing
 * also dereferences references */
static void* to_cdata(lua_State* L, int idx, ctype_t* ct)
{
    cdata_t* cd;

    if (lua_getmetatable(L, idx)) {
        if (!lua_rawequal(L, -1, CDATA_MT_UPVAL)) {
            lua_pop(L, 1);
            return NULL;
        }

        lua_pop(L, 1);
        cd = (cdata_t*) lua_touserdata(L, idx);
        lua_getuservalue(L, idx);

    } else if (lua_iscfunction(L, idx) && lua_getupvalue(L, idx, 1)) {
        /* cdata functions have the cdata function pointer as the first upvalue */
        if (!lua_getmetatable(L, -1)) {
            lua_pop(L, 1);
            return NULL;
        } else if (!lua_rawequal(L, -1, CDATA_MT_UPVAL)) {
            lua_pop(L, 2);
            return NULL;
        }

        lua_pop(L, 1);
        cd = (cdata_t*) lua_touserdata(L, -1);
        lua_getuservalue(L, -1);
        lua_remove(L, -2); /* remove the cdata user data */

    } else {
        return NULL;
    }

    *ct = cd->type;

    if (ct->is_reference) {
        ct->is_reference = 0;
        return *(void**) (cd+1);

    } else if (ct->pointers && !ct->is_array) {
        return *(void**) (cd+1);

    } else {
        return cd + 1;
    }
}

/* returns the cdata_t* and pushes the user value onto the stack
 * also dereferences references */
static void* check_cdata(lua_State* L, int idx, ctype_t* ct)
{
    void* p = to_cdata(L, idx, ct);
    if (!p) {
        luaL_error(L, "expected cdata for arg #%d", idx);
    }
    return p;
}

#define TO_NUMBER(TYPE, ALLOW_POINTERS)                                     \
    void* p;                                                                \
    ctype_t ct;                                                             \
                                                                            \
    switch (lua_type(L, idx)) {                                             \
    case LUA_TBOOLEAN:                                                      \
        return (TYPE) lua_toboolean(L, idx);                                \
                                                                            \
    case LUA_TNUMBER:                                                       \
        return (TYPE) lua_tonumber(L, idx);                                 \
                                                                            \
    case LUA_TSTRING:                                                       \
        if (ALLOW_POINTERS) {                                               \
            return (TYPE) (intptr_t) lua_tostring(L, idx);                  \
        } else {                                                            \
            goto err;                                                       \
        }                                                                   \
                                                                            \
    case LUA_TLIGHTUSERDATA:                                                \
        if (ALLOW_POINTERS) {                                               \
            return (TYPE) (intptr_t) lua_topointer(L, idx);                 \
        } else {                                                            \
            goto err;                                                       \
        }                                                                   \
                                                                            \
    case LUA_TUSERDATA:                                                     \
        p = to_cdata(L, idx, &ct);                                          \
                                                                            \
        if (!p) {                                                           \
            if (ALLOW_POINTERS) {                                           \
                return (TYPE) (intptr_t) p;                                 \
            } else {                                                        \
                goto err;                                                   \
            }                                                               \
        }                                                                   \
                                                                            \
        lua_pop(L, 1);                                                      \
                                                                            \
        if (ct.pointers || ct.type == STRUCT_TYPE || ct.type == UNION_TYPE) {\
            if (ALLOW_POINTERS) {                                           \
                return (TYPE) (intptr_t) p;                                 \
            } else {                                                        \
                goto err;                                                   \
            }                                                               \
        } else if (ct.type == UINTPTR_TYPE) {                               \
            return (TYPE) *(intptr_t*) p;                                   \
        } else if (ct.type == INT64_TYPE) {                                 \
            return (TYPE) *(int64_t*) p;                                    \
        } else if (ct.type == UINT64_TYPE) {                                \
            return (TYPE) *(uint64_t*) p;                                   \
        } else {                                                            \
            goto err;                                                       \
        }                                                                   \
                                                                            \
    case LUA_TNIL:                                                          \
        return (TYPE) 0;                                                    \
                                                                            \
    default:                                                                \
        goto err;                                                           \
    }                                                                       \
                                                                            \
err:                                                                        \
    luaL_error(L, "unable to convert value to a cdata");                    \
    return 0

int32_t __fastcall to_int32(lua_State* L, int idx)
{ TO_NUMBER(int32_t, 0); }

uint32_t __fastcall to_uint32(lua_State* L, int idx)
{ TO_NUMBER(uint32_t, 0); }

int64_t __fastcall to_int64(lua_State* L, int idx)
{ TO_NUMBER(int64_t, 0); }

uint64_t __fastcall to_uint64(lua_State* L, int idx)
{ TO_NUMBER(uint64_t, 0); }

double __fastcall to_double(lua_State* L, int idx)
{ TO_NUMBER(double, 0); }

uintptr_t __fastcall to_uintptr(lua_State* L, int idx)
{ TO_NUMBER(uintptr_t, 1); }

/* the caller allocates 8 bytes on the stack (as 'to')
 * we return how many bytes are unused */
int __fastcall to_vararg(lua_State* L, int idx, char* to)
{
    void* p;
    ctype_t ct;

    to += 8;

    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
        *((int*) to - 1) = lua_toboolean(L, idx);
        return 4;

    case LUA_TNUMBER:
        *((double*) to - 1) = lua_tonumber(L, idx);
        return 0;

    case LUA_TSTRING:
        *((const char**) to - 1) = lua_tostring(L, idx);
        return 8 - sizeof(const char*);

    case LUA_TLIGHTUSERDATA:
        *((void**) to - 1) = lua_touserdata(L, idx);
        return 8 - sizeof(void*);

    case LUA_TUSERDATA:
        p = to_cdata(L, idx, &ct);

        if (!p) {
            *((void**) to - 1) = p;
            return 8 - sizeof(void*);
        }

        lua_pop(L, 1);

        if (ct.pointers || ct.type == UINTPTR_TYPE) {
            *((void**) to - 1) = p;
            return 8 - sizeof(void*);

        } else if (ct.type == INT32_TYPE || ct.type == UINT32_TYPE) {
            *((int32_t*) to - 1) = *(int32_t*) p;
            return 4;

        } else if (ct.type == INT64_TYPE || ct.type == UINT64_TYPE) {
            *((int64_t*) to - 1) = *(int64_t*) p;
            return 0;

        } else {
            goto err;
        }

    case LUA_TNIL:
        *((void**) to - 1) = NULL;
        return 8 - sizeof(void*);

    default:
        goto err;
    }

err:
    return luaL_error(L, "unable to convert value to a cdata");
}

int32_t __fastcall to_enum(lua_State* L, int idx, int to_usr)
{
    int32_t ret;

    switch (lua_type(L, idx)) {
    case LUA_TSTRING:
        /* lookup string in to_usr to find value */
        to_usr = lua_absindex(L, to_usr);
        lua_pushvalue(L, idx);
        lua_rawget(L, to_usr);

        if (lua_isnil(L, -1)) {
            goto err;
        }

        ret = lua_tointeger(L, -1);
        lua_pop(L, 1);
        return ret;

    case LUA_TUSERDATA:
        return to_int32(L, idx);

    case LUA_TNIL:
        return (int32_t) 0;

    default:
        goto err;
    }

err:
    return luaL_error(L, "unable to convert to enum");
}

static void* to_pointer(lua_State* L, int idx, ctype_t* ct)
{
    void* p;
    memset(ct, 0, sizeof(*ct));
    ct->pointers = 1;

    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        ct->type = VOID_TYPE;
        lua_pushvalue(L, BUILTIN_UPVAL);
        return NULL;

    case LUA_TLIGHTUSERDATA:
        ct->type = VOID_TYPE;
        lua_pushvalue(L, BUILTIN_UPVAL);
        return lua_touserdata(L, idx);

    case LUA_TSTRING:
        ct->type = INT8_TYPE;
        ct->is_array = 1;
        ct->size = 1;
        lua_pushvalue(L, BUILTIN_UPVAL);
        return (void*) lua_tolstring(L, idx, &ct->array_size);

    case LUA_TUSERDATA:
        p = to_cdata(L, idx, ct);

        if (!p) {
            /* some other type of user data */
            ct->type = VOID_TYPE;
            lua_pushvalue(L, BUILTIN_UPVAL);
            return lua_touserdata(L, idx);
        } else if (ct->pointers || ct->type == STRUCT_TYPE || ct->type == UNION_TYPE) {
            return p;
        } else if (ct->type == UINTPTR_TYPE) {
            return *(void**) p;
        }
        break;
    }

    luaL_error(L, "unable to convert value to a cdata");
    return NULL;
}

static function_t to_function(lua_State* L, int idx, ctype_t* ct)
{
    void* p;
    memset(ct, 0, sizeof(*ct));

    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        ct->type = VOID_TYPE;
        lua_pushvalue(L, BUILTIN_UPVAL);
        return NULL;

    case LUA_TLIGHTUSERDATA:
        ct->type = VOID_TYPE;
        lua_pushvalue(L, BUILTIN_UPVAL);
        return lua_touserdata(L, idx);

    case LUA_TUSERDATA:
        p = to_cdata(L, idx, ct);

        if (!p) {
            goto err;
        } else if (ct->pointers) {
            return p;
        } else if (ct->type == UINTPTR_TYPE) {
            return *(void**) p;
        }
        break;
    }

err:
    luaL_error(L, "unable to convert value to a cdata");
    return NULL;
}

static int is_void_ptr(const ctype_t* ct)
{
    return ct->type == VOID_TYPE
        && ct->pointers == 1;
}

static int is_same_type(lua_State* L, int usr1, int usr2, const ctype_t* t1, const ctype_t* t2)
{
    return t1->type == t2->type
        && t1->type < UNION_TYPE || lua_rawequal(L, usr1, usr2);
}

void* __fastcall to_typed_pointer(lua_State* L, int idx, int to_usr, const ctype_t* tt)
{
    ctype_t ft;
    void* p = to_pointer(L, idx, &ft);

    if (is_void_ptr(tt)) {
        /* any pointer can convert to void* */
        goto suc;

    } else if (is_void_ptr(&ft) && p == NULL) {
        /* NULL can convert to any pointer */
        goto suc;
    }
    
    if (!is_same_type(L, to_usr, -1, tt, &ft)) {
        goto err;
    }
    
    /* auto dereference structs */
    if (tt->pointers + ft.pointers == 1 && (ft.type == STRUCT_TYPE || ft.type == UNION_TYPE)) {
        goto suc;
    } else if (tt->pointers != ft.pointers) {
        goto err;
    }

suc:
    lua_pop(L, 1);
    return p;

err:
    luaL_error(L, "invalid conversion");
    return NULL;
}

function_t __fastcall to_typed_function(lua_State* L, int idx, int to_usr, const ctype_t* tt)
{
    ctype_t ft;
    function_t f = to_function(L, idx, &ft);

    if (is_void_ptr(&ft) && f == NULL) {
        /* NULL can convert to any function */
    } else if (!tt->type != ft.type || tt->calling_convention != ft.calling_convention || !lua_rawequal(L, to_usr, -1)) {
        goto err;
    }

    lua_pop(L, 1);
    return f;

err:
    luaL_error(L, "invalid conversion");
    return NULL;
}

static void set_value(lua_State* L, int idx, void* to, int to_usr, const ctype_t* tt, int check_pointers);

static void set_array(lua_State* L, int idx, void* to, int to_usr, const ctype_t* tt, int check_pointers)
{
    size_t i, sz, esz;
    ctype_t et;

    switch (lua_type(L, idx)) {
    case LUA_TSTRING:
        if (tt->pointers == 1 && IS_CHAR(tt->type)) {
            const char* str = lua_tolstring(L, idx, &sz);

            if (sz >= tt->array_size) {
                memcpy(to, str, tt->array_size);
            } else {
                /* include nul terminator */
                memcpy(to, str, sz+1);
            }
        } else {
            goto err;
        }
        break;

    case LUA_TTABLE:
        et = *tt;
        et.pointers--;
        et.is_array = 0;
        et.array_size = 1;
        to_usr = lua_absindex(L, to_usr);
        esz = et.pointers ? sizeof(void*) : et.size;

        lua_rawgeti(L, idx, 2);

        if (lua_isnil(L, -1)) {
            /* there is no second element, so we set the whole array to the
             * first element (or nil - ie 0) if there is no first element) */
            lua_pop(L, 1);
            lua_rawgeti(L, idx, 1);
            if (lua_isnil(L, -1)) {
                memset(to, 0, tt->size * tt->array_size);
            } else {
                for (i = 0; i < tt->array_size; i++) {
                    set_value(L, -1, (char*) to + esz * i, to_usr, &et, check_pointers);
                }
            }
            lua_pop(L, 1);

        } else {
            /* there is a second element, so we set each element using the
             * equiv index in the table initializer */
            lua_pop(L, 1);
            for (i = 0; i < tt->array_size; i++) {
                lua_rawgeti(L, idx, i+1);

                if (lua_isnil(L, -1)) {
                    /* we've hit the end of the values provided in the
                     * initializer, so memset the rest to zero */
                    lua_pop(L, 1);
                    memset((char*) to + esz * i, 0, (tt->array_size - i) * esz);
                    break;

                } else {
                    set_value(L, -1, (char*) to + esz * i, to_usr, &et, check_pointers);
                    lua_pop(L, 1);
                }
            }
        }
        break;

    default:
        goto err;
    }

    return;

err:
    luaL_error(L, "unable to convert to array");
}

static void set_struct(lua_State* L, int idx, void* to, int to_usr, const ctype_t* tt, int check_pointers)
{
    int have_first = 0;
    int have_other = 0;
    const ctype_t* mt;
    void* p;

    switch (lua_type(L, idx)) {
    case LUA_TTABLE:
        /* match up to the members based off the table initializers key - this
         * will match both numbered and named members in the user table
         * we need a special case for when no entries in the initializer -
         * zero initialize the c struct, and only one entry in the initializer
         * - set all members to this value */
        memset(to, 0, tt->size);
        to_usr = lua_absindex(L, to_usr);
        idx = lua_absindex(L, idx);
        lua_pushnil(L);
        while (lua_next(L, idx)) {

            if (!have_first && lua_tonumber(L, -2) == 1 && lua_tonumber(L, -1) != 0) {
                have_first = 1;
            } else if (!have_other && (lua_type(L, -2) != LUA_TNUMBER || lua_tonumber(L, -2) != 1)) {
                have_other = 1;
            }

            /* lookup the key in the usr table to find the members offset and type */
            lua_pushvalue(L, -2);
            lua_rawget(L, to_usr);
            mt = (const ctype_t*) lua_touserdata(L, -1);

            if (!mt) {
                goto err;
            }

            lua_getuservalue(L, -1);
            set_value(L, -3, (char*) to + mt->offset, -1, mt, check_pointers);

            /* initializer value, mt, mt usr */
            lua_pop(L, 3);
        }

        /* if we only had a single non zero value then initialize all members to that value */
        if (!have_other && have_first && tt->type != UNION_TYPE) {
            size_t i, sz;
            lua_rawgeti(L, idx, 1);
            sz = lua_rawlen(L, to_usr);

            for (i = 2; i < sz; i++) {
                lua_rawgeti(L, to_usr, i);
                mt = (const ctype_t*) lua_touserdata(L, -1);
                lua_getuservalue(L, -1);
                set_value(L, -3, (char*) to + mt->offset, -1, mt, check_pointers);
                lua_pop(L, 2);
            }

            lua_pop(L, 1);
        }
        break;

    case LUA_TUSERDATA:
        if (check_pointers) {
            p = to_typed_pointer(L, idx, to_usr, tt);
        } else {
            ctype_t ct;
            p = to_pointer(L, idx, &ct);
            lua_pop(L, 1);
        }
        memcpy(to, p, tt->size);
        break;

    default:
        goto err;
    }

    return;

err:
    luaL_error(L, "unable to convert to struct");
}

static void set_value(lua_State* L, int idx, void* to, int to_usr, const ctype_t* tt, int check_pointers)
{
    if (tt->is_array) {
        set_array(L, idx, to, to_usr, tt, check_pointers);

    } else if (tt->pointers) {

        if (check_pointers) {
            *(void**) to = to_typed_pointer(L, idx, to_usr, tt);
        } else {
            ctype_t ct;
            *(void**) to = to_pointer(L, idx, &ct);
            lua_pop(L, 1);
        }

    } else {

        switch (tt->type) {
        case BOOL_TYPE:
            *(_Bool*) to = (to_int32(L, idx) != 0);
            break;
        case UINT8_TYPE:
            *(uint8_t*) to = (uint8_t) to_uint32(L, idx);
            break;
        case INT8_TYPE:
            *(int8_t*) to = (int8_t) to_int32(L, idx);
            break;
        case UINT16_TYPE:
            *(uint16_t*) to = (uint16_t) to_uint32(L, idx);
            break;
        case INT16_TYPE:
            *(int16_t*) to = (int16_t) to_int32(L, idx);
            break;
        case UINT32_TYPE:
            *(uint32_t*) to = to_uint32(L, idx);
            break;
        case INT32_TYPE:
            *(int32_t*) to = to_int32(L, idx);
            break;
        case UINT64_TYPE:
            *(uint64_t*) to = to_uint64(L, idx);
            break;
        case INT64_TYPE:
            *(int64_t*) to = to_int64(L, idx);
            break;
        case FLOAT_TYPE:
            *(float*) to = (float) to_double(L, idx);
            break;
        case DOUBLE_TYPE:
            *(double*) to = to_double(L, idx);
            break;
        case UINTPTR_TYPE:
            *(uintptr_t*) to = to_uintptr(L, idx);
            break;
        case ENUM_TYPE:
            *(int32_t*) to = to_enum(L, idx, to_usr);
            break;
        case STRUCT_TYPE:
        case UNION_TYPE:
            set_struct(L, idx, to, to_usr, tt, check_pointers);
            break;
        default:
            goto err;
        }
    }

    return;
err:
    luaL_error(L, "can't convert");
}

static int ffi_typeof(lua_State* L)
{
    ctype_t ct;
    check_ctype(L, 1, &ct);
    push_ctype(L, -1, &ct);
    return 1;
}

static int ctype_call(lua_State* L)
{
    const ctype_t* ct = (const ctype_t*) lua_touserdata(L, 1);
    void* p;
    lua_settop(L, 2);
    lua_getuservalue(L, 1);
    p = push_cdata(L, -1, ct);
    if (!lua_isnoneornil(L, 2)) {
        set_value(L, 2, p, -2, ct, 1);        
    }
    return 1;
}

static int ffi_new(lua_State* L)
{
    ctype_t ct;
    void* p;
    lua_settop(L, 2);
    check_ctype(L, 1, &ct);
    p = push_cdata(L, -1, &ct);
    if (!lua_isnoneornil(L, 2)) {
        set_value(L, 2, p, -2, &ct, 1);
    }
    return 1;
}

static int ffi_cast(lua_State* L)
{
    ctype_t ct;
    void* p;
    lua_settop(L, 2);
    check_ctype(L, 1, &ct);
    p = push_cdata(L, -1, &ct);
    set_value(L, 2, p, -2, &ct, 0);
    return 1;
}

static int ffi_sizeof(lua_State* L)
{
    ctype_t ct;
    if (!lua_isnoneornil(L, 2)) {
        return luaL_error(L, "NYI: variable sized arrays");
    }
    check_ctype(L, 1, &ct);

    if (ct.is_array) {
        lua_pushnumber(L, (ct.pointers - 1 > 0 ? sizeof(void*) : ct.size) * ct.array_size);
    } else {
        lua_pushnumber(L, ct.pointers ? sizeof(void*) : ct.size);
    }
    return 1;
}

static int ffi_alignof(lua_State* L)
{
    ctype_t ct;
    check_ctype(L, 1, &ct);
    lua_pushnumber(L, ct.align_mask + 1);
    return 1;
}

static int ffi_offsetof(lua_State* L)
{
    ctype_t ct;
    lua_settop(L, 2);
    check_ctype(L, 1, &ct);

    if (ct.type != STRUCT_TYPE) {
        return luaL_error(L, "type is not a struct");
    }

    /* get the member ctype */
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);

    if (lua_isuserdata(L, -1)) {
        ctype_t* mbr = (ctype_t*) lua_touserdata(L, -1);
        lua_pushnumber(L, mbr->offset);
        return 1;
    } else {
        return luaL_error(L, "member not found in struct");
    }
}

static int ffi_istype(lua_State* L)
{
    ctype_t tt, ft;
    check_ctype(L, 1, &tt);

    if (!to_cdata(L, 2, &ft)) {
        goto fail;
    }
   
    if (!is_same_type(L, 3, 4, &tt, &ft)) {
        goto fail;
    }

    if (tt.pointers + ft.pointers == 1 && (ft.type == UNION_TYPE || tt.type == STRUCT_TYPE)) {
        ft.pointers = tt.pointers;
    } else if (tt.pointers != ft.pointers) {
        goto fail;
    }

    lua_pushboolean(L, 1);
    return 1;

fail:
    lua_pushboolean(L, 0);
    return 0;
}

static int cdata_gc(lua_State* L)
{
    lua_pushvalue(L, 1);
    lua_rawget(L, GC_UPVAL);

    if (!lua_isnil(L, -1)) {
        lua_pushvalue(L, 1);
        lua_call(L, 1, 0);
    }

    return 0;
}

static int ffi_gc(lua_State* L)
{
    ctype_t ct;
    lua_settop(L, 2);
    check_cdata(L, 1, &ct);

    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_rawset(L, GC_UPVAL);

    /* return the cdata back */
    lua_settop(L, 1);
    return 1;
}

/* pushes a variable number of items onto the stack
 * the user value of the returned type is at the top of the stack
 * errors if the member isn't found or when the key type is incorrect
 * for the type of data
 */
static void* find_member(lua_State* L, ctype_t* type)
{
    char* data = (char*) check_cdata(L, 1, type);

    switch (lua_type(L, 2)) {
    case LUA_TNUMBER:
        /* possibilities are array, pointer */

        if (!type->pointers || is_void_ptr(type)) {
            luaL_error(L, "can't deref a non-pointer type");
        }

        type->is_array = 0;
        type->array_size = 1;
        type->pointers--;

        lua_getuservalue(L, 1);

        return data + (type->pointers ? sizeof(void*) : type->size) * (size_t) lua_tonumber(L, 2);

    case LUA_TSTRING:
        /* possibilities are struct/union, pointer to struct/union */

        if ((type->type != STRUCT_TYPE && type->type != UNION_TYPE) || type->is_array || type->pointers > 1) {
            luaL_error(L, "can't deref a non-struct type");
        }

        lua_getuservalue(L, 1);
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);

        if (lua_isnil(L, -1)) {
            luaL_error(L, "invalid member name");
        }

        /* get member type */
        *type = *(const ctype_t*) lua_touserdata(L, -1);
        lua_getuservalue(L, -1);

        data += type->offset;
        type->offset = 0;
        return data;

    default:
        luaL_error(L, "invalid key type");
        return NULL;
    }
}

static int cdata_newindex(lua_State* L)
{
    ctype_t tt;
    void *to = find_member(L, &tt);
    set_value(L, 3, to, -1, &tt, 1);
    return 0;
}

static int cdata_index(lua_State* L)
{
    void* to;
    ctype_t ct;
    jit_t* jit;
    char* data = find_member(L, &ct);

    if (ct.is_array) {
        /* push a reference to the array */
        ct.is_reference = 1;
        to = push_cdata(L, -1, &ct);
        *(void**) to = data;
        return 1;

    } else if (ct.pointers) {
        to = push_cdata(L, -1, &ct);
        *(void**) to = *(void**) data;
        return 1;

    } else {
        switch (ct.type) {
        case BOOL_TYPE:
            lua_pushboolean(L, *(_Bool*) data);
            break;
        case UINT8_TYPE:
            lua_pushnumber(L, *(uint8_t*) data);
            break;
        case INT8_TYPE:
            lua_pushnumber(L, *(int8_t*) data);
            break;
        case UINT16_TYPE:
            lua_pushnumber(L, *(uint16_t*) data);
            break;
        case INT16_TYPE:
            lua_pushnumber(L, *(int16_t*) data);
            break;
        case UINT32_TYPE:
            lua_pushnumber(L, *(uint32_t*) data);
            break;
        case ENUM_TYPE:
        case INT32_TYPE:
            lua_pushnumber(L, *(int32_t*) data);
            break;
        case UINT64_TYPE:
        case INT64_TYPE:
            to = push_cdata(L, -1, &ct);
            *(int64_t*) to = *(int64_t*) data;
            break;
        case UINTPTR_TYPE:
            to = push_cdata(L, -1, &ct);
            *(uintptr_t*) to = *(uintptr_t*) data;
            break;
        case FLOAT_TYPE:
            lua_pushnumber(L, *(float*) data);
            break;
        case DOUBLE_TYPE:
            lua_pushnumber(L, *(double*) data);
            break;
        case STRUCT_TYPE:
        case UNION_TYPE:
            /* push a reference to the member */
            ct.is_reference = 1;
            to = push_cdata(L, -1, &ct);
            *(void**) to = data;
            break;
        case FUNCTION_TYPE:
            jit = (jit_t*) lua_touserdata(L, JIT_UPVAL);
            push_function(jit, L, *(function_t*) data, -1, &ct);
            break;
        default:
            luaL_error(L, "internal error: invalid member type");
        }
    }

    return 1;
}

static int64_t to_intptr(lua_State* L, int idx, ctype_t* ct)
{
    void* p;
    int64_t ret;

    switch (lua_type(L, idx)) {
    case LUA_TNUMBER:
        memset(ct, 0, sizeof(*ct));
        ct->size = 8;
        ct->type = DOUBLE_TYPE;
        ct->pointers = 0;
        ret = lua_tonumber(L, idx);
        lua_pushvalue(L, BUILTIN_UPVAL);
        return ret;

    case LUA_TUSERDATA:
        p = to_cdata(L, idx, ct);

        if (!p) {
            goto err;
        }

        if (ct->pointers) {
            return (intptr_t) p;
        } else if (ct->type == UINTPTR_TYPE) {
            return *(intptr_t*) p;
        } else if (ct->type == INT64_TYPE) {
            return *(int64_t*) p;
        } else if (ct->type == UINT64_TYPE) {
            return *(int64_t*) p;
        } else {
            goto err;
        }

    default:
        goto err;
    }

err:
    luaL_error(L, "unable to convert value to a cdata");
    return 0;
}

static int rank(int type)
{
    switch (type) {
    case UINTPTR_TYPE:
        return sizeof(uintptr_t) >= 8 ? 4 : 1;
    case INT64_TYPE:
        return 2;
    case UINT64_TYPE:
        return 3;
    default:
        return 0;
    }
}

static void push_number(lua_State* L, int usr, const ctype_t* ct, int64_t val)
{
    void* to = push_cdata(L, usr, ct);

    if (ct->type == UINTPTR_TYPE) {
        *(intptr_t*) to = val;
    } else {
        *(int64_t*) to = val;
    }
}

static int cdata_add(lua_State* L)
{
    ctype_t lt, rt;
    int64_t left, right, res;
    void* to;

    lua_settop(L, 2);
    left = to_intptr(L, 1, &lt);
    right = to_intptr(L, 2, &rt);
    assert(lua_gettop(L) == 4);

    /* note due to 2s complement it doesn't matter if we do the addition as int or uint,
     * but the result needs to be uint64_t if either of the sources are */

    if (lt.pointers && rt.pointers) {
        luaL_error(L, "can't add two pointers");

    } else if (lt.pointers) {
        lt.is_array = 0;
        res = left + (lt.pointers > 1 ? sizeof(void*) : lt.size) * right;
        to = push_cdata(L, 3, &lt);
        *(intptr_t*) to = res;

    } else if (rt.pointers) {
        rt.is_array = 0;
        res = right + (rt.pointers > 1 ? sizeof(void*) : rt.size) * left;
        to = push_cdata(L, 4, &rt);
        *(intptr_t*) to = res;

    } else if (rank(lt.type) > rank(rt.type)) {
        res = left + right;
        push_number(L, 3, &lt, res);

    } else {
        res = left + right;
        push_number(L, 4, &rt, res);
    }

    return 1;
}

static int cdata_sub(lua_State* L)
{
    ctype_t lt, rt;
    int64_t left, right, res;
    void* to;

    lua_settop(L, 2);
    left = to_intptr(L, 1, &lt);
    right = to_intptr(L, 2, &rt);

    if (rt.pointers) {
        luaL_error(L, "can't subtract a pointer value");

    } else if (lt.pointers) {
        lt.is_array = 0;
        res = left - (lt.pointers > 1 ? sizeof(void*) : lt.size) * right;
        to = push_cdata(L, 3, &lt);
        *(intptr_t*) to = res;

    } else if (rank(lt.type) > rank(rt.type)) {
        res = left - right;
        push_number(L, 3, &lt, res);

    } else {
        res = left - right;
        push_number(L, 4, &rt, res);
    }

    return 1;
}

/* TODO fix for unsigned */
#define NUMBER_ONLY_BINOP(OP)                                               \
    ctype_t lt, rt;                                                         \
    int64_t left, right, res;                                               \
                                                                            \
    lua_settop(L, 2);                                                       \
    left = to_intptr(L, 1, &lt);                                            \
    right = to_intptr(L, 2, &rt);                                           \
    res = OP(left, right);                                                  \
                                                                            \
    if (lt.pointers || rt.pointers) {                                       \
        luaL_error(L, "can't operate on a pointer value");                  \
    } else if (rank(lt.type) > rank(rt.type)) {                             \
        push_number(L, 3, &lt, res);                                        \
    } else {                                                                \
        push_number(L, 4, &rt, res);                                        \
    }                                                                       \
                                                                            \
    return 1

#define MUL(l,r) l * r
#define DIV(l,r) l / r
#define MOD(l,r) l % r
#define POW(l,r) pow(l, r)

static int cdata_mul(lua_State* L)
{ NUMBER_ONLY_BINOP(MUL); }

static int cdata_div(lua_State* L)
{ NUMBER_ONLY_BINOP(DIV); }

static int cdata_mod(lua_State* L)
{ NUMBER_ONLY_BINOP(MOD); }

static int cdata_pow(lua_State* L)
{ NUMBER_ONLY_BINOP(POW); }

static int cdata_unm(lua_State* L)
{ 
    ctype_t ct;
    void* to;
    int64_t val = to_intptr(L, 1, &ct);

    if (ct.pointers) {
        luaL_error(L, "can't negate a pointer value");
    } else {
        lua_pushvalue(L, BUILTIN_UPVAL);
        ct.type = INT64_TYPE;
        ct.size = 8;
        to = push_cdata(L, -1, &ct);
        *(int64_t*) to = -val;
    }

    return 1;
}

#define COMPARE_BINOP(OP)                                                   \
    ctype_t lt, rt;                                                         \
    int64_t left, right;                                                    \
    int res;                                                                \
                                                                            \
    lua_settop(L, 2);                                                       \
    left = to_intptr(L, 1, &lt);                                            \
    right = to_intptr(L, 2, &rt);                                           \
                                                                            \
    if (lt.pointers && rt.pointers) {                                       \
        if (!is_void_ptr(&lt) && !is_void_ptr(&rt) && !is_same_type(L, 3, 4, &lt, &rt)) { \
            luaL_error(L, "trying to compare incompatible pointers");       \
        }                                                                   \
        res = OP((uint64_t) left, (uint64_t) right);                        \
                                                                            \
    } else if (lt.pointers && rt.type == UINTPTR_TYPE) {                    \
        res = OP((uint64_t) left, (uint64_t) right);                        \
                                                                            \
    } else if (rt.pointers && lt.type == UINTPTR_TYPE) {                    \
        res = OP((uint64_t) left, (uint64_t) right);                        \
                                                                            \
    } else if (rt.pointers || lt.pointers) {                                \
        luaL_error(L, "trying to compare pointer and integer");             \
                                                                            \
    } else if (IS_UNSIGNED(lt.type) && IS_UNSIGNED(rt.type)) {              \
        res = OP((uint64_t) left, (uint64_t) right);                        \
                                                                            \
    } else if (IS_UNSIGNED(lt.type)) {                                      \
        res = OP((int64_t) (uint64_t) left, right);                         \
                                                                            \
    } else if (IS_UNSIGNED(rt.type)) {                                      \
        res = OP(left, (int64_t) (uint64_t) right);                         \
                                                                            \
    } else {                                                                \
        res = OP(left, right);                                              \
    }                                                                       \
                                                                            \
    lua_pushboolean(L, res);                                                \
    return 1

#define EQ(l, r) (l) == (r)
#define LT(l, r) (l) < (r)
#define LE(l, r) (l) <= (r)

static int cdata_eq(lua_State* L)
{ COMPARE_BINOP(EQ); }

static int cdata_lt(lua_State* L)
{ COMPARE_BINOP(LT); }

static int cdata_le(lua_State* L)
{ COMPARE_BINOP(LE); }

static int ctype_tostring(lua_State* L)
{
    luaL_Buffer B;
    const ctype_t* ct = (const ctype_t*) lua_touserdata(L, 1);
    lua_settop(L, 1);
    lua_getuservalue(L, 1);

    luaL_buffinit(L, &B);
    luaL_addstring(&B, "ctype<");
    append_type_string(&B, 2, ct);
    luaL_addchar(&B, '>');
    luaL_pushresult(&B);

    return 1;
}

static int cdata_tostring(lua_State* L)
{
    ctype_t ct;
    void* p;
    char buf[64];
    lua_settop(L, 1);
    p = to_cdata(L, 1, &ct);

    if (ct.pointers == 0 && ct.type == UINT64_TYPE) {
        sprintf(buf, "%"PRIu64, *(uint64_t*) p);
        lua_pushstring(L, buf);

    } else if (ct.pointers == 0 && ct.type == INT64_TYPE) {
        sprintf(buf, "%"PRId64, *(uint64_t*) p);
        lua_pushstring(L, buf);

    } else if (ct.pointers == 0 && ct.type == UINTPTR_TYPE) {
        lua_pushfstring(L, "%p", *(int64_t*) p);

    } else {
        luaL_Buffer B;
        luaL_buffinit(L, &B);
        luaL_addstring(&B, "cdata<");
        append_type_string(&B, 2, &ct);
        lua_pushfstring(L, ">: %p", p);
        luaL_addvalue(&B);
        luaL_pushresult(&B);
    }

    return 1;
}

static int ffi_errno(lua_State* L)
{
    jit_t* jit = (jit_t*) lua_touserdata(L, JIT_UPVAL);

    if (!lua_isnoneornil(L, 1)) {
        lua_pushnumber(L, jit->last_errno);
        jit->last_errno = luaL_checknumber(L, 1);
    } else {
        lua_pushnumber(L, jit->last_errno);
    }

    return 1;
}

static int ffi_number(lua_State* L)
{
    ctype_t ct;
    void* data = to_cdata(L, 1, &ct);

    if (data) {
        if (ct.pointers) {
            return 0;

        } else if (ct.type == UINTPTR_TYPE) {
            lua_pushnumber(L, *(uintptr_t*) data);
            return 1;

        } else if (ct.type == UINT64_TYPE) {
            lua_pushnumber(L, *(uintptr_t*) data);
            return 1;

        } else if (ct.type == INT64_TYPE) {
            lua_pushnumber(L, *(uintptr_t*) data);
            return 1;

        } else {
            return 0;
        }

    } else {
        lua_pushvalue(L, TO_NUMBER_UPVAL);
        lua_insert(L, 1);
        lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
        return lua_gettop(L);
    }
}

static int ffi_string(lua_State* L)
{
    ctype_t ct;
    char* data;
    lua_settop(L, 2);

    data = (char*) check_cdata(L, 1, &ct);

    if (is_void_ptr(&ct)) {
        lua_pushlstring(L, data, (size_t) luaL_checknumber(L, 2));
        return 1;

    } else if (IS_CHAR(ct.type) && ct.pointers == 1) {
        size_t sz;

        if (!lua_isnil(L, 2)) {
            sz = (size_t) luaL_checknumber(L, 2);

        } else if (ct.is_array) {
            sz = ct.array_size;

        } else {
            sz = strlen(data);
        }

        lua_pushlstring(L, data, sz);
        return 1;
    }

    return luaL_error(L, "unable to convert cdata to string");
}

static int ffi_copy(lua_State* L)
{
    ctype_t ft, tt;
    char* to = (char*) to_pointer(L, 1, &tt);
    char* from = (char*) to_pointer(L, 2, &ft);

    if (!lua_isnoneornil(L, 3)) {
        memcpy(to, from, (size_t) luaL_checknumber(L, 3));

    } else if (IS_CHAR(ft.type) && ft.pointers == 1) {
        size_t sz = ft.is_array ? ft.array_size : strlen(from);
        memcpy(to, from, sz);
        to[sz] = '\0';
    }

    return 0;
}

static int ffi_fill(lua_State* L)
{
    ctype_t ct;
    void* to = to_pointer(L, 1, &ct);
    size_t sz = (size_t) luaL_checknumber(L, 2);
    int val = 0;

    if (!lua_isnoneornil(L, 3)) {
        val = luaL_checkinteger(L, 3);
    }

    memset(to, val, sz);
    return 0;
}

static int ffi_abi(lua_State* L)
{
    luaL_checkstring(L, 1);
    lua_settop(L, 1);
    lua_rawget(L, ABI_PARAM_UPVAL);
    lua_pushboolean(L, lua_toboolean(L, -1));
    return 1;
}

static int ffi_load(lua_State* L)
{
    const char* libname = luaL_checkstring(L, 1);
    void** lib = (void**) lua_newuserdata(L, sizeof(void*));

    *lib = LoadLibraryA(libname);

    if (!*lib) {
        libname = lua_pushfstring(L, "%s.dll", libname);
        *lib = LoadLibraryA(libname);
        lua_pop(L, 1);
    }

    if (!*lib) {
        return luaL_error(L, "could not load library %s", lua_tostring(L, 1));
    }

    lua_newtable(L);
    lua_setuservalue(L, -2);

    lua_pushvalue(L, CMODULE_MT_UPVAL);
    lua_setmetatable(L, -2);
    return 1;
}

static int cmodule_index(lua_State* L)
{
    const char* funcname;
    const ctype_t* ct;
    jit_t* jit;
    void** libs;
    size_t i, num;

    lua_getuservalue(L, 1);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);

    /* see if we have already loaded the function */
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 1);

    funcname = luaL_checkstring(L, 2);

    /* find the function type */
    lua_pushvalue(L, 2);
    lua_rawget(L, FUNCTION_UPVAL);
    if (lua_isnil(L, -1)) {
        return 1; /* return nil */
    }
    ct = (const ctype_t*) lua_touserdata(L, -1);
    lua_getuservalue(L, -1);

    /* unpack the user data */
    jit = (jit_t*) lua_touserdata(L, JIT_UPVAL);
    libs = (void**) lua_touserdata(L, 1);
    num = lua_rawlen(L, 1) / sizeof(void*);

    /* iterate over our modules looking for the function */
    for (i = 0; i < num; i++) {
        function_t func = (function_t) GetProcAddress(libs[i], funcname);

        if (func) {
            push_function(jit, L, func, -1, ct);

            /* cache the function in our user value for next time */
            lua_pushvalue(L, 2);
            lua_pushvalue(L, -2);
            lua_rawset(L, 3);
            return 1;
        }
    }

    return luaL_error(L, "failed to find function %s", funcname);
}

static int jit_gc(lua_State* L)
{
    size_t i;
    jit_t* jit = (jit_t*) lua_touserdata(L, 1);
    dasm_free(jit);
    for (i = 0; i < jit->pagenum; i++) {
        VirtualFree(jit->pages[i].data, 0, MEM_RELEASE);
    }
    free(jit->globals);
    return 0;
}

void* reserve_code(jit_t* jit, size_t sz)
{
    page_t* page;
    size_t off = (jit->pagenum > 0) ? jit->pages[jit->pagenum-1].off : 0;
    size_t size = (jit->pagenum > 0) ? jit->pages[jit->pagenum-1].size : 0;

    if (off + sz >= size) {
        /* need to create a new page */
        jit->pages = (page_t*) realloc(jit->pages, (++jit->pagenum) * sizeof(page_t));
        page = &jit->pages[jit->pagenum-1];

        page->size = ALIGN(sz, jit->align_page_size);
        page->off = 0;
        page->data = (char*) VirtualAlloc(NULL, page->size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    } else {
        DWORD oldprotect;
        page = &jit->pages[jit->pagenum-1];
        VirtualProtect(page->data, page->size, PAGE_READWRITE, &oldprotect);
    }

    return page->data + page->off;
}

void commit_code(jit_t* jit, void* p, size_t sz)
{
    DWORD oldprotect;
    page_t* page = &jit->pages[jit->pagenum-1];
    page->off += sz;
    VirtualProtect(page->data, page->size, PAGE_EXECUTE, &oldprotect);
}

static const luaL_Reg cdata_mt[] = {
    {"__gc", &cdata_gc},
    {"__index", &cdata_index},
    {"__newindex", &cdata_newindex},
    {"__add", &cdata_add},
    {"__sub", &cdata_sub},
    {"__mul", &cdata_mul},
    {"__div", &cdata_div},
    {"__mod", &cdata_mod},
    {"__pow", &cdata_pow},
    {"__unm", &cdata_unm},
    {"__eq", &cdata_eq},
    {"__lt", &cdata_lt},
    {"__le", &cdata_le},
    {"__tostring", &cdata_tostring},
    {NULL, NULL}
};

static const luaL_Reg ctype_mt[] = {
    {"__call", &ctype_call},
    {"__tostring", &ctype_tostring},
    {NULL, NULL}
};

static const luaL_Reg cmodule_mt[] = {
    {"__index", &cmodule_index},
    {NULL, NULL}
};

static const luaL_Reg jit_mt[] = {
    {"__gc", &jit_gc},
};

static const luaL_Reg ffi_reg[] = {
    {"cdef", &ffi_cdef},
    {"load", &ffi_load},
    {"new", &ffi_new},
    {"typeof", &ffi_typeof},
    {"cast", &ffi_cast},
    /* {"metatype", &ffi_metatype}, TODO */
    {"gc", &ffi_gc},
    {"sizeof", &ffi_sizeof},
    {"alignof", &ffi_alignof},
    {"offsetof", &ffi_offsetof},
    {"istype", &ffi_istype},
    {"errno", &ffi_errno},
    {"string", &ffi_string},
    {"copy", &ffi_copy},
    {"fill", &ffi_fill},
    {"abi", &ffi_abi},
    {"number", &ffi_number},
    {NULL, NULL}
};

/* leaves the usr table on the stack */
static void push_builtin(lua_State* L, ctype_t* ct, const char* name, int type, int size)
{
    memset(ct, 0, sizeof(*ct));
    ct->type = type;
    ct->size = size;
    ct->align_mask = (size > 0) ? (size - 1) : 0;
    ct->array_size = 1;
    ct->is_defined = 1;

    lua_pushvalue(L, BUILTIN_UPVAL);
    push_ctype(L, -1, ct);
    lua_setfield(L, TYPE_UPVAL, name);
}

static void add_typedef(lua_State* L, const char* from, const char* to)
{
    ctype_t ct;
    parser_t P;
    P.line = 1;
    P.align_mask = DEFAULT_ALIGN_MASK;
    P.next = P.prev = from;
    parse_type(L, &P, &ct);
    parse_argument(L, &P, -1, &ct, NULL);
    push_ctype(L, -1, &ct);
    lua_setfield(L, TYPE_UPVAL, to);
    lua_pop(L, 2);
}

static int setup_upvals(lua_State* L)
{
    /* weak key mt */
    {
        lua_pushliteral(L, "k");
        lua_setfield(L, WEAK_KEY_MT_UPVAL, "__mode");
    }

    /* jit setup */
    {
        SYSTEM_INFO si;
        jit_t* jit = (jit_t*) lua_touserdata(L, JIT_UPVAL);

        jit->last_errno = 0;

        /* setup mt */
        lua_newtable(L);
        luaL_setfuncs(L, jit_mt, 0);
        lua_setmetatable(L, JIT_UPVAL);

        dasm_init(jit, 1024);
        GetSystemInfo(&si);
        jit->align_page_size = si.dwAllocationGranularity - 1;
        jit->pagenum = 0;
        jit->pages = NULL;
        jit->globals = (void**) malloc(1024 * sizeof(void*));
        dasm_setupglobal(jit, jit->globals, 1024);
        compile_globals(jit, L);
    }

    /* setup builtin types */
    {
        ctype_t ct;
        /* add void type and NULL constant */
        push_builtin(L, &ct, "void", VOID_TYPE, 0);

        ct.pointers = 1;
        push_cdata(L, -1, &ct);
        lua_setfield(L, CONSTANTS_UPVAL, "NULL");
        lua_pop(L, 1);

        /* add the rest of the builtin types */
        push_builtin(L, &ct, "bool", BOOL_TYPE, sizeof(_Bool));
        push_builtin(L, &ct, "uint8_t", UINT8_TYPE, 1);
        push_builtin(L, &ct, "int8_t", INT8_TYPE, 1);
        push_builtin(L, &ct, "uint16_t", UINT16_TYPE, 2);
        push_builtin(L, &ct, "int16_t", INT16_TYPE, 2);
        push_builtin(L, &ct, "uint32_t", UINT32_TYPE, 4);
        push_builtin(L, &ct, "int32_t", INT32_TYPE, 4);
        push_builtin(L, &ct, "uint64_t", UINT64_TYPE, 8);
        push_builtin(L, &ct, "int64_t", INT64_TYPE, 8);
        push_builtin(L, &ct, "float", FLOAT_TYPE, 4);
        push_builtin(L, &ct, "double", DOUBLE_TYPE, 8);
        push_builtin(L, &ct, "uintptr_t", UINTPTR_TYPE, sizeof(uintptr_t));
        lua_pop(L, 11);
    }

    /* setup builtin typedefs */
    {
        if (sizeof(uint32_t) == sizeof(size_t)) {
            add_typedef(L, "uint32_t", "size_t");
            add_typedef(L, "int32_t", "ssize_t");
        } else if (sizeof(uint64_t) == sizeof(size_t)) {
            add_typedef(L, "uint64_t", "size_t");
            add_typedef(L, "int64_t", "ssize_t");
        }

        if (sizeof(int32_t) == sizeof(intptr_t)) {
            add_typedef(L, "int32_t", "intptr_t");
            add_typedef(L, "int32_t", "ptrdiff_t");
        } else if (sizeof(int64_t) == sizeof(intptr_t)) {
            add_typedef(L, "int64_t", "intptr_t");
            add_typedef(L, "int64_t", "ptrdiff_t");
        }

        if (sizeof(uint8_t) == sizeof(wchar_t)) {
            add_typedef(L, "uint8_t", "wchar_t");
        } else if (sizeof(uint16_t) == sizeof(wchar_t)) {
            add_typedef(L, "uint16_t", "wchar_t");
        } else if (sizeof(uint32_t) == sizeof(wchar_t)) {
            add_typedef(L, "uint32_t", "wchar_t");
        }

        if (sizeof(va_list) == sizeof(char*)) {
            add_typedef(L, "char*", "va_list");
        }
    }

    /* setup ABI params table */
    {
        lua_pushboolean(L, 1);
#if defined __i386__ || defined _M_IX86
        lua_setfield(L, ABI_PARAM_UPVAL, "32bit");
#elif defined __amd64__ || defined _M_X64
        lua_setfield(L, ABI_PARAM_UPVAL, "64bit");
#else
#error
#endif

        lua_pushboolean(L, 1);
#if defined __i386__ || defined _M_IX86 || defined __amd64__ || defined _M_X64
        lua_setfield(L, ABI_PARAM_UPVAL, "le");
#else
#error
#endif

        lua_pushboolean(L, 1);
#if defined __i386__ || defined _M_IX86 || defined __amd64__ || defined _M_X64
        lua_setfield(L, ABI_PARAM_UPVAL, "fpu");
#else
#error
#endif
    }

    /* gc table */
    {
        lua_pushvalue(L, WEAK_KEY_MT_UPVAL);
        lua_setmetatable(L, GC_UPVAL);
    }

    /* ffi.os */
    {
#if defined _WIN32_WCE
        lua_pushliteral(L, "WindowsCE");
#elif defined _WIN32
        lua_pushliteral(L, "Windows");
#elif defined __APPLE__ && defined __MACH__
        lua_pushliteral(L, "OSX");
#elif defined __linux__
        lua_pushliteral(L, "Linux");
#elif defined __FreeBSD__ || defined __OpenBSD__ || defined __NetBSD__
        lua_pushliteral(L, "BSD");
#elif defined unix || defined __unix__ || defined __unix || defined _POSIX_VERSION || defined _XOPEN_VERSION
        lua_pushliteral(L, "POSIX");
#else
        lua_pushliteral(L, "Other");
#endif
        lua_setfield(L, 1, "os");
    }


    /* ffi.arch */
    {
#if defined __i386__ || defined _M_IX86
        lua_pushliteral(L, "x86");
#elif defined __amd64__ || defined _M_X64
        lua_pushliteral(L, "x64");
#else
#error
#endif
        lua_setfield(L, 1, "arch");
    }


    /* ffi.C */
    {
        DWORD needed = 0;
        void** libs = NULL;
        EnumProcessModules(GetCurrentProcess(), (HMODULE*) libs, needed, &needed);
        libs = (void**) lua_newuserdata(L, sizeof(void*) * needed);
        EnumProcessModules(GetCurrentProcess(), (HMODULE*) libs, needed, &needed);

        lua_pushvalue(L, CONSTANTS_UPVAL);
        lua_setuservalue(L, -2);

        lua_pushvalue(L, CMODULE_MT_UPVAL);
        lua_setmetatable(L, -2);

        lua_setfield(L, 1, "C");
    }


    return 0;
}

static void push_upvals(lua_State* L, int off)
{
    int i;
    for (i = 0; i < UPVAL_NUM; i++) {
        lua_pushvalue(L, -UPVAL_NUM-off);
    }
}

int luaopen_ffi(lua_State* L)
{
    lua_settop(L, 0);
    lua_checkstack(L, UPVAL_NUM * 2 + 10);
    lua_newtable(L); /* ffi table */

    /* register all C functions with the same set of upvalues in the same
     * order, so that the CDATA_MT_UPVAL etc macros can be used from any C code
     *
     * also set the __metatable field on all of the metatables to stop
     * getmetatable(usr) from returning the metatable
     */
    lua_newtable(L); /* ctype_mt */
    lua_newtable(L); /* cdata_mt */
    lua_newtable(L); /* cmodule_mt */
    lua_newtable(L); /* constants */
    lua_newtable(L); /* weak key mt table */
    lua_newtable(L); /* type table */
    lua_newtable(L); /* function table */
    lua_newtable(L); /* ABI parameters table */
    lua_newuserdata(L, sizeof(jit_t)); /* jit state */
    lua_newtable(L); /* gc table */
    lua_newtable(L); /* builtin usr table */
    lua_getglobal(L, "tonumber");

    assert(lua_gettop(L) == UPVAL_NUM + 1);

    /* ctype_mt */
    lua_pushvalue(L, CTYPE_MT_IDX);
    push_upvals(L, 1);
    luaL_setfuncs(L, ctype_mt, UPVAL_NUM);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    assert(lua_gettop(L) == UPVAL_NUM + 1);

    /* cdata_mt */
    lua_pushvalue(L, CDATA_MT_IDX);
    push_upvals(L, 1);
    luaL_setfuncs(L, cdata_mt, UPVAL_NUM);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    assert(lua_gettop(L) == UPVAL_NUM + 1);

    /* cmodule_mt */
    lua_pushvalue(L, CMODULE_MT_IDX);
    push_upvals(L, 1);
    luaL_setfuncs(L, cmodule_mt, UPVAL_NUM);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    assert(lua_gettop(L) == UPVAL_NUM + 1);

    /* setup upvalues */
    push_upvals(L, 0);
    lua_pushcclosure(L, &setup_upvals, UPVAL_NUM);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 0);

    assert(lua_gettop(L) == UPVAL_NUM + 1);

    /* ffi table */
    luaL_setfuncs(L, ffi_reg, UPVAL_NUM);

    assert(lua_gettop(L) == 1);

    lua_getfield(L, -1, "number");
    lua_setglobal(L, "tonumber");

    return 1;
}