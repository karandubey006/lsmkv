# lsmkv

An embeddable key-value storage engine in C++20, built on a log-structured merge tree (LSM-tree): a write-ahead log, a skip-list memtable, sorted immutable SSTables with bloom filters, and leveled background compaction.

> **Status: early development.** Writes are durable: every write goes to a checksummed write-ahead log and then to a skip-list memtable, and opening a database replays its logs. SSTables, flushing, compaction and bloom filters are not built yet, so the whole database lives in memory and its logs are never deleted.

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
- **Detected corruption:** each log record carries a CRC32C, so damaged data is never returned. A bad record in the newest log is what a crash leaves behind, so recovery stops there and truncates the log (damage from a failing disk in that log looks the same and is handled the same way). A bad record in any older log, which was fully synced before the next one began, makes `Open` return `Corruption`.
- **Failed writes stop the database:** after a log write or sync fails, every later write returns that error until the database is reopened, because the log may end in a partial record.

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

- **Power loss** runs on an in-memory file system that tracks how much of each file, and which directory entries, were synced. Power fails just before a random file operation (inside a write, a log rotation, or recovery itself), and every unsynced byte is dropped or torn at 512-byte sector granularity. Every `sync=true` write must survive.
- **Kill** forks a writer on the real file system and `SIGKILL`s it mid-write. Every acknowledged write must survive, synced or not.

Only power-loss mode catches a missing fsync: a killed process loses nothing that reached the kernel. Removing the log sync, the directory sync after creating a log, the old log's sync on rotation, the tail truncation in recovery, or the CRC check each makes the power-loss test fail, while the kill test still passes.

```sh
LSMKV_CRASH_SEED=17 ./build/asan/lsmkv_tests --gtest_filter='CrashTest.*'
```

## Layout

```
include/lsmkv/   public API: db.h, options.h, status.h, iterator.h, write_batch.h
src/env/         Env: the only code that touches the file system (POSIX, and an in-memory
                 fault-injecting one for tests)
src/wal/         write-ahead log record format, writer and reader
src/db/          the engine (lsm_db), recovery, WriteBatch, internal keys, iterators
src/memtable/    arena allocator, skip list, memtable
src/util/        Status, byte encodings, CRC32C
tests/           unit tests, the differential tester and the crash tests
bench/           microbenchmarks (Google Benchmark)
```

All file I/O goes through the `Env` interface. File names are changed only through a `Directory` object, which is also the object that syncs them, because a new or renamed file isn't durable until its directory is synced.
