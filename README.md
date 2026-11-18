# lsmkv

An embeddable key-value storage engine in C++20, built on a log-structured merge tree (LSM-tree): a write-ahead log, a skip-list memtable, sorted immutable SSTables with bloom filters, and leveled background compaction.

> **Status: early development.** Writes go to a checksummed write-ahead log and a skip-list memtable. A full memtable is flushed by a background thread to an immutable SSTable, recorded in a MANIFEST, and its log is then deleted. Compaction and bloom filters are not built yet, so every flush adds a level-0 table and reads slow down as tables accumulate.

## API

```cpp
#include "lsmkv/db.h"

lsmkv::Options options;
options.create_if_missing = true;

std::unique_ptr<lsmkv::DB> db;
lsmkv::Status s = lsmkv::DB::Open(options, "/tmp/mydb", &db);

s = db->Put(lsmkv::WriteOptions(), "key", "value");

std::string value;
s = db->Get(lsmkv::ReadOptions(), "key", &value);   // s.IsNotFound() if absent

lsmkv::WriteBatch batch;                              // applied atomically
batch.Delete("key");
batch.Put("other", "value");
s = db->Write(lsmkv::WriteOptions(), batch);

auto it = db->NewIterator(lsmkv::ReadOptions());      // a snapshot as of this call
for (it->Seek("a"); it->Valid(); it->Next()) { /* it->key(), it->value() */ }
```

- Keys and values are arbitrary bytes (`std::string_view`, embedded `\0` allowed), ordered byte-wise as unsigned.
- Every fallible call returns a `[[nodiscard]] Status`. The engine never throws.
- A directory can be opened by one `DB` at a time, enforced with a `LOCK` file.

## Durability

A write is **acknowledged** when `Put`, `Delete` or `Write` returns OK.

- **`sync=true`:** the write-ahead log is synced before the call returns (`F_FULLFSYNC` on macOS, `fdatasync` on Linux), so the write survives a process crash, an OS crash and a power loss. It also makes every earlier write durable.
- **`sync=false`:** the write has been handed to the kernel, so it survives a process crash but may be lost on a power loss.
- **Prefix recovery:** after any crash, the database holds exactly the writes up to some point in the acknowledged history: a later write never survives while an earlier one is lost.
- **Atomic batches:** a `WriteBatch` is one log record with one checksum, so after any crash it is either fully applied or absent.
- **Detected corruption:** each log record, each SSTable block and each MANIFEST record carries a CRC32C, checked on every read, so damaged data is never returned. A bad record in the newest log is what a crash leaves behind, so recovery stops there (damage from a failing disk in that log looks the same and is handled the same way). A bad record in any older log, which was fully synced before the next one began, makes `Open` return `Corruption`.
- **Flushes are crash-safe:** a flush writes the SSTable, syncs it and its directory, and then appends a synced edit to the MANIFEST. Only after that edit is durable is the flushed log deleted, so a crash at any point leaves either the log or the table to recover from. Opening a database flushes whatever it replayed from its logs, writes a fresh MANIFEST, and switches the `CURRENT` file to it with an atomic rename.
- **Failed writes stop the database:** after a log write or sync fails, or a background flush fails, every later write returns that error until the database is reopened, because the log may end in a partial record and the flush may be incomplete.

## Building and testing

Requires CMake 3.25+, Ninja, and GCC 13+ or Clang 17+.

```sh
make test      # AddressSanitizer + UndefinedBehaviorSanitizer build, all tests
make tsan      # ThreadSanitizer build, all tests
make release   # optimized build
make bench-micro   # memtable vs std::map microbenchmark (release build)
```

Each target wraps a CMake preset (`debug`, `asan`, `tsan`, `release`), so `cmake --preset asan && cmake --build --preset asan && ctest --preset asan` is equivalent to `make test`.

On macOS, the sanitizer runtimes in older Xcode toolchains can fail at startup on newer macOS releases. `make` picks up Homebrew's LLVM (`brew install llvm`) automatically when it's installed.

CI runs every test under ASan + UBSan and ThreadSanitizer, with both GCC and Clang, on every push.

### Differential testing

`tests/differential_test.cc` runs seeded random sequences of puts, deletes, batches, gets, range scans and snapshot checks against lsmkv and against a `std::map` model, and fails on the first difference. Each failure prints the seed and the operations leading up to it, so a failing run can be replayed exactly:

```sh
LSMKV_DIFF_SEED=42 LSMKV_DIFF_OPS=1000000 ./build/asan/lsmkv_tests --gtest_filter='Seeds/*'
```

### Crash testing

`tests/crash_test.cc` crashes the engine at random points and checks, after every recovery, that the database equals the result of some prefix of the operations it ran, and that the prefix includes every write the engine promised to keep.

- **Power loss** runs on an in-memory file system that tracks how much of each file, and which directory entries, were synced. Power fails just before a random file operation (inside a write, a log rotation, a flush, or recovery itself). Unsynced bytes are dropped or torn at 512-byte sector granularity, and unsynced creates, renames and removes are either all lost or partly kept in the order they were made, as on a journaling file system. Every `sync=true` write must survive.
- **Every step of a flush and of recovery:** two more tests run a flush, and an `Open` that replays a log, with the power failing just before each file operation in turn, and check that no durable write is lost and that the database opens.
- **Kill** forks a writer on the real file system and `SIGKILL`s it mid-write. Every acknowledged write must survive, synced or not.

Only power-loss mode catches a missing fsync: a killed process loses nothing that reached the kernel. Removing the log sync, the directory sync after creating a log, the old log's sync on rotation, or the CRC check makes the power-loss test fail while the kill test still passes. So does each step of the flush order: dropping the SSTable's sync or its directory's sync, or deleting the log before the MANIFEST edit is synced.

```sh
LSMKV_CRASH_SEED=17 ./build/asan/lsmkv_tests --gtest_filter='CrashTest.*'
```

The random power-loss test also crashes the background flush thread, whose timing varies, so a seed reproduces the workload but not always the exact crash point. The step-by-step tests are deterministic.

### Large runs

`tests/large_test.cc` writes a chosen amount of data to the real disk, reopens it, scans everything and reads random keys back. It is skipped unless `LSMKV_LARGE_MB` is set:

```sh
make release
LSMKV_LARGE_MB=1024 ./build/release/lsmkv_tests --gtest_filter='LargeTest.*'
```

## Inspecting SSTables

`sst_dump` prints a table file's footer, index and every entry (user key, sequence number, type and value), and exits non-zero if any checksum fails:

```sh
./build/release/sst_dump /path/to/db/000012.sst
```

## Layout

```
include/lsmkv/   public API: db.h, options.h, status.h, iterator.h, write_batch.h
src/env/         Env: the only code that touches the file system (POSIX, and an in-memory
                 fault-injecting one for tests)
src/wal/         write-ahead log record format, writer and reader
src/table/       SSTable format: blocks with restart points, table writer and reader
src/db/          the engine (lsm_db), recovery, the MANIFEST and versions, WriteBatch,
                 internal keys, iterators
src/memtable/    arena allocator, skip list, memtable
src/util/        Status, byte encodings, CRC32C
tests/           unit tests, the differential tester and the crash tests
tools/           sst_dump
bench/           microbenchmarks (Google Benchmark)
```

A database directory holds `NNNNNN.wal` logs, `NNNNNN.sst` tables, `MANIFEST-NNNNNN` (the log of which tables are live), `CURRENT` (the name of the MANIFEST in use) and `LOCK`. All numbered files share one counter.

All file I/O goes through the `Env` interface. File names are changed only through a `Directory` object, which is also the object that syncs them, because a new or renamed file isn't durable until its directory is synced.
