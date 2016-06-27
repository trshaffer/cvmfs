#include <errno.h>
#include <string.h>
#include "kvstore.h"

using namespace std;  // NOLINT

namespace kvstore {
bool MemoryKvStore::Lookup(const shash::Any &id, MemoryBuffer *mem) {
  MemoryBuffer m;
  if (!mem) mem = &m;
  return Entries.Lookup(id, mem);
}

int64_t MemoryKvStore::GetSize(const shash::Any &id) {
  MemoryBuffer mem;
  if (Lookup(id, &mem)) {
    return mem.size;
  } else {
    return -ENOENT;
  }
}

int64_t MemoryKvStore::GetRefcount(const shash::Any &id) {
  MemoryBuffer mem;
  if (Lookup(id, &mem)) {
    return mem.refcount;
  } else {
    return -ENOENT;
  }
}

bool MemoryKvStore::Ref(const shash::Any &id) {
  MemoryBuffer mem;
  if (Lookup(id, &mem)) {
    // is an overflow check necessary here?
    ++mem.refcount;
    return true;
  } else {
    return false;
  }
}

bool MemoryKvStore::Unref(const shash::Any &id) {
  MemoryBuffer mem;
  if (Lookup(id, &mem) && mem.refcount > 0) {
    --mem.refcount;
    return true;
  } else {
    return false;
  }
}

int64_t MemoryKvStore::Pread(const shash::Any &id, void *buf, uint64_t size, uint64_t offset) {
  MemoryBuffer mem;
  if (Lookup(id, &mem)) {
    uint64_t copy_size = min(mem.size - offset, size);
    memcpy(buf, mem.address + offset, copy_size);
    return copy_size;
  } else {
    return -ENOENT;
  }
}

// consumes the passed-in memory
bool MemoryKvStore::Commit(const shash::Any &id, void *buf, uint64_t size) {
  MemoryBuffer mem;
  bool existed = false;
  if (Lookup(id, NULL)) {
    Delete(id);
    existed = true;
  }
  mem.address = buf;
  mem.size = size;
  mem.refcount = 0; // should commit start at 0 refs?
  used_bytes += size;
  Entries.Insert(id, mem);
  return existed;
}

bool MemoryKvStore::Delete(const shash::Any &id) {
  MemoryBuffer mem;
  if (Lookup(id, &mem)) {
    used_bytes -= mem.size;
    Entries.Forget(id);
    return true;
  } else {
    return false;
  }
}

} // namespace kvstore

namespace lru {
  bool MemoryCache::Insert(const shash::Any &hash, const kvstore::MemoryBuffer &buf) {
    LogCvmfs(kLogLru, kLogDebug, "insert hash --> memory %s -> '%p'",
             hash.ToString().c_str(), buf.address);
    const bool result = LruCache<shash::Any, kvstore::MemoryBuffer>::Insert(hash, buf);
    return result;
  }

  bool MemoryCache::Lookup(const shash::Any &hash, kvstore::MemoryBuffer *buf) {
    const bool found = LruCache<shash::Any, kvstore::MemoryBuffer>::Lookup(hash, buf);
    LogCvmfs(kLogLru, kLogDebug, "lookup hash --> memory: %s (%s)",
             hash.ToString().c_str(), found ? "hit" : "miss");
    return found;
  }

  bool MemoryCache::Forget(const shash::Any &hash) {
    const bool found = LruCache<shash::Any, kvstore::MemoryBuffer>::Forget(hash);
    LogCvmfs(kLogLru, kLogDebug, "forget hash: %s (%s)",
             hash.ToString().c_str(), found ? "hit" : "miss");
    return found;
  }

  void MemoryCache::Drop() {
    LogCvmfs(kLogLru, kLogDebug, "dropping memory cache");
    LruCache<shash::Any, kvstore::MemoryBuffer>::Drop();
  }
} // namespace lru
