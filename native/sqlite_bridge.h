#ifndef P21_SQLITE_BRIDGE_H
#define P21_SQLITE_BRIDGE_H

#include "interpreter/interpreter.h"

#include <sqlite3.h>

typedef struct {
    void *handle;
    const char *error;
} p21_sqlite_result;

typedef enum {
    P21_SQLITE_HANDLE_POOL = 1,
    P21_SQLITE_HANDLE_TX = 2,
    P21_SQLITE_HANDLE_STMT = 3
} p21_sqlite_handle_kind;

typedef struct p21_sqlite_pool {
    p21_sqlite_handle_kind kind;
    sqlite3 *db;
    char *path;
    int busy_timeout_ms;
    int max_open;
    int max_idle;
    int open;
} p21_sqlite_pool;

typedef struct p21_sqlite_tx {
    p21_sqlite_handle_kind kind;
    p21_sqlite_pool *pool;
    int active;
} p21_sqlite_tx;

typedef struct p21_sqlite_stmt {
    p21_sqlite_handle_kind kind;
    void *target_handle;
    char *sql;
} p21_sqlite_stmt;

p21_sqlite_result p21_sqlite_open(const char *path, int busy_timeout_ms, int max_open, int max_idle);
p21_sqlite_result p21_sqlite_close(void *pool_handle);
p21_sqlite_result p21_sqlite_exec(void *target_handle, const char *sql, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_exec_ctx(void *ctx_handle, void *target_handle, const char *sql, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_query(void *target_handle, const char *sql, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_query_ctx(void *ctx_handle, void *target_handle, const char *sql, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_query_one(void *target_handle, const char *sql, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_query_one_ctx(void *ctx_handle, void *target_handle, const char *sql, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_prepare(void *target_handle, const char *sql);
p21_sqlite_result p21_sqlite_close_prepared(void *stmt_handle);
p21_sqlite_result p21_sqlite_exec_prepared(void *stmt_handle, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_exec_prepared_ctx(void *ctx_handle, void *stmt_handle, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_query_prepared(void *stmt_handle, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_query_prepared_ctx(void *ctx_handle, void *stmt_handle, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_query_one_prepared(void *stmt_handle, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_query_one_prepared_ctx(void *ctx_handle, void *stmt_handle, Value *params, char **json_out);
p21_sqlite_result p21_sqlite_begin(void *pool_handle);
p21_sqlite_result p21_sqlite_commit(void *tx_handle);
p21_sqlite_result p21_sqlite_rollback(void *tx_handle);
p21_sqlite_result p21_sqlite_state(void *target_handle, char **json_out);
p21_sqlite_result p21_sqlite_ping(void *target_handle, char **json_out);
p21_sqlite_result p21_sqlite_ping_ctx(void *ctx_handle, void *target_handle, char **json_out);

#endif
