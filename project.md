# DB Manager

Desktop application สำหรับจัดการ Database โดยเน้นความเรียบง่าย ใช้งานเร็ว และสามารถเชื่อมต่อ Database ผ่าน IP ได้โดยตรง

## 1. เป้าหมาย

สร้าง Desktop Database Manager ที่สามารถ:

* Connect Database ผ่าน IP / Hostname
* รองรับ PostgreSQL
* รองรับ Redis
* รองรับ MySQL ในอนาคต
* ดู Database / Schema / Table
* ดูและแก้ไขข้อมูลใน Table
* ดู Redis Keys และ Values
* Execute SQL / Redis Commands
* ดู Query / Command Result
* ดู Table Structure
* Backup / Restore
* Export / Import
* รองรับหลาย Connection
* ทำงานโดยไม่ต้องมี Backend Server กลาง

---

# 2. Technology Stack

## Language / Build

* **C++20** (Core / UI)
* **C** libraries สำหรับ Database client
* **CMake** — dependency ผ่าน Homebrew (`find_package` / pkg-config); vcpkg ไว้ทีหลังเมื่อต้อง build Windows / CI
* Build เป็น native binary: macOS / Windows / Linux

## Desktop / UI

**Qt 6 (Qt Widgets)**

รับผิดชอบ:

* UI
* Database Explorer (`QTreeView`)
* SQL Editor (`QPlainTextEdit` + `QSyntaxHighlighter` หรือ QScintilla)
* Redis Explorer
* Data Grid (`QTableView` + `QAbstractTableModel`)
* Connection Dialog
* Query / Command History
* Tabs (`QTabWidget`)

## Core

**C++** (อยู่ใน process เดียวกับ UI ไม่มี IPC / Bridge)

รับผิดชอบ:

* Database Connection
* SQL Execution
* Redis Connection
* Redis Commands
* Schema Introspection
* Backup / Restore
* Export / Import
* SSH Tunnel
* File System
* Connection Management

## Libraries

| งาน          | Library                                   |
| ------------ | ----------------------------------------- |
| PostgreSQL   | **libpq** (C)                             |
| Redis        | **hiredis** (C, รองรับ TLS ผ่าน hiredis_ssl) |
| MySQL        | libmysqlclient / MariaDB Connector/C (Future) |
| SQLite       | **sqlite3** (C) (Future)                  |
| SSH Tunnel   | **libssh2**                               |
| TLS          | OpenSSL                                   |
| Config / JSON | Qt (`QJsonDocument`, `QSettings`)        |
| Password     | Qt Keychain (macOS Keychain / Windows Credential Manager) |

## Threading

Database call ทั้งหมดเป็น blocking ห้ามรันบน UI thread

* แต่ละ Connection มี worker thread ของตัวเอง (`QThread`)
* UI ส่งคำสั่งเข้า worker และรับผลกลับผ่าน Qt signal / slot
* Query ที่ใช้เวลานานต้องยกเลิกได้ (`PQcancel` สำหรับ PostgreSQL)

---

# 3. Supported Databases

Architecture ต้องออกแบบให้ Database แต่ละประเภทแยก implementation ออกจากกัน

```text
Database Manager
│
├── PostgreSQL
│
├── Redis
│
├── MySQL
│
└── SQLite
```

Version แรก:

```text
PostgreSQL
Redis
```

Future:

```text
MySQL
SQLite
```

---

# 4. Architecture

```text
┌───────────────────────────────────────────────┐
│                  DB Manager                   │
│                                               │
│  ┌─────────────────────────────────────────┐  │
│  │         Qt 6 Widgets (UI thread)        │  │
│  │                                         │  │
│  │ PostgreSQL │ Redis │ SQL │ Data        │  │
│  └───────────────────┬─────────────────────┘  │
│                      │ signal / slot          │
│  ┌───────────────────▼─────────────────────┐  │
│  │        C++ Core (worker threads)        │  │
│  │                                         │  │
│  │ Connection Manager                      │  │
│  │     ├── PostgreSQL  (libpq)             │  │
│  │     ├── Redis       (hiredis)           │  │
│  │     ├── MySQL       (libmysqlclient)    │  │
│  │     └── SQLite      (sqlite3)           │  │
│  └───────────────┬─────────────────────────┘  │
└──────────────────┼────────────────────────────┘
                   │ TCP / TLS / SSH Tunnel
          ┌────────┴─────────┐
          ▼                  ▼
   PostgreSQL              Redis
   192.168.1.100           192.168.1.101
   :5432                   :6379
```

ไม่มี Web Backend Server กลาง และไม่มี WebView — ทั้งหมดเป็น native binary ตัวเดียว

---

# 5. Connection

ทุก Connection มีประเภท:

```text
PostgreSQL
Redis
MySQL
SQLite
```

ตัวอย่าง PostgreSQL:

```text
Type:     PostgreSQL
Host:     192.168.1.100
Port:     5432
Database: my_database
Username: postgres
Password: ********
SSL Mode: disable
```

ตัวอย่าง Redis:

```text
Type:     Redis
Host:     192.168.1.101
Port:     6379
Username: default
Password: ********
Database: 0
TLS:      disabled
```

รองรับ:

```text
localhost
127.0.0.1
192.168.x.x
10.x.x.x
172.16.x.x
hostname
Public IP
```

---

# 6. Redis

Redis จะมี UI และการทำงานต่างจาก SQL Database

ไม่ควรพยายามทำ Redis ให้เหมือน Table ของ PostgreSQL

## Redis Explorer

ตัวอย่าง:

```text
Redis

▼ Connection
  ├── Database 0
  │   ├── user:1
  │   ├── user:2
  │   ├── session:abc
  │   ├── cache:product:1
  │   └── queue:jobs
  │
  ├── Database 1
  │
  └── Database 2
```

---

# 7. Redis Key Browser

แสดง:

```text
┌──────────────────────────────────────────────┐
│ Redis Keys                                   │
├──────────────────────────────────────────────┤
│ Key                    │ Type │ TTL │ Size    │
├────────────────────────┼──────┼─────┼─────────┤
│ user:1                 │ HASH │ -1  │ 5       │
│ user:2                 │ HASH │ 300 │ 5       │
│ session:abc            │ STRING│ 120 │ 1KB   │
│ products               │ LIST │ -1  │ 120     │
└──────────────────────────────────────────────┘
```

รองรับ:

* Key Search
* Pattern Search
* Pagination / Scan
* Delete Key
* Rename Key
* TTL
* Type
* Memory Size

ควรใช้ `SCAN` แทน `KEYS *` เพื่อหลีกเลี่ยงการ block Redis ใน production

---

# 8. Redis Value Viewer

รองรับ Redis Data Types:

```text
STRING
LIST
SET
SORTED SET
HASH
STREAM
```

ตัวอย่าง STRING:

```text
Key:
session:abc

Type:
STRING

TTL:
120 seconds

Value:
eyJhbGciOi...
```

ตัวอย่าง HASH:

```text
Key:
user:1

Type:
HASH

┌──────────┬──────────────────┐
│ Field    │ Value            │
├──────────┼──────────────────┤
│ id       │ 1                │
│ username │ admin            │
│ email    │ admin@example... │
└──────────┴──────────────────┘
```

---

# 9. Redis Commands

มี Command Editor สำหรับ Redis

ตัวอย่าง:

```text
GET user:1
```

หรือ:

```text
HGETALL user:1
```

หรือ:

```text
TTL session:abc
```

สามารถ Execute Redis Command และแสดง Result

```text
> HGETALL user:1

id
1
username
admin
email
admin@example.com
```

---

# 10. Redis Operations

รองรับพื้นฐาน:

```text
GET
SET
DEL
EXISTS
TTL
EXPIRE
TYPE
SCAN
```

Hash:

```text
HGET
HSET
HGETALL
HDEL
```

List:

```text
LPUSH
RPUSH
LPOP
RPOP
LRANGE
```

Set:

```text
SADD
SREM
SMEMBERS
```

Sorted Set:

```text
ZADD
ZREM
ZRANGE
```

---

# 11. Redis Database Selection

Redis มี logical databases:

```text
DB 0
DB 1
DB 2
...
```

UI:

```text
Redis
│
├── DB 0
├── DB 1
├── DB 2
└── DB 3
```

ผู้ใช้สามารถเปลี่ยน Database ได้

```text
SELECT 0
SELECT 1
SELECT 2
```

---

# 12. Redis Server Information

แสดงข้อมูล:

```text
Redis Server

Version:        7.2.4
Uptime:         12 days
Connected:      5
Memory Used:    128 MB
Memory Peak:    240 MB
Keys:           12,450
Commands/sec:   125
```

สามารถใช้:

```text
INFO
DBSIZE
MEMORY
```

เพื่อดึงข้อมูล

---

# 13. Redis Backup

รองรับ Redis backup ในอนาคต

เช่น:

```text
BGSAVE
```

และแสดงสถานะ:

```text
Backup started...

Status:
Running

Last Save:
2026-10-06 16:20:30
```

ไม่ควรให้ UI ทำ `SAVE` บน production โดยไม่เตือนผู้ใช้ เพราะอาจ block Redis

---

# 14. Database Abstraction

Core ควรแยก interface ตามประเภท Database

ตัวอย่างแนวคิด:

```cpp
class Database {
public:
    virtual ~Database() = default;
    virtual void connect() = 0;      // throw DbError เมื่อล้มเหลว
    virtual void disconnect() = 0;
    virtual void ping() = 0;
    virtual Capabilities capabilities() const = 0;
};
```

PostgreSQL:

```cpp
class PostgreSQL : public Database {
    PGconn* conn_ = nullptr;         // libpq
    ...
};
```

Redis:

```cpp
class Redis : public Database {
    redisContext* ctx_ = nullptr;    // hiredis
    ...
};
```

ห่อ C handle ด้วย RAII (`std::unique_ptr` + custom deleter เช่น `PQfinish`, `redisFree`, `PQclear`) เพื่อกัน resource leak

ไม่ควรพยายามบังคับ Redis ให้ implement API ที่มีเฉพาะ SQL เช่น:

```cpp
getTables();
getColumns();
executeSql();
```

แต่ให้แต่ละ Database มี capability ของตัวเอง (UI ใช้ `dynamic_cast` หรือเช็ค `capabilities()` ก่อนเรียก API เฉพาะ)

---

# 15. Capability System

แต่ละ Connection บอกได้ว่ารองรับอะไร

ตัวอย่าง PostgreSQL:

```text
PostgreSQL
├── Query
├── Tables
├── Views
├── Functions
├── Indexes
├── Foreign Keys
├── Transactions
├── Backup
└── Export
```

Redis:

```text
Redis
├── Keys
├── String
├── Hash
├── List
├── Set
├── Sorted Set
├── Stream
├── TTL
├── Memory
├── Server Info
└── Backup
```

UI จะใช้ Capability เพื่อแสดง UI ที่เหมาะสม

```cpp
enum class Capability : uint32_t {
    Query = 1 << 0, Tables = 1 << 1, Views = 1 << 2, ...
    Keys  = 1 << 10, Ttl = 1 << 11, ServerInfo = 1 << 12, ...
};
```

---

# 16. Project Structure

```text
db-manager/
│
├── CMakeLists.txt
├── .gitignore
│
├── src/
│   ├── main.cpp
│   │
│   ├── ui/                      # Qt Widgets
│   │   ├── MainWindow.{h,cpp}
│   │   ├── connection/          # Connection Dialog
│   │   ├── postgres/            # Database Explorer, Data Grid
│   │   ├── redis/               # Key Browser, Value Viewer
│   │   ├── sql-editor/          # Editor + Highlighter
│   │   └── backup/
│   │
│   └── core/                    # ไม่ depend Qt Widgets
│       ├── database/
│       │   ├── Database.h
│       │   ├── postgres/
│       │   ├── redis/
│       │   └── mysql/
│       │
│       ├── connection/          # Connection Manager + Worker
│       ├── query/
│       ├── schema/
│       ├── backup/
│       ├── export/
│       └── ssh/
│
├── resources/                   # icons, .qrc
├── tests/                       # GoogleTest / Qt Test
│
└── README.md
```

---

# 17. Development Phases

## Phase 1 — PostgreSQL MVP

* [x] CMake Project (Qt 6, libpq, hiredis)
* [x] Main Window (Qt Widgets)
* [x] Worker Thread + signal / slot
* [x] PostgreSQL Connection
* [x] Test Connection
* [x] Connection Manager
* [x] Database Explorer
* [x] SQL Editor
* [x] Execute Query
* [x] Query Result

## Phase 2 — Redis

* [x] Redis Connection
* [x] Test Connection
* [x] Database Selection
* [x] Key Browser
* [x] SCAN
* [x] Key Search
* [x] Key Details
* [x] String Viewer
* [x] Hash Viewer
* [x] List Viewer
* [x] Set Viewer
* [x] Sorted Set Viewer
* [x] TTL
* [x] Delete Key
* [x] Redis Command Editor

## Phase 3 — PostgreSQL Data Manager

* [x] Table Data
* [ ] Pagination
* [ ] Search
* [ ] Filter
* [ ] Sort
* [x] Insert
* [x] Update
* [x] Delete

## Phase 4 — Schema Manager

* [ ] Columns
* [ ] Indexes
* [ ] Foreign Keys
* [ ] Constraints
* [ ] Views
* [ ] Functions
* [ ] Generate CREATE SQL

## Phase 5 — Backup

* [ ] PostgreSQL Backup
* [ ] PostgreSQL Restore
* [ ] Redis Backup
* [ ] Export SQL
* [ ] CSV Export
* [ ] CSV Import

## Phase 6 — Advanced

* [ ] SSH Tunnel
* [ ] Multiple SQL Tabs
* [ ] Query History
* [ ] ER Diagram
* [ ] MySQL
* [ ] SQLite
* [ ] Redis Streams
* [ ] Redis Pub/Sub Monitor

---

# 18. Initial Goal

เป้าหมายแรก:

```text
เปิดโปรแกรม
    ↓
New Connection
    ↓
เลือก PostgreSQL / Redis
    ↓
ใส่ IP + Port + Credentials
    ↓
Test Connection
    ↓
Connect
    ↓
แสดง Database Explorer
```

PostgreSQL:

```text
PostgreSQL
    ↓
Database
    ↓
Schema
    ↓
Table
    ↓
Data
    ↓
SQL
```

Redis:

```text
Redis
    ↓
Database 0
    ↓
SCAN Keys
    ↓
เลือก Key
    ↓
ดู Type / TTL / Value
    ↓
Edit / Delete
```

**เป้าหมายของ MVP คือให้ PostgreSQL และ Redis ใช้งานได้จริงก่อน แล้วค่อยเพิ่ม MySQL / SQLite และฟีเจอร์ขั้นสูง**
