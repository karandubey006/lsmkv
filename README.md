# lsmkv

An embeddable key-value storage engine in C++20, built on a log-structured merge tree (LSM-tree): a write-ahead log, a skip-list memtable, sorted immutable SSTables with bloom filters, and leveled background compaction.

> **Status: early development.** The public API, the file-system abstraction, the build and CI, and a randomized differential tester are in place. The memtable is built and tested on its own: a skip list with one writer and lock-free concurrent readers, an arena allocator, and multi-version internal keys. `DB::Open` still returns a temporary in-memory engine backed by `std::map`, which keeps no data on disk. It is being replaced by the real LSM-tree one component at a time.

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

## Layout

```
include/lsmkv/   public API: db.h, options.h, status.h, iterator.h, write_batch.h
src/env/         Env: the only code that touches the file system (POSIX implementation)
src/db/          DB::Open, WriteBatch, internal keys (dbformat), the temporary std::map engine
src/memtable/    arena allocator, skip list, memtable
src/util/        Status, byte encodings
tests/           unit tests and the differential tester
bench/           microbenchmarks (Google Benchmark)
```

All file I/O goes through the `Env` interface, so tests can substitute a fault-injecting implementation that simulates power loss by discarding every byte that was never fsynced.
