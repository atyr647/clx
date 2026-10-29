// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  dynamic_vm.cpp · Embedded Lua 5.5 VM       │
// └─────────────────────────────────────────────┘

#include "dynamic_vm.h"
#include "vm_convert.h"
#include "vm_function_object.h"
#include "vm_table_proxy.h"

#include <clx_simd.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
int clx_vm_env_index(lua_State* L);
int clx_vm_env_newindex(lua_State* L);
int clx_vm_env_pairs(lua_State* L);
int clx_vm_env_pairs_iter(lua_State* L);
int clx_vm_env_collectgarbage(lua_State* L);
int clx_vm_env_require(lua_State* L);
}

namespace clx {

void clx_mark_vm_proxies_(LState* clx_L, LState::GCStack& wl);

//------------------ clx_register_vm_proxy - link a VM proxy into clx GC
void clx_register_vm_proxy(LState* clx_L, LHeader* proxy, size_t bytes)
{
    proxy->next = clx_L->allocated_objects;
    clx_L->allocated_objects = proxy;
    clx_L->gc_recent.push(proxy);
    clx_L->object_count++;
    clx_L->allocated_bytes += bytes;

    DynamicVM* vm = DynamicVM::find(clx_L);
    if (vm)
        vm->pace_vm_gc();
}

//------------------ clx_free_vm_proxy_ - free a VM proxy collected by clx GC
static void clx_free_vm_proxy_(LState* clx_L, LHeader* proxy)
{
    if (proxy->type == static_cast<uint8_t>(Table)) {
        clx_L->allocated_bytes -= sizeof(VMTableProxy);
        delete static_cast<VMTableProxy*>(proxy);
    } else {
        clx_L->allocated_bytes -= sizeof(VMFunction);
        delete static_cast<VMFunction*>(proxy);
    }
}

namespace {

    std::unordered_map<LState*, DynamicVM*> g_dynamic_vms_;

}

//------------------ DynamicVM::DynamicVM
DynamicVM::DynamicVM(LState* clx_L)
    : clx_L_(clx_L)
    , L_(nullptr)
    , registry_root_(LUA_NOREF)
    , env_table_ref_(LUA_NOREF)
    , opened_libs_(false)
{
    L_ = luaL_newstate();
    if (!L_)
        throw_runtime_error("failed to create embedded Lua 5.5 vm_state");
    luaL_openlibs(L_);

    lua_gc(L_, LUA_GCRESTART);
    lua_gc(L_, LUA_GCGEN);
    opened_libs_ = true;
    build_parking_lot();
    env_table_ref_ = build_default_env();

    clx_mark_vm_proxies_ptr = clx_mark_vm_proxies_;
    clx_free_vm_proxy_ptr = clx_free_vm_proxy_;
}

//------------------ DynamicVM::~DynamicVM
DynamicVM::~DynamicVM()
{
    if (L_) {

        if (registry_root_ != LUA_NOREF) {
            lua_pushnil(L_);
            lua_rawseti(L_, LUA_REGISTRYINDEX, registry_root_);
        }
        lua_close(L_);
        L_ = nullptr;
    }
}

//------------------ DynamicVM::default_env_registry_ref
int DynamicVM::default_env_registry_ref() const
{
    return env_table_ref_;
}

//------------------ DynamicVM::build_parking_lot
void DynamicVM::build_parking_lot()
{
    lua_newtable(L_);
    registry_root_ = luaL_ref(L_, LUA_REGISTRYINDEX);
}

//------------------ DynamicVM::build_default_env
int DynamicVM::build_default_env()
{
    lua_newtable(L_);
    lua_newtable(L_);

    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_->_G));
    lua_pushinteger(L_, 0);
    lua_pushcclosure(L_, clx_vm_env_index, 3);
    lua_setfield(L_, -2, "__index");

    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_->_G));
    lua_pushinteger(L_, 0);
    lua_pushcclosure(L_, clx_vm_env_newindex, 3);
    lua_setfield(L_, -2, "__newindex");

    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_->_G));
    lua_pushinteger(L_, 0);
    lua_pushcclosure(L_, clx_vm_env_pairs, 3);
    lua_setfield(L_, -2, "__pairs");

    lua_setmetatable(L_, -2);

    // require: clx's own loader serves every module, so the compiled and the interpreted code share one instance of each (clx_vm_env_require)
    lua_pushstring(L_, "require");
    lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
    lua_pushcclosure(L_, clx_vm_env_require, 1);
    lua_rawset(L_, -3);
    lua_pushstring(L_, "package");
    lua_getglobal(L_, "package");
    lua_rawset(L_, -3);

    lua_pushstring(L_, "setmetatable");
    lua_getglobal(L_, "setmetatable");
    lua_rawset(L_, -3);
    lua_pushstring(L_, "getmetatable");
    lua_getglobal(L_, "getmetatable");
    lua_rawset(L_, -3);
    lua_pushstring(L_, "rawset");
    lua_getglobal(L_, "rawset");
    lua_rawset(L_, -3);
    lua_pushstring(L_, "rawget");
    lua_getglobal(L_, "rawget");
    lua_rawset(L_, -3);
    lua_pushstring(L_, "next");
    lua_getglobal(L_, "next");
    lua_rawset(L_, -3);

    {
        static const char* lua_only_globals[] = { "coroutine", "pairs", "ipairs", "pcall", "xpcall", "select",
            "tostring", "type", "error", "assert", "math", "string", "table", "os", "io", nullptr };
        for (const char** gp = lua_only_globals; *gp; ++gp) {
            lua_pushstring(L_, *gp);
            lua_getglobal(L_, *gp);
            lua_rawset(L_, -3);
        }
    }

    {
        lua_pushstring(L_, "collectgarbage");
        lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
        lua_pushcclosure(L_, clx_vm_env_collectgarbage, 1);
        lua_rawset(L_, -3);
    }

    int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    return ref;
}

//------------------ DynamicVM::build_env_table
int DynamicVM::build_env_table(const LValue& preferred)
{
    if (preferred.type == Table && preferred.as_pointer()) {
        lua_newtable(L_);
        LTable* t = static_cast<LTable*>(preferred.as_pointer());
        lua_newtable(L_);

        lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
        lua_pushlightuserdata(L_, static_cast<void*>(t));
        lua_pushinteger(L_, 0);
        lua_pushcclosure(L_, clx_vm_env_index, 3);
        lua_setfield(L_, -2, "__index");

        lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
        lua_pushlightuserdata(L_, static_cast<void*>(t));
        lua_pushinteger(L_, 0);
        lua_pushcclosure(L_, clx_vm_env_newindex, 3);
        lua_setfield(L_, -2, "__newindex");

        lua_pushlightuserdata(L_, static_cast<void*>(clx_L_));
        lua_pushlightuserdata(L_, static_cast<void*>(t));
        lua_pushinteger(L_, 0);
        lua_pushcclosure(L_, clx_vm_env_pairs, 3);
        lua_setfield(L_, -2, "__pairs");
        lua_setmetatable(L_, -2);

        for (size_t i = 0; i < t->array_size; ++i) {
            if (t->array_types[i] != Nil) {
                LValue key(static_cast<int64_t>(i + 1));
                LValue val(t->array[i], t->array_types[i]);
                clx_to_vm_value_(clx_L_, L_, val);
                lua_rawseti(L_, -2, static_cast<int>(i + 1));
            }
        }
        if (t->ext && t->ext->hash_bitmap) {
            size_t bm_words = (t->ext->hash_size + 63) / 64;
            for (size_t word = 0; word < bm_words; ++word) {
                uint64_t bits = t->ext->hash_bitmap[word];
                while (bits) {
                    size_t i = word * 64 + clx_ctzll(bits);
                    if (i >= t->ext->hash_size)
                        break;
                    if (t->ext->entries[i].ktype != Nil) {
                        LValue key(t->ext->entries[i].key, t->ext->entries[i].ktype);
                        LValue val(t->ext->entries[i].val, t->ext->entries[i].vtype);
                        clx_to_vm_value_(clx_L_, L_, key);
                        clx_to_vm_value_(clx_L_, L_, val);
                        lua_rawset(L_, -3);
                    }
                    bits &= bits - 1;
                }
            }
        } else if (t->ext) {
            for (size_t i = 0; i < t->ext->hash_size; ++i) {
                if (t->ext->entries[i].ktype != Nil) {
                    LValue key(t->ext->entries[i].key, t->ext->entries[i].ktype);
                    LValue val(t->ext->entries[i].val, t->ext->entries[i].vtype);
                    clx_to_vm_value_(clx_L_, L_, key);
                    clx_to_vm_value_(clx_L_, L_, val);
                    lua_rawset(L_, -3);
                }
            }
        }
        return luaL_ref(L_, LUA_REGISTRYINDEX);
    }

    lua_rawgeti(L_, LUA_REGISTRYINDEX, env_table_ref_);
    return luaL_ref(L_, LUA_REGISTRYINDEX);
}

//------------------ DynamicVM::load_buffer
bool DynamicVM::load_buffer(const char* src, size_t len, const char* name, const char* mode_str, int env_ref,
    int* out_ref, std::string* error_msg)
{
    int rc = luaL_loadbufferx(L_, src, len, name ? name : "[chunk]", mode_str);
    if (rc != LUA_OK) {
        if (error_msg) {
            const char* msg = lua_tostring(L_, -1);
            *error_msg = msg ? msg : "<unknown>";
        }
        lua_pop(L_, 1);
        return false;
    }
    if (env_ref != LUA_NOREF) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, env_ref);
        //------------------ lua_setupvalue only pops when upvalue #1 exists; a chunk with no
        //------------------ upvalues (e.g. a dumped function body referencing no globals) must not
        //------------------ leave the env table behind for luaL_ref to capture instead of the function
        if (!lua_setupvalue(L_, -2, 1))
            lua_pop(L_, 1);
    }
    *out_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    return true;
}

//------------------ DynamicVM::load_file
bool DynamicVM::load_file(const char* path, const char* mode_str, int env_ref, int* out_ref, std::string* error_msg)
{
    int rc = luaL_loadfilex(L_, path, mode_str);
    if (rc != LUA_OK) {
        if (error_msg) {
            const char* msg = lua_tostring(L_, -1);
            *error_msg = msg ? msg : "<unknown>";
        }
        lua_pop(L_, 1);
        return false;
    }
    if (env_ref != LUA_NOREF) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, env_ref);
        //------------------ same no-upvalue guard as load_buffer: never ref the env table itself
        if (!lua_setupvalue(L_, -2, 1))
            lua_pop(L_, 1);
    }
    *out_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    return true;
}

//------------------ DynamicVM::capture_error
LValue DynamicVM::capture_error()
{
    const char* msg = lua_tostring(L_, -1);
    if (!msg)
        return LValue();
    LValue v = clx_L_->intern_lvalue(msg, std::strlen(msg));
    lua_pop(L_, 1);
    return v;
}

//------------------ DynamicVM::acquire
DynamicVM* DynamicVM::acquire(LState* clx_L)
{
    auto it = g_dynamic_vms_.find(clx_L);
    if (it != g_dynamic_vms_.end())
        return it->second;
    DynamicVM* vm = new DynamicVM(clx_L);
    g_dynamic_vms_[clx_L] = vm;
    return vm;
}

//------------------ DynamicVM::release
void DynamicVM::release(LState* clx_L)
{
    auto it = g_dynamic_vms_.find(clx_L);
    if (it == g_dynamic_vms_.end())
        return;

    ProxyNode* n = it->second->proxy_head_;
    while (n) {
        ProxyNode* nx = n->next;
        n->prev = nullptr;
        n->next = nullptr;
        n->header = nullptr;
        n = nx;
    }
    it->second->proxy_head_ = nullptr;
    delete it->second;
    g_dynamic_vms_.erase(it);
}

//------------------ DynamicVM::find - non-lazy lookup
DynamicVM* DynamicVM::find(LState* clx_L)
{
    auto it = g_dynamic_vms_.find(clx_L);
    return (it != g_dynamic_vms_.end()) ? it->second : nullptr;
}

//------------------ DynamicVM::pace_vm_gc - throttle embedded VM GC
void DynamicVM::pace_vm_gc()
{
    if (L_ && ++proxy_alloc_since_gc_ >= PROXY_GC_PACE_THRESHOLD) {
        proxy_alloc_since_gc_ = 0;
        lua_gc(L_, LUA_GCSTEP, 200);
    }
}

//------------------ DynamicVM::link_proxy - O(1) head insert
void DynamicVM::link_proxy(ProxyNode* node)
{
    if (!node)
        return;
    node->prev = nullptr;
    node->next = proxy_head_;
    if (proxy_head_)
        proxy_head_->prev = node;
    proxy_head_ = node;
    pace_vm_gc();
    if (clx_L_)
        clx_L_->gc_step();
}

//------------------ DynamicVM::unlink_proxy - O(1) pointer surgery
void DynamicVM::unlink_proxy(ProxyNode* node)
{
    if (!node)
        return;
    if (node->prev)
        node->prev->next = node->next;
    else if (proxy_head_ == node)
        proxy_head_ = node->next;
    if (node->next)
        node->next->prev = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
}

//------------------ DynamicVM::mark_proxy_roots - walk list into clx GC worklist
void DynamicVM::mark_proxy_roots(LState::GCStack& wl)
{
    for (ProxyNode* n = proxy_head_; n; n = n->next) {
        if (n->header && n->header->marked == 0) {
            n->header->marked = 1;
            wl.push(n->header);
        }
        if (n->header && n->header->type == static_cast<uint8_t>(Table)) {
            VMTableProxy* tp = static_cast<VMTableProxy*>(n->header);
            if (tp->clx_underlying && tp->clx_underlying->marked == 0) {
                tp->clx_underlying->marked = 1;
                wl.push(reinterpret_cast<LHeader*>(tp->clx_underlying));
            }
        }
    }
}

//------------------ clx_mark_vm_proxies_ - free-function facade
void clx_mark_vm_proxies_(LState* clx_L, LState::GCStack& wl)
{
    DynamicVM* vm = DynamicVM::find(clx_L);
    if (!vm)
        return;
    vm->mark_proxy_roots(wl);
}

//------------------ clx_vm_env_require - require for code the embedded VM runs. A library the VM itself holds (string, table, ...) comes from its package.loaded;
//------------------ every other module goes through clx's require, which prefers the compiled module while its source is unchanged (tiered loading) and otherwise
//------------------ loads the file into this VM, and which caches the result in clx's package.loaded. So an interpreted module that requires a compiled one
//------------------ gets the compiled instance (not a second, interpreted copy with its own state), and two interpreted modules share one instance too.
extern "C" int clx_vm_env_require(lua_State* L)
{
    size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));

    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1))
        return 1;   // the VM's own library: the value is on top of the stack
    lua_settop(L, 1);

    clx::LValue req = clx::get_global(clx_L, "require");
    if (req.type != clx::ValueType::Function)
        return luaL_error(L, "module '%s' not found: clx's require is not available", name);
    clx::LValue arg = clx::LValue(clx_L->intern_string(name, len));
    clx::MultiValue r;
    std::string msg;
    bool failed = false;
    try {
        r = clx::call_function(clx_L, req, &arg, 1, "require", 0);
    } catch (const clx::LRuntimeException& e) {
        failed = true;
        msg = e.what();
    } catch (const std::exception& e) {
        failed = true;
        msg = e.what();
    }
    if (failed) {
        lua_pushlstring(L, msg.data(), msg.size());
        std::string().swap(msg);   // release before lua_error, which does not return through this frame's destructors
        return lua_error(L);
    }
    clx::clx_to_vm_value_(clx_L, L, r.count > 0 ? r[0] : clx::LValue());
    return 1;
}

//------------------ clx_vm_env_collectgarbage - collectgarbage wrapper
extern "C" int clx_vm_env_collectgarbage(lua_State* L)
{
    const char* opt = lua_tostring(L, 1);

    if (opt && strcmp(opt, "count") == 0) {
        int kb = lua_gc(L, LUA_GCCOUNT, 0);
        lua_pushnumber(L, static_cast<lua_Number>(kb));
        return 1;
    }

    clx::LState* clx_L = static_cast<clx::LState*>(lua_touserdata(L, lua_upvalueindex(1)));

    if (opt && strcmp(opt, "stop") == 0) {
        lua_gc(L, LUA_GCSTOP, 0);
    } else if (opt && strcmp(opt, "restart") == 0) {
        lua_gc(L, LUA_GCRESTART, 0);
    } else if (opt && strcmp(opt, "step") == 0) {
        int step = lua_tointeger(L, 2);
        if (step == 0)
            step = 1;
        lua_gc(L, LUA_GCSTEP, step);
    } else {

        lua_gc(L, LUA_GCCOLLECT, 0);
    }

    if (clx_L)
        clx_L->collect_garbage();

    lua_pushnil(L);
    return 1;
}

}
