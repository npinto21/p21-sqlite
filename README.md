# Pinto21 SQLite Module

`p21@npinto21/p21-sqlite@v1` is the first external database driver for Pinto21.

## Metadata

- Repository: `github.com/npinto21/p21-sqlite`
- Current version: see [`VERSION`](./VERSION)
- License: [`Apache License 2.0`](./LICENSE)

## Host integration context

Inside the Pinto21 workspace, the host-side module infrastructure now lives in:

- `src/modules/manifest.*`
- `src/modules/package_resolver.*`
- `src/modules/native/module_native_*`
- `src/cli/mod_main.c`

This module plugs into that host infrastructure through its own `module.p21`, package files, and `native/` implementation.

## Current scope

- open and close a SQLite-backed pool handle
- execute statements with positional parameters
- query a single row
- query row sets
- prepare statements
- begin, commit, and rollback transactions
- structured driver/runtime error objects
- context-aware helpers for cancellation and deadlines

## Design direction

- `sqlite` is a concrete provider for `p21.db`
- the preferred application entrypoint is:
  - `db.Open(sqlite, config)`
- direct driver calls still exist for driver-level testing and standalone use
- the provider keeps:
  - pool-first access
  - structured errors
  - explicit transactions
  - cancellation-aware context calls

## Contract notes

When used through `p21.db`:

- the application should call `db.Open(sqlite, config)`
- `p21.db` attaches core metadata such as:
  - `db.pool`
  - `db.stmt`
  - `db.tx`
- `p21.db` also guards invalid target usage before dispatching to the provider

When used directly through `sqlite.Open(...)`:

- the returned handle remains driver-shaped
- this is useful for driver-level tests, but it is not the recommended application path
- application code should prefer the `p21.db` surface for portability across providers

## Examples

Recommended application-style example through `p21.db`:

```p21
package p21

use p21@npinto21/p21-sqlite@v1 as sqlite
use p21.db as db

pool := db.Open(sqlite, {
    path: "./.recovery/modules/npinto21/sqlite/examples/db/demo.db",
    busy_timeout: 5000,
    max_open: 1,
    max_idle: 1
})

db.Exec(pool, "create table if not exists items (id integer primary key, name text)", [])
db.Exec(pool, "insert into items (name) values (?)", ["nuno"])

row := db.QueryOne(pool, "select id, name from items where name = ?", ["nuno"])

@"driver=%db.DriverName(pool)"\
@"kind=%db.Kind(pool)"\
@"found=%1", row.found\
if (row.found) {
    @"item.id=%1", row.row.id\
    @"item.name=%1", row.row.name\
}

db.Close(pool)
```

Direct driver example for provider-level testing and debugging:

```p21
package p21

use p21@npinto21/p21-sqlite@v1 as sqlite

pool := sqlite.Open({
    path: "./.recovery/modules/npinto21/sqlite/examples/db/sqlite_direct_demo.db",
    busy_timeout: 5000,
    max_open: 1,
    max_idle: 1
})

sqlite.Exec(pool, "create table if not exists items (id integer primary key, name text)", [])
sqlite.Exec(pool, "insert into items (name) values (?)", ["nuno"])

row := sqlite.QueryOne(pool, "select id, name from items where name = ?", ["nuno"])

@"driver=%1", sqlite.DriverName()\
@"found=%1", row.found\

sqlite.Close(pool)
```

Archived example files:

- `examples/basic.p21` — provider-style usage through `p21.db`
- `examples/direct_basic.p21` — direct driver usage through `sqlite.*`
- `examples/probe_open_only.p21` — minimal open probe
- `examples/probe_open_close.p21` — minimal open/close probe
