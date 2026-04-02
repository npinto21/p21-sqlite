#include "sqlite_bridge.h"

#include <p21/runtime_context.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static p21_sqlite_result ok_handle(void *handle) {
    p21_sqlite_result result;
    result.handle = handle;
    result.error = NULL;
    return result;
}

static char *copy_text_local(const char *text) {
    size_t len = strlen(text ? text : "");
    char *copy = malloc(len + 1);
    memcpy(copy, text ? text : "", len + 1);
    return copy;
}

static p21_sqlite_result ok_void(void) {
    p21_sqlite_result result;
    result.handle = NULL;
    result.error = NULL;
    return result;
}

static p21_sqlite_result make_error(const char *message) {
    p21_sqlite_result result;
    result.handle = NULL;
    result.error = message;
    return result;
}

static long long sqlite_now_ms_local(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
}

static int sqlite_context_poll_cancelled(void *ctx_handle) {
    long long deadline_ms;

    if (!ctx_handle) {
        return 0;
    }

    deadline_ms = p21_runtime_context_deadline_ms(ctx_handle);
    if (!p21_runtime_context_is_cancelled(ctx_handle) &&
        deadline_ms > 0 &&
        sqlite_now_ms_local() > deadline_ms) {
        p21_runtime_context_mark_cancelled(ctx_handle, "request timeout");
    }

    return p21_runtime_context_is_cancelled(ctx_handle);
}

static int sqlite_interrupt_progress(void *userdata) {
    return sqlite_context_poll_cancelled(userdata);
}

static void sqlite_interrupt_begin(sqlite3 *db, void *ctx_handle) {
    if (db && ctx_handle) {
        sqlite3_progress_handler(db, 1000, sqlite_interrupt_progress, ctx_handle);
    }
}

static void sqlite_interrupt_end(sqlite3 *db) {
    if (db) {
        sqlite3_progress_handler(db, 0, NULL, NULL);
    }
}

static p21_sqlite_result sqlite_cancelled_result(void *ctx_handle, const char *fallback) {
    char buffer[256];
    const char *reason = p21_runtime_context_cancel_reason(ctx_handle);

    if (!reason || reason[0] == '\0') {
        reason = fallback;
    }
    snprintf(buffer, sizeof(buffer), "sqlite operation cancelled: %s", reason ? reason : "cancelled");
    return make_error(copy_text_local(buffer));
}

static p21_sqlite_pool *pool_from_handle(void *handle) {
    p21_sqlite_pool *pool = (p21_sqlite_pool *)handle;
    if (!pool || pool->kind != P21_SQLITE_HANDLE_POOL) {
        return NULL;
    }
    return pool;
}

static p21_sqlite_tx *tx_from_handle(void *handle) {
    p21_sqlite_tx *tx = (p21_sqlite_tx *)handle;
    if (!tx || tx->kind != P21_SQLITE_HANDLE_TX) {
        return NULL;
    }
    return tx;
}

static p21_sqlite_stmt *stmt_from_handle(void *handle) {
    p21_sqlite_stmt *stmt = (p21_sqlite_stmt *)handle;
    if (!stmt || stmt->kind != P21_SQLITE_HANDLE_STMT) {
        return NULL;
    }
    return stmt;
}

static sqlite3 *db_from_target(void *target_handle) {
    p21_sqlite_pool *pool = pool_from_handle(target_handle);
    if (pool && pool->db) {
        return pool->db;
    }
    {
        p21_sqlite_tx *tx = tx_from_handle(target_handle);
        if (tx && tx->pool) {
            return tx->pool->db;
        }
    }
    {
        p21_sqlite_stmt *stmt = stmt_from_handle(target_handle);
        if (stmt) {
            return db_from_target(stmt->target_handle);
        }
    }
    return NULL;
}

static int bind_params(sqlite3_stmt *stmt, Value *params, char **error_out) {
    int i;
    if (error_out) {
        *error_out = NULL;
    }
    if (!params || params->type == VALUE_VOID) {
        return 1;
    }
    if (params->type != VALUE_ARRAY) {
        if (error_out) {
            *error_out = copy_text_local("sqlite parameters must be an array");
        }
        return 0;
    }
    for (i = 0; i < params->as.array.count; i++) {
        int rc = SQLITE_ERROR;
        Value value = params->as.array.items[i];
        switch (value.type) {
            case VALUE_VOID:
                rc = sqlite3_bind_null(stmt, i + 1);
                break;
            case VALUE_BOOL:
                rc = sqlite3_bind_int(stmt, i + 1, value.as.boolean ? 1 : 0);
                break;
            case VALUE_INT:
                rc = sqlite3_bind_int64(stmt, i + 1, value.as.integer);
                break;
            case VALUE_STRING:
                rc = sqlite3_bind_text(stmt, i + 1, value.as.string ? value.as.string : "", -1, SQLITE_TRANSIENT);
                break;
            case VALUE_DOUBLE:
                rc = sqlite3_bind_double(stmt, i + 1, value.as.double_value);
                break;
            case VALUE_FLOAT:
                rc = sqlite3_bind_double(stmt, i + 1, (double)value.as.float_value);
                break;
            default:
                if (error_out) {
                    *error_out = copy_text_local("sqlite parameter type is not supported yet");
                }
                return 0;
        }
        if (rc != SQLITE_OK) {
            if (error_out) {
                *error_out = copy_text_local(sqlite3_errmsg(sqlite3_db_handle(stmt)));
            }
            return 0;
        }
    }
    return 1;
}

static char *dup_sqlite_error(sqlite3 *db, const char *fallback) {
    const char *message = db ? sqlite3_errmsg(db) : fallback;
    char *copy = malloc(strlen(message ? message : "sqlite error") + 1);
    strcpy(copy, message ? message : "sqlite error");
    return copy;
}

static char *json_escape(const char *text) {
    size_t len = 0;
    size_t i;
    char *out;
    if (!text) {
        out = malloc(1);
        out[0] = '\0';
        return out;
    }
    for (i = 0; text[i] != '\0'; i++) {
        switch (text[i]) {
            case '\\':
            case '"':
            case '\n':
            case '\r':
            case '\t':
                len += 2;
                break;
            default:
                len += 1;
                break;
        }
    }
    out = malloc(len + 1);
    len = 0;
    for (i = 0; text[i] != '\0'; i++) {
        switch (text[i]) {
            case '\\': out[len++] = '\\'; out[len++] = '\\'; break;
            case '"': out[len++] = '\\'; out[len++] = '"'; break;
            case '\n': out[len++] = '\\'; out[len++] = 'n'; break;
            case '\r': out[len++] = '\\'; out[len++] = 'r'; break;
            case '\t': out[len++] = '\\'; out[len++] = 't'; break;
            default: out[len++] = text[i]; break;
        }
    }
    out[len] = '\0';
    return out;
}

static char *json_string_value(sqlite3_stmt *stmt, int column) {
    const unsigned char *text = sqlite3_column_text(stmt, column);
    char *escaped = json_escape((const char *)text);
    char *out = malloc(strlen(escaped) + 3);
    sprintf(out, "\"%s\"", escaped);
    free(escaped);
    return out;
}

static char *json_value_for_column(sqlite3_stmt *stmt, int column) {
    int type = sqlite3_column_type(stmt, column);
    char buffer[128];
    switch (type) {
        case SQLITE_INTEGER:
            snprintf(buffer, sizeof(buffer), "%lld", sqlite3_column_int64(stmt, column));
            return copy_text_local(buffer);
        case SQLITE_FLOAT:
            snprintf(buffer, sizeof(buffer), "%.15g", sqlite3_column_double(stmt, column));
            return copy_text_local(buffer);
        case SQLITE_NULL:
            return copy_text_local("null");
        case SQLITE_TEXT:
            return json_string_value(stmt, column);
        case SQLITE_BLOB: {
            const unsigned char *blob = sqlite3_column_blob(stmt, column);
            int bytes = sqlite3_column_bytes(stmt, column);
            char *escaped = json_escape(blob ? (const char *)blob : "");
            char *out = malloc(strlen(escaped) + 32 + (size_t)bytes);
            sprintf(out, "\"%s\"", escaped);
            free(escaped);
            return out;
        }
        default:
            return copy_text_local("null");
    }
}

static p21_sqlite_result exec_control_sql(sqlite3 *db, const char *sql, const char *fallback) {
    char *sqlite_error = NULL;
    int rc;
    if (!db || !sql) {
        return make_error("invalid sqlite control statement");
    }
    rc = sqlite3_exec(db, sql, NULL, NULL, &sqlite_error);
    if (rc != SQLITE_OK) {
        char *message = copy_text_local(sqlite_error ? sqlite_error : fallback);
        if (sqlite_error) {
            sqlite3_free(sqlite_error);
        }
        return make_error(message);
    }
    if (sqlite_error) {
        sqlite3_free(sqlite_error);
    }
    return ok_void();
}

static char *build_query_one_json(sqlite3_stmt *stmt, int step_rc) {
    if (step_rc != SQLITE_ROW) {
        return copy_text_local("{\"found\":false,\"row\":{}}");
    }
    {
        int columns = sqlite3_column_count(stmt);
        int i;
        size_t size = strlen("{\"found\":true,\"row\":{}}") + 1;
        char *out;
        for (i = 0; i < columns; i++) {
            const char *name = sqlite3_column_name(stmt, i);
            char *escaped_name = json_escape(name ? name : "");
            char *value = json_value_for_column(stmt, i);
            size += strlen(escaped_name) + strlen(value) + 4;
            if (i + 1 < columns) {
                size += 1;
            }
            free(escaped_name);
            free(value);
        }
        out = malloc(size);
        strcpy(out, "{\"found\":true,\"row\":{");
        for (i = 0; i < columns; i++) {
            const char *name = sqlite3_column_name(stmt, i);
            char *escaped_name = json_escape(name ? name : "");
            char *value = json_value_for_column(stmt, i);
            strcat(out, "\"");
            strcat(out, escaped_name);
            strcat(out, "\":");
            strcat(out, value);
            if (i + 1 < columns) {
                strcat(out, ",");
            }
            free(escaped_name);
            free(value);
        }
        strcat(out, "}}");
        return out;
    }
}

static char *build_query_json(sqlite3_stmt *stmt, int *final_rc) {
    int rc = SQLITE_DONE;
    int first = 1;
    int row_count = 0;
    size_t size = strlen("{\"count\":0,\"rows\":[]}") + 1;
    char *out = malloc(size);
    strcpy(out, "{\"count\":0,\"rows\":[");

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int columns = sqlite3_column_count(stmt);
        int i;
        size_t row_size = 3;
        char *row_text;
        if (!first) {
            size += 1;
            out = realloc(out, size);
            strcat(out, ",");
        }
        for (i = 0; i < columns; i++) {
            const char *name = sqlite3_column_name(stmt, i);
            char *escaped_name = json_escape(name ? name : "");
            char *value = json_value_for_column(stmt, i);
            row_size += strlen(escaped_name) + strlen(value) + 4;
            if (i + 1 < columns) {
                row_size += 1;
            }
            free(escaped_name);
            free(value);
        }
        row_text = malloc(row_size + 1);
        strcpy(row_text, "{");
        for (i = 0; i < columns; i++) {
            const char *name = sqlite3_column_name(stmt, i);
            char *escaped_name = json_escape(name ? name : "");
            char *value = json_value_for_column(stmt, i);
            strcat(row_text, "\"");
            strcat(row_text, escaped_name);
            strcat(row_text, "\":");
            strcat(row_text, value);
            if (i + 1 < columns) {
                strcat(row_text, ",");
            }
            free(escaped_name);
            free(value);
        }
        strcat(row_text, "}");
        size += strlen(row_text);
        out = realloc(out, size);
        strcat(out, row_text);
        free(row_text);
        first = 0;
        row_count++;
    }

    {
        char suffix[96];
        snprintf(
            suffix,
            sizeof(suffix),
            "],\"count\":%d,\"has_rows\":%s}",
            row_count,
            row_count > 0 ? "true" : "false"
        );
        size += strlen(suffix);
        out = realloc(out, size);
        strcat(out, suffix);
    }

    if (final_rc) {
        *final_rc = rc;
    }
    return out;
}

p21_sqlite_result p21_sqlite_open(const char *path, int busy_timeout_ms, int max_open, int max_idle) {
    p21_sqlite_pool *pool;
    sqlite3 *db = NULL;
    if (!path || path[0] == '\0') {
        return make_error("sqlite path is required");
    }
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        char *message = dup_sqlite_error(db, "could not open sqlite database");
        if (db) {
            sqlite3_close(db);
        }
        return make_error(message);
    }
    sqlite3_busy_timeout(db, busy_timeout_ms > 0 ? busy_timeout_ms : 5000);
    pool = calloc(1, sizeof(*pool));
    pool->kind = P21_SQLITE_HANDLE_POOL;
    pool->db = db;
    pool->path = copy_text_local(path);
    pool->busy_timeout_ms = busy_timeout_ms > 0 ? busy_timeout_ms : 5000;
    pool->max_open = max_open > 0 ? max_open : 1;
    pool->max_idle = max_idle > 0 ? max_idle : 1;
    pool->open = 1;
    return ok_handle(pool);
}

p21_sqlite_result p21_sqlite_close(void *pool_handle) {
    p21_sqlite_pool *pool = pool_from_handle(pool_handle);
    if (!pool) {
        return make_error("invalid sqlite pool handle");
    }
    if (pool->db) {
        sqlite3_close(pool->db);
        pool->db = NULL;
    }
    pool->open = 0;
    free(pool->path);
    free(pool);
    return ok_void();
}

p21_sqlite_result p21_sqlite_exec(void *target_handle, const char *sql, Value *params, char **json_out) {
    return p21_sqlite_exec_ctx(NULL, target_handle, sql, params, json_out);
}

p21_sqlite_result p21_sqlite_exec_ctx(void *ctx_handle, void *target_handle, const char *sql, Value *params, char **json_out) {
    sqlite3 *db = db_from_target(target_handle);
    sqlite3_stmt *stmt = NULL;
    char *bind_error = NULL;
    int rc;
    if (json_out) {
        *json_out = NULL;
    }
    if (!db || !sql) {
        return make_error("invalid sqlite target or sql");
    }
    if (sqlite_context_poll_cancelled(ctx_handle)) {
        return sqlite_cancelled_result(ctx_handle, "cancelled");
    }
    sqlite_interrupt_begin(db, ctx_handle);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite_interrupt_end(db);
        return make_error(dup_sqlite_error(db, "could not prepare sqlite statement"));
    }
    if (!bind_params(stmt, params, &bind_error)) {
        sqlite3_finalize(stmt);
        sqlite_interrupt_end(db);
        return make_error(bind_error ? bind_error : "could not bind sqlite parameters");
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        sqlite_interrupt_end(db);
        if (rc == SQLITE_INTERRUPT && sqlite_context_poll_cancelled(ctx_handle)) {
            return sqlite_cancelled_result(ctx_handle, "cancelled");
        }
        return make_error(dup_sqlite_error(db, "could not execute sqlite statement"));
    }
    if (json_out) {
        char buffer[256];
        snprintf(
            buffer,
            sizeof(buffer),
            "{\"rows_affected\":%d,\"last_insert_id\":%lld}",
            sqlite3_changes(db),
            sqlite3_last_insert_rowid(db)
        );
        *json_out = copy_text_local(buffer);
    }
    sqlite3_finalize(stmt);
    sqlite_interrupt_end(db);
    return ok_void();
}

p21_sqlite_result p21_sqlite_query_one(void *target_handle, const char *sql, Value *params, char **json_out) {
    return p21_sqlite_query_one_ctx(NULL, target_handle, sql, params, json_out);
}

p21_sqlite_result p21_sqlite_query_one_ctx(void *ctx_handle, void *target_handle, const char *sql, Value *params, char **json_out) {
    sqlite3 *db = db_from_target(target_handle);
    sqlite3_stmt *stmt = NULL;
    char *bind_error = NULL;
    int rc;
    if (json_out) {
        *json_out = NULL;
    }
    if (!db || !sql) {
        return make_error("invalid sqlite target or sql");
    }
    if (sqlite_context_poll_cancelled(ctx_handle)) {
        return sqlite_cancelled_result(ctx_handle, "cancelled");
    }
    sqlite_interrupt_begin(db, ctx_handle);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite_interrupt_end(db);
        return make_error(dup_sqlite_error(db, "could not prepare sqlite statement"));
    }
    if (!bind_params(stmt, params, &bind_error)) {
        sqlite3_finalize(stmt);
        sqlite_interrupt_end(db);
        return make_error(bind_error ? bind_error : "could not bind sqlite parameters");
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite_interrupt_end(db);
        if (rc == SQLITE_INTERRUPT && sqlite_context_poll_cancelled(ctx_handle)) {
            return sqlite_cancelled_result(ctx_handle, "cancelled");
        }
        return make_error(dup_sqlite_error(db, "could not read sqlite row"));
    }
    if (json_out) {
        *json_out = build_query_one_json(stmt, rc);
    }
    sqlite3_finalize(stmt);
    sqlite_interrupt_end(db);
    return ok_void();
}

p21_sqlite_result p21_sqlite_query(void *target_handle, const char *sql, Value *params, char **json_out) {
    return p21_sqlite_query_ctx(NULL, target_handle, sql, params, json_out);
}

p21_sqlite_result p21_sqlite_query_ctx(void *ctx_handle, void *target_handle, const char *sql, Value *params, char **json_out) {
    sqlite3 *db = db_from_target(target_handle);
    sqlite3_stmt *stmt = NULL;
    char *bind_error = NULL;
    int rc;
    if (json_out) {
        *json_out = NULL;
    }
    if (!db || !sql) {
        return make_error("invalid sqlite target or sql");
    }
    if (sqlite_context_poll_cancelled(ctx_handle)) {
        return sqlite_cancelled_result(ctx_handle, "cancelled");
    }
    sqlite_interrupt_begin(db, ctx_handle);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite_interrupt_end(db);
        return make_error(dup_sqlite_error(db, "could not prepare sqlite statement"));
    }
    if (!bind_params(stmt, params, &bind_error)) {
        sqlite3_finalize(stmt);
        sqlite_interrupt_end(db);
        return make_error(bind_error ? bind_error : "could not bind sqlite parameters");
    }
    if (json_out) {
        *json_out = build_query_json(stmt, &rc);
    } else {
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        }
    }
    if (rc != SQLITE_DONE) {
        if (json_out && *json_out) {
            free(*json_out);
            *json_out = NULL;
        }
        sqlite3_finalize(stmt);
        sqlite_interrupt_end(db);
        if (rc == SQLITE_INTERRUPT && sqlite_context_poll_cancelled(ctx_handle)) {
            return sqlite_cancelled_result(ctx_handle, "cancelled");
        }
        return make_error(dup_sqlite_error(db, "could not read sqlite rows"));
    }
    sqlite3_finalize(stmt);
    sqlite_interrupt_end(db);
    return ok_void();
}

p21_sqlite_result p21_sqlite_prepare(void *target_handle, const char *sql) {
    p21_sqlite_stmt *stmt;
    sqlite3 *db = db_from_target(target_handle);
    if (!db || !sql) {
        return make_error("invalid sqlite target or sql");
    }
    stmt = calloc(1, sizeof(*stmt));
    stmt->kind = P21_SQLITE_HANDLE_STMT;
    stmt->target_handle = target_handle;
    stmt->sql = copy_text_local(sql);
    return ok_handle(stmt);
}

p21_sqlite_result p21_sqlite_close_prepared(void *stmt_handle) {
    p21_sqlite_stmt *stmt = stmt_from_handle(stmt_handle);
    if (!stmt) {
        return make_error("invalid sqlite statement handle");
    }
    free(stmt->sql);
    free(stmt);
    return ok_void();
}

p21_sqlite_result p21_sqlite_exec_prepared(void *stmt_handle, Value *params, char **json_out) {
    return p21_sqlite_exec_prepared_ctx(NULL, stmt_handle, params, json_out);
}

p21_sqlite_result p21_sqlite_exec_prepared_ctx(void *ctx_handle, void *stmt_handle, Value *params, char **json_out) {
    p21_sqlite_stmt *stmt = stmt_from_handle(stmt_handle);
    if (!stmt) {
        return make_error("invalid sqlite statement handle");
    }
    return p21_sqlite_exec_ctx(ctx_handle, stmt->target_handle, stmt->sql, params, json_out);
}

p21_sqlite_result p21_sqlite_query_prepared(void *stmt_handle, Value *params, char **json_out) {
    return p21_sqlite_query_prepared_ctx(NULL, stmt_handle, params, json_out);
}

p21_sqlite_result p21_sqlite_query_prepared_ctx(void *ctx_handle, void *stmt_handle, Value *params, char **json_out) {
    p21_sqlite_stmt *stmt = stmt_from_handle(stmt_handle);
    if (!stmt) {
        return make_error("invalid sqlite statement handle");
    }
    return p21_sqlite_query_ctx(ctx_handle, stmt->target_handle, stmt->sql, params, json_out);
}

p21_sqlite_result p21_sqlite_query_one_prepared(void *stmt_handle, Value *params, char **json_out) {
    return p21_sqlite_query_one_prepared_ctx(NULL, stmt_handle, params, json_out);
}

p21_sqlite_result p21_sqlite_query_one_prepared_ctx(void *ctx_handle, void *stmt_handle, Value *params, char **json_out) {
    p21_sqlite_stmt *stmt = stmt_from_handle(stmt_handle);
    if (!stmt) {
        return make_error("invalid sqlite statement handle");
    }
    return p21_sqlite_query_one_ctx(ctx_handle, stmt->target_handle, stmt->sql, params, json_out);
}

p21_sqlite_result p21_sqlite_begin(void *pool_handle) {
    p21_sqlite_pool *pool = pool_from_handle(pool_handle);
    p21_sqlite_tx *tx;
    p21_sqlite_result control_result;
    if (!pool || !pool->db) {
        return make_error("invalid sqlite pool handle");
    }
    control_result = exec_control_sql(pool->db, "BEGIN IMMEDIATE", "could not begin sqlite transaction");
    if (control_result.error != NULL) {
        return control_result;
    }
    tx = calloc(1, sizeof(*tx));
    tx->kind = P21_SQLITE_HANDLE_TX;
    tx->pool = pool;
    tx->active = 1;
    return ok_handle(tx);
}

p21_sqlite_result p21_sqlite_commit(void *tx_handle) {
    p21_sqlite_tx *tx = tx_from_handle(tx_handle);
    p21_sqlite_result control_result;
    if (!tx || !tx->pool || !tx->active) {
        return make_error("invalid sqlite transaction handle");
    }
    control_result = exec_control_sql(tx->pool->db, "COMMIT", "could not commit sqlite transaction");
    if (control_result.error != NULL) {
        return control_result;
    }
    tx->active = 0;
    free(tx);
    return ok_void();
}

p21_sqlite_result p21_sqlite_rollback(void *tx_handle) {
    p21_sqlite_tx *tx = tx_from_handle(tx_handle);
    p21_sqlite_result control_result;
    if (!tx || !tx->pool || !tx->active) {
        return make_error("invalid sqlite transaction handle");
    }
    control_result = exec_control_sql(tx->pool->db, "ROLLBACK", "could not rollback sqlite transaction");
    if (control_result.error != NULL) {
        return control_result;
    }
    tx->active = 0;
    free(tx);
    return ok_void();
}

p21_sqlite_result p21_sqlite_state(void *target_handle, char **json_out) {
    p21_sqlite_pool *pool = pool_from_handle(target_handle);
    if (json_out) {
        *json_out = NULL;
    }
    if (!pool) {
        p21_sqlite_tx *tx = tx_from_handle(target_handle);
        if (!tx || !tx->pool) {
            return make_error("invalid sqlite handle");
        }
        pool = tx->pool;
    }
    if (json_out) {
        char *escaped = json_escape(pool->path ? pool->path : "");
        char buffer[512];
        snprintf(
            buffer,
            sizeof(buffer),
            "{\"driver\":\"sqlite\",\"path\":\"%s\",\"busy_timeout\":%d,\"max_open\":%d,\"max_idle\":%d,\"open\":%s}",
            escaped,
            pool->busy_timeout_ms,
            pool->max_open,
            pool->max_idle,
            pool->open ? "true" : "false"
        );
        *json_out = copy_text_local(buffer);
        free(escaped);
    }
    return ok_void();
}

p21_sqlite_result p21_sqlite_ping(void *target_handle, char **json_out) {
    return p21_sqlite_ping_ctx(NULL, target_handle, json_out);
}

p21_sqlite_result p21_sqlite_ping_ctx(void *ctx_handle, void *target_handle, char **json_out) {
    sqlite3 *db = db_from_target(target_handle);
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (json_out) {
        *json_out = NULL;
    }
    if (!db) {
        return make_error("invalid sqlite handle");
    }
    if (sqlite_context_poll_cancelled(ctx_handle)) {
        return sqlite_cancelled_result(ctx_handle, "cancelled");
    }
    sqlite_interrupt_begin(db, ctx_handle);
    rc = sqlite3_prepare_v2(db, "select 1", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite_interrupt_end(db);
        return make_error(dup_sqlite_error(db, "could not ping sqlite database"));
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite_interrupt_end(db);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        if (rc == SQLITE_INTERRUPT && sqlite_context_poll_cancelled(ctx_handle)) {
            return sqlite_cancelled_result(ctx_handle, "cancelled");
        }
        return make_error(dup_sqlite_error(db, "could not ping sqlite database"));
    }
    if (json_out) {
        *json_out = copy_text_local("{\"ok\":true}");
    }
    return ok_void();
}
