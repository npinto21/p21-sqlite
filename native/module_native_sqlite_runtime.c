#include "module_native_sqlite_hooks.h"
#include "sqlite_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *copy_text(const char *text) {
    size_t len = strlen(text);
    char *result = malloc(len + 1);
    memcpy(result, text, len + 1);
    return result;
}

static Value make_void_local(void) {
    Value value;
    value.type = VALUE_VOID;
    return value;
}

static Value make_bool_local(int boolean) {
    Value value;
    value.type = VALUE_BOOL;
    value.as.boolean = boolean ? 1 : 0;
    return value;
}

static Value make_int_local(long long integer) {
    Value value;
    value.type = VALUE_INT;
    value.as.integer = integer;
    return value;
}

static Value make_string_copy_local(const char *string) {
    Value value;
    value.type = VALUE_STRING;
    value.as.string = copy_text(string ? string : "");
    return value;
}

static Value make_object_local(char **keys, Value *values, int count) {
    Value value;
    value.type = VALUE_OBJECT;
    value.as.object.keys = keys;
    value.as.object.values = values;
    value.as.object.count = count;
    return value;
}

static Value make_handle_object(void *handle, const char *kind, const char *driver) {
    char **keys = calloc(3, sizeof(char *));
    Value *values = calloc(3, sizeof(Value));
    char buffer[64];

    snprintf(buffer, sizeof(buffer), "%p", handle);
    keys[0] = copy_text("_handle");
    values[0] = make_string_copy_local(buffer);
    keys[1] = copy_text("_kind");
    values[1] = make_string_copy_local(kind);
    keys[2] = copy_text("_driver");
    values[2] = make_string_copy_local(driver);
    return make_object_local(keys, values, 3);
}

static void *parse_handle_from_object(Value object) {
    int i;
    if (object.type != VALUE_OBJECT) {
        return NULL;
    }
    for (i = 0; i < object.as.object.count; i++) {
        if (strcmp(object.as.object.keys[i], "_handle") == 0 &&
            object.as.object.values[i].type == VALUE_STRING) {
            void *handle = NULL;
            if (sscanf(object.as.object.values[i].as.string, "%p", &handle) == 1) {
                return handle;
            }
            return NULL;
        }
    }
    return NULL;
}

static int value_to_int(Value value, int fallback) {
    if (value.type == VALUE_INT) {
        return (int)value.as.integer;
    }
    if (value.type == VALUE_BOOL) {
        return value.as.boolean ? 1 : 0;
    }
    return fallback;
}

static const char *object_string_field(Value object, const char *key) {
    int i;
    if (object.type != VALUE_OBJECT) {
        return NULL;
    }
    for (i = 0; i < object.as.object.count; i++) {
        if (strcmp(object.as.object.keys[i], key) == 0 &&
            object.as.object.values[i].type == VALUE_STRING) {
            return object.as.object.values[i].as.string;
        }
    }
    return NULL;
}

static int object_int_field(Value object, const char *key, int fallback) {
    int i;
    if (object.type != VALUE_OBJECT) {
        return fallback;
    }
    for (i = 0; i < object.as.object.count; i++) {
        if (strcmp(object.as.object.keys[i], key) == 0) {
            return value_to_int(object.as.object.values[i], fallback);
        }
    }
    return fallback;
}

typedef struct {
    const char *text;
    int pos;
} LocalJsonParser;

static void local_json_skip_spaces(LocalJsonParser *parser) {
    while (parser->text[parser->pos] == ' ' ||
           parser->text[parser->pos] == '\n' ||
           parser->text[parser->pos] == '\r' ||
           parser->text[parser->pos] == '\t') {
        parser->pos++;
    }
}

static int local_json_match(LocalJsonParser *parser, const char *token) {
    int len = (int)strlen(token);
    if (strncmp(parser->text + parser->pos, token, (size_t)len) == 0) {
        parser->pos += len;
        return 1;
    }
    return 0;
}

static Value parse_json_value(LocalJsonParser *parser, int *ok);

static char *parse_json_string(LocalJsonParser *parser, int *ok) {
    char *out = NULL;
    size_t size = 0;
    parser->pos++;
    while (parser->text[parser->pos] != '\0' && parser->text[parser->pos] != '"') {
        char ch = parser->text[parser->pos++];
        if (ch == '\\') {
            char escaped = parser->text[parser->pos++];
            switch (escaped) {
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                default: ch = escaped; break;
            }
        }
        out = realloc(out, size + 2);
        out[size++] = ch;
        out[size] = '\0';
    }
    if (parser->text[parser->pos] != '"') {
        free(out);
        *ok = 0;
        return NULL;
    }
    parser->pos++;
    if (!out) {
        out = copy_text("");
    }
    return out;
}

static Value parse_json_object(LocalJsonParser *parser, int *ok) {
    char **keys = NULL;
    Value *values = NULL;
    int count = 0;
    Value result;
    parser->pos++;
    local_json_skip_spaces(parser);
    while (parser->text[parser->pos] != '\0' && parser->text[parser->pos] != '}') {
        char *key;
        Value value;
        if (parser->text[parser->pos] != '"') {
            *ok = 0;
            break;
        }
        key = parse_json_string(parser, ok);
        if (!*ok) {
            break;
        }
        local_json_skip_spaces(parser);
        if (parser->text[parser->pos] != ':') {
            free(key);
            *ok = 0;
            break;
        }
        parser->pos++;
        local_json_skip_spaces(parser);
        value = parse_json_value(parser, ok);
        if (!*ok) {
            free(key);
            break;
        }
        keys = realloc(keys, (size_t)(count + 1) * sizeof(char *));
        values = realloc(values, (size_t)(count + 1) * sizeof(Value));
        keys[count] = key;
        values[count] = value;
        count++;
        local_json_skip_spaces(parser);
        if (parser->text[parser->pos] == ',') {
            parser->pos++;
            local_json_skip_spaces(parser);
        } else {
            break;
        }
    }
    if (parser->text[parser->pos] != '}') {
        *ok = 0;
    } else {
        parser->pos++;
    }
    result = make_object_local(keys, values, count);
    return result;
}

static Value parse_json_array(LocalJsonParser *parser, int *ok) {
    Value *items = NULL;
    int count = 0;
    Value result;
    parser->pos++;
    local_json_skip_spaces(parser);
    while (parser->text[parser->pos] != '\0' && parser->text[parser->pos] != ']') {
        Value value = parse_json_value(parser, ok);
        if (!*ok) {
            break;
        }
        items = realloc(items, (size_t)(count + 1) * sizeof(Value));
        items[count++] = value;
        local_json_skip_spaces(parser);
        if (parser->text[parser->pos] == ',') {
            parser->pos++;
            local_json_skip_spaces(parser);
        } else {
            break;
        }
    }
    if (parser->text[parser->pos] != ']') {
        *ok = 0;
    } else {
        parser->pos++;
    }
    result.type = VALUE_ARRAY;
    result.as.array.items = items;
    result.as.array.count = count;
    return result;
}

static Value parse_json_value(LocalJsonParser *parser, int *ok) {
    local_json_skip_spaces(parser);
    if (parser->text[parser->pos] == '"') {
        char *text = parse_json_string(parser, ok);
        return make_string_copy_local(text ? text : "");
    }
    if (parser->text[parser->pos] == '[') {
        return parse_json_array(parser, ok);
    }
    if (parser->text[parser->pos] == '{') {
        return parse_json_object(parser, ok);
    }
    if (local_json_match(parser, "true")) {
        return make_bool_local(1);
    }
    if (local_json_match(parser, "false")) {
        return make_bool_local(0);
    }
    if (local_json_match(parser, "null")) {
        return make_void_local();
    }
    {
        int start = parser->pos;
        while ((parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9') ||
               parser->text[parser->pos] == '-' || parser->text[parser->pos] == '+') {
            parser->pos++;
        }
        if (parser->pos > start) {
            char buffer[64];
            int len = parser->pos - start;
            if (len >= (int)sizeof(buffer)) {
                len = (int)sizeof(buffer) - 1;
            }
            memcpy(buffer, parser->text + start, (size_t)len);
            buffer[len] = '\0';
            return make_int_local(atoll(buffer));
        }
    }
    *ok = 0;
    return make_void_local();
}

static Value parse_json_object_string_local(const char *json, int *ok) {
    LocalJsonParser parser;
    parser.text = json ? json : "{}";
    parser.pos = 0;
    *ok = 1;
    return parse_json_value(&parser, ok);
}

static void fill_error(ModuleNativeInvokeResult *result, const char *message, const char *source) {
    char buffer[512];
    result->handle = NULL;
    result->kind = NULL;
    snprintf(
        buffer,
        sizeof(buffer),
        "{kind: db_error, message: %s, source: %s, transient: false}",
        message ? message : "sqlite error",
        source ? source : "sqlite"
    );
    result->error = copy_text(buffer);
    result->has_value = 0;
    result->value = make_void_local();
}

int p21_module_sqlite_invoke(
    const char *package_path,
    const char *func_name,
    Value *args,
    int arg_count,
    const ModuleNativeHostApi *host,
    ModuleNativeInvokeResult *result
) {
    p21_sqlite_result bridge_result;
    char *json_text = NULL;
    int json_ok = 0;
    (void)arg_count;
    (void)host;

    memset(result, 0, sizeof(*result));

    if (strcmp(package_path, "sqlite_native") != 0) {
        return 0;
    }

    if (strcmp(func_name, "open") == 0) {
        const char *path = object_string_field(args[0], "path");
        int busy_timeout = object_int_field(args[0], "busy_timeout", 5000);
        int max_open = object_int_field(args[0], "max_open", 1);
        int max_idle = object_int_field(args[0], "max_idle", 1);
        bridge_result = p21_sqlite_open(path, busy_timeout, max_open, max_idle);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.open");
            return 1;
        }
        result->has_value = 1;
        result->value = make_handle_object(bridge_result.handle, "sqlite.pool", "sqlite");
        return 1;
    }

    if (strcmp(func_name, "close") == 0) {
        bridge_result = p21_sqlite_close(parse_handle_from_object(args[0]));
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.close");
            return 1;
        }
        result->has_value = 1;
        result->value = make_void_local();
        return 1;
    }

    if (strcmp(func_name, "exec") == 0) {
        bridge_result = p21_sqlite_exec(parse_handle_from_object(args[0]), args[1].as.string, &args[2], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.exec");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite exec result", "sqlite.exec");
        }
        return 1;
    }

    if (strcmp(func_name, "exec_ctx") == 0) {
        bridge_result = p21_sqlite_exec_ctx(parse_handle_from_object(args[0]), parse_handle_from_object(args[1]), args[2].as.string, &args[3], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.exec_ctx");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite exec result", "sqlite.exec_ctx");
        }
        return 1;
    }

    if (strcmp(func_name, "query") == 0) {
        bridge_result = p21_sqlite_query(parse_handle_from_object(args[0]), args[1].as.string, &args[2], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.query");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{\"count\":0,\"rows\":[]}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite query result", "sqlite.query");
        }
        return 1;
    }

    if (strcmp(func_name, "query_ctx") == 0) {
        bridge_result = p21_sqlite_query_ctx(parse_handle_from_object(args[0]), parse_handle_from_object(args[1]), args[2].as.string, &args[3], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.query_ctx");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{\"count\":0,\"rows\":[]}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite query result", "sqlite.query_ctx");
        }
        return 1;
    }

    if (strcmp(func_name, "query_one") == 0) {
        bridge_result = p21_sqlite_query_one(parse_handle_from_object(args[0]), args[1].as.string, &args[2], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.query_one");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{\"found\":false,\"row\":{}}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite query result", "sqlite.query_one");
        }
        return 1;
    }

    if (strcmp(func_name, "query_one_ctx") == 0) {
        bridge_result = p21_sqlite_query_one_ctx(parse_handle_from_object(args[0]), parse_handle_from_object(args[1]), args[2].as.string, &args[3], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.query_one_ctx");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{\"found\":false,\"row\":{}}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite query result", "sqlite.query_one_ctx");
        }
        return 1;
    }

    if (strcmp(func_name, "prepare") == 0) {
        bridge_result = p21_sqlite_prepare(parse_handle_from_object(args[0]), args[1].as.string);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.prepare");
            return 1;
        }
        result->has_value = 1;
        result->value = make_handle_object(bridge_result.handle, "sqlite.stmt", "sqlite");
        return 1;
    }

    if (strcmp(func_name, "close_prepared") == 0) {
        bridge_result = p21_sqlite_close_prepared(parse_handle_from_object(args[0]));
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.close_prepared");
            return 1;
        }
        result->has_value = 1;
        result->value = make_void_local();
        return 1;
    }

    if (strcmp(func_name, "exec_prepared") == 0) {
        bridge_result = p21_sqlite_exec_prepared(parse_handle_from_object(args[0]), &args[1], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.exec_prepared");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite exec_prepared result", "sqlite.exec_prepared");
        }
        return 1;
    }

    if (strcmp(func_name, "exec_prepared_ctx") == 0) {
        bridge_result = p21_sqlite_exec_prepared_ctx(parse_handle_from_object(args[0]), parse_handle_from_object(args[1]), &args[2], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.exec_prepared_ctx");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite exec_prepared result", "sqlite.exec_prepared_ctx");
        }
        return 1;
    }

    if (strcmp(func_name, "query_prepared") == 0) {
        bridge_result = p21_sqlite_query_prepared(parse_handle_from_object(args[0]), &args[1], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.query_prepared");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{\"count\":0,\"rows\":[]}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite query_prepared result", "sqlite.query_prepared");
        }
        return 1;
    }

    if (strcmp(func_name, "query_prepared_ctx") == 0) {
        bridge_result = p21_sqlite_query_prepared_ctx(parse_handle_from_object(args[0]), parse_handle_from_object(args[1]), &args[2], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.query_prepared_ctx");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{\"count\":0,\"rows\":[]}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite query_prepared result", "sqlite.query_prepared_ctx");
        }
        return 1;
    }

    if (strcmp(func_name, "query_one_prepared") == 0) {
        bridge_result = p21_sqlite_query_one_prepared(parse_handle_from_object(args[0]), &args[1], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.query_one_prepared");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{\"found\":false,\"row\":{}}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite query_one_prepared result", "sqlite.query_one_prepared");
        }
        return 1;
    }

    if (strcmp(func_name, "query_one_prepared_ctx") == 0) {
        bridge_result = p21_sqlite_query_one_prepared_ctx(parse_handle_from_object(args[0]), parse_handle_from_object(args[1]), &args[2], &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.query_one_prepared_ctx");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{\"found\":false,\"row\":{}}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite query_one_prepared result", "sqlite.query_one_prepared_ctx");
        }
        return 1;
    }

    if (strcmp(func_name, "begin") == 0) {
        bridge_result = p21_sqlite_begin(parse_handle_from_object(args[0]));
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.begin");
            return 1;
        }
        result->has_value = 1;
        result->value = make_handle_object(bridge_result.handle, "sqlite.tx", "sqlite");
        return 1;
    }

    if (strcmp(func_name, "commit") == 0) {
        bridge_result = p21_sqlite_commit(parse_handle_from_object(args[0]));
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.commit");
            return 1;
        }
        result->has_value = 1;
        result->value = make_void_local();
        return 1;
    }

    if (strcmp(func_name, "rollback") == 0) {
        bridge_result = p21_sqlite_rollback(parse_handle_from_object(args[0]));
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.rollback");
            return 1;
        }
        result->has_value = 1;
        result->value = make_void_local();
        return 1;
    }

    if (strcmp(func_name, "state") == 0) {
        bridge_result = p21_sqlite_state(parse_handle_from_object(args[0]), &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.state");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite state result", "sqlite.state");
        }
        return 1;
    }

    if (strcmp(func_name, "ping") == 0) {
        bridge_result = p21_sqlite_ping(parse_handle_from_object(args[0]), &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.ping");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{\"ok\":true}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite ping result", "sqlite.ping");
        }
        return 1;
    }

    if (strcmp(func_name, "ping_ctx") == 0) {
        bridge_result = p21_sqlite_ping_ctx(parse_handle_from_object(args[0]), parse_handle_from_object(args[1]), &json_text);
        if (bridge_result.error) {
            fill_error(result, bridge_result.error, "sqlite.ping_ctx");
            return 1;
        }
        result->has_value = 1;
        result->value = parse_json_object_string_local(json_text ? json_text : "{\"ok\":true}", &json_ok);
        free(json_text);
        if (!json_ok) {
            fill_error(result, "invalid sqlite ping result", "sqlite.ping_ctx");
        }
        return 1;
    }

    fill_error(result, "sqlite native function is not implemented", func_name);
    return 1;
}

const ModuleNativeRuntimeProvider *p21_module_native_runtime_provider(void) {
    static const ModuleNativeRuntimeProvider provider = {
        "sqlite",
        p21_module_sqlite_invoke
    };
    return &provider;
}
