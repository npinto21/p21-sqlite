#include "modules/native/core_native_packages.h"
#include "module_native_sqlite_hooks.h"

#include <string.h>

static const NativeFuncSpec SQLITE_MODULE_FUNCS[] = {
    { "open", 1, { TYPE_OBJ }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "close", 1, { TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "exec", 3, { TYPE_OBJ, TYPE_STRING, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "exec_ctx", 4, { TYPE_OBJ, TYPE_OBJ, TYPE_STRING, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "query", 3, { TYPE_OBJ, TYPE_STRING, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "query_ctx", 4, { TYPE_OBJ, TYPE_OBJ, TYPE_STRING, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "query_one", 3, { TYPE_OBJ, TYPE_STRING, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "query_one_ctx", 4, { TYPE_OBJ, TYPE_OBJ, TYPE_STRING, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "prepare", 2, { TYPE_OBJ, TYPE_STRING }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "close_prepared", 1, { TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "exec_prepared", 2, { TYPE_OBJ, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "exec_prepared_ctx", 3, { TYPE_OBJ, TYPE_OBJ, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "query_prepared", 2, { TYPE_OBJ, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "query_prepared_ctx", 3, { TYPE_OBJ, TYPE_OBJ, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "query_one_prepared", 2, { TYPE_OBJ, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "query_one_prepared_ctx", 3, { TYPE_OBJ, TYPE_OBJ, TYPE_ARRAY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "begin", 1, { TYPE_OBJ }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "commit", 1, { TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "rollback", 1, { TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "state", 1, { TYPE_OBJ }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "ping", 1, { TYPE_OBJ }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "ping_ctx", 2, { TYPE_OBJ, TYPE_OBJ }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
};

static const NativePackageSpec SQLITE_MODULE_PACKAGE = {
    "sqlite_native",
    SQLITE_MODULE_FUNCS,
    (int)(sizeof(SQLITE_MODULE_FUNCS) / sizeof(SQLITE_MODULE_FUNCS[0]))
};

const NativePackageSpec *p21_module_sqlite_find_package(const char *path) {
    if (strcmp(path, "sqlite_native") == 0) {
        return &SQLITE_MODULE_PACKAGE;
    }
    return NULL;
}

const ModuleNativeRegistryProvider *p21_module_native_registry_provider(void) {
    static const ModuleNativeRegistryProvider provider = {
        "sqlite",
        p21_module_sqlite_find_package
    };
    return &provider;
}
