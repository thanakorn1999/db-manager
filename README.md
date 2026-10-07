# DB Manager

Native desktop DB manager (PostgreSQL + Redis) — C++20 / Qt 6 Widgets / libpq / hiredis. See `project.md`.

## Build (macOS)

```sh
brew install cmake qt qtkeychain libpq hiredis
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix qtkeychain);$(brew --prefix libpq)"
cmake --build build -j
open build/db-manager.app
```

## Test

```sh
ctest --test-dir build                                   # unit checks
DBM_TEST_PG=localhost:5432 DBM_TEST_REDIS=127.0.0.1:6379 ctest --test-dir build --output-on-failure
```

The Redis live test uses DB 15 and runs `FLUSHDB` on it.

## Shortcuts (⌘ = Ctrl on Windows/Linux)

| Key | Action |
| --- | --- |
| ⌘N | New connection |
| ⌘T | New SQL editor (selected PostgreSQL connection/database) |
| ⌘R | Refresh selected explorer node |
| ⌘W | Close current tab |
| ⌃⇥ / ⌃⇧⇥, ⌘⇧] / ⌘⇧[ | Next / previous tab |
| ⌘1…⌘8, ⌘9 | Go to tab N, last tab |
| ⌘↵ | Run SQL (selection only if text is selected) |
| ⌘. | Cancel running query |
| ⌘, | Settings (result grid colours) |
| ⌃Space | SQL: show suggestions (they also pop up while typing; Tab / ↵ accepts, Esc closes) |
| ⌘C | Copy selected cells (tab-separated) |
| ⌘S | Save grid edits (SQL) / save string value (Redis) |
| ⌘⌫ / Delete | SQL grid: mark selected rows for deletion (again to unmark) |
| ⌘F | Redis: focus key pattern |
| ⌫ / Delete | Redis: delete selected key / selected items (asks first) |

## SQL autocomplete

Suggestions follow the clause you're in: statement keywords at the start, tables after `FROM` / `JOIN` / `UPDATE` / `INTO`, columns of the tables in the statement (`alias.` narrows to one table; names found in several tables are qualified), and the next likely keywords (`WHERE`, `JOIN`, `ORDER BY`, `AND`…).
After `JOIN`, tables linked by a foreign key come first with the condition filled in — picking `orders o ON o.user_id = u.id` writes the whole thing. The catalog reloads after `CREATE` / `ALTER` / `DROP`.

## Result grid

- **Column tags** — coloured dots in the header: PK, FK, UNIQUE, INDEX (hover for details, e.g. `FK → public.users(id)`). Tag colours are in Settings (⌘,).
- **Foreign keys** — FK cells get a → on the right; click it to open the parent row in a new tab (`SELECT * FROM parent WHERE pk = value`). Composite FKs work when all their columns are in the result.
- **Value colours** (Settings) — text / background for TRUE, FALSE, NULL, empty text (shown as `<EMPTY>`), "not important" columns, and optionally key columns (off by default: the header tags mark them). Edit highlights win over all of these.
- Rows alternate colours.

## Table icons

Tables in the explorer get an emoji guessed from their name (`users` → 🧑, `orders` → 🧾, `products` → 📦, `user_roles` → 🔑 — the last word counts most); no match keeps the file icon. Right-click → **Set Icon…** to pick another or type any emoji; **Automatic** goes back to the guess. Stored per `schema.table`, shared by all connections.

## Editing

- **PostgreSQL** — any result from a single table that includes its primary key is editable: double-click a cell, `+ Row`, `− Row`, `Set NULL`, then **Save** (one transaction; every UPDATE/DELETE must hit exactly one row or nothing is written). The status bar says why a result is read-only.
- **Redis** — edits are written immediately: hash/list values, set/zset members, zset scores (double-click), `+ Add` / `− Remove` items, `+ Key`, `Rename…`, `TTL…`, string values (`Save`, keeps the TTL).

## Export

Right-click a **table** → **Export…** saves that table (CSV / Excel / JSON / SQL INSERTs). Right-click a **database** or **schema** → **Export…**: pick the format (CSV / Excel / JSON), tick tables (or **Select all**), then a folder: one file per table, `schema.table.ext`. You're asked before existing files are replaced.

## Build and run

cmake --build build -j && open build/db-manager.app

## License

[MIT](LICENSE) © 2026 Jamezarkk. Roadmap: [ROADMAP.md](ROADMAP.md).
