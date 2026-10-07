# Roadmap

Native PostgreSQL + Redis manager (C++20 / Qt 6). Design notes: [project.md](project.md).
Want to help? Pick an unchecked item, open an issue to say you're on it, then send a PR.

## Done

- PostgreSQL: connections, database / schema / table explorer, SQL editor (tabs, highlighting, context-aware autocomplete, cancel)
- PostgreSQL data: edit cells, insert / delete rows in one transaction, FK jump to parent row, column tags (PK / FK / UNIQUE / INDEX)
- Redis: connections, DB selection, SCAN key browser + search, viewers / editors for string, hash, list, set, sorted set, TTL, rename, delete, command editor
- Export: CSV, Excel (.xlsx), JSON (data + structure), SQL INSERTs; whole database / schema to one file per table
- Backup: `pg_dump`
- ER diagram, table icons, Settings (grid colours), passwords in the OS keychain

## Next — PostgreSQL data

- [ ] Pagination for big tables (single-row mode instead of loading the whole result)
- [ ] Search / filter / sort in the result grid
- [ ] Query history
- [ ] Restore (`pg_restore` / run .sql)
- [ ] Import CSV
- [ ] Progress + cancel for long exports and backups

## Schema manager

- [ ] Columns, indexes, foreign keys, constraints view
- [ ] Views and functions in the explorer
- [ ] Generate `CREATE` SQL

## Redis

- [ ] Server info (`INFO`, `DBSIZE`, memory)
- [ ] Streams viewer
- [ ] Pub/Sub monitor
- [ ] Backup (`BGSAVE` with status, warn before `SAVE`)
- [ ] Paging for huge collections (viewer caps at 1000 elements today)

## Connections

- [ ] SSH tunnel (libssh2)
- [ ] TLS options UI

## More databases

- [ ] MySQL / MariaDB
- [ ] SQLite

## Platforms

- [ ] Windows and Linux builds (vcpkg)
- [ ] CI (build + tests on push)
- [ ] Signed release builds
