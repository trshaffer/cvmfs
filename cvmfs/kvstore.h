/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_KVSTORE_H_
#define CVMFS_KVSTORE_H_

#include <pthread.h>
#include <unistd.h>

#include <string>

#include "cache.h"
#include "lru.h"
#include "statistics.h"

using namespace std;  // NOLINT

namespace kvstore {

struct MemoryBuffer {
  void *address;
  size_t size;
  unsigned int refcount;
  cache::CacheManager::ObjectType object_type;
};

class MemoryKvStore :SingleCopy {
 public:
  MemoryKvStore(
    unsigned int cache_entries,
    const string &name,
    perf::Statistics *statistics)
    : used_bytes_(0)
    , entries_(cache_entries, shash::Any(), lru::hasher_any,
        statistics, name) {
    int retval = pthread_rwlock_init(&rwlock_, NULL);
    assert(retval == 0);
  }

  ~MemoryKvStore() {
    pthread_rwlock_destroy(&rwlock_);
  }

  /**
   * Get the size in bytes of the entry at id
   * @param id The hash key
   * @returns A size, or -ENOENT if the entry is absent
   */
  int64_t GetSize(const shash::Any &id);

  /**
   * Get the number of references to the entry at id
   * @param id The hash key
   * @returns A reference count, or -ENOENT if the entry is absent
   */
  int64_t GetRefcount(const shash::Any &id);

  /**
   * Increase the reference count on the entry at id
   * @param id The hash key
   * @returns True if the entry exists and was updated
   */
  bool IncRef(const shash::Any &id);

  /**
   * Decrease the reference count on the entry at id. If the refcount is zero, no effect
   * @param id The hash key
   * @returns True if the entry exists and was updated
   */
  bool Unref(const shash::Any &id);

  /**
   * Copy a portion of the entry at id to the given address. See pread(2)
   * @param id The hash key
   * @param buf The address at which to write the data
   * @param size The number of bytes to copy
   * @param offset The offset within the entry to start the copy
   * @returns The number of bytes copied, or -ENOENT if the entry is absent
   */
  int64_t Read(
    const shash::Any &id,
    void *buf,
    size_t size,
    size_t offset);

  /**
   * Insert a new memory buffer. The KvStore takes ownership of the referred memory, so
   * callers must not free() it themselves
   * @param id The hash key
   * @param buf The memory buffer to insert
   * @returns True iff the commit succeeds
   */
  bool Commit(const shash::Any &id, const kvstore::MemoryBuffer &buf);

  /**
   * Delete an entry, free()ing its memory. Note that the entry not have any references
   * @param id The hash key
   * @returns True iff the entry was successfully deleted
   */
  bool Delete(const shash::Any &id);

  /**
   * Delete the oldest entries until the KvStore uses less than the given size.
   * Entries with nonzero refcount will not be deleted.
   * @param size The maximum size to make the KvStore
   * @returns True iff the shrink succeeds
   */
  bool ShrinkTo(size_t size);

  /**
   * Get the memory buffer describing the entry at id
   * @param id The hash key
   * @returns True iff the entry is present
   */
  bool GetBuffer(const shash::Any &id, MemoryBuffer *buf);

  /**
   * Get the memory buffer describing the entry at id as in @ref GetBuffer,
   * and remove the entry *without* freeing the associated memory
   * @param id The hash key
   * @returns True iff the entry is present
   */
  bool PopBuffer(const shash::Any &id, MemoryBuffer *buf);

  /**
   * Get the total space used for data
   */
  size_t GetUsed() { return used_bytes_; }

 private:
  size_t used_bytes_;
  lru::LruCache<shash::Any, MemoryBuffer> entries_;
  pthread_rwlock_t rwlock_;
  bool DoDelete(const shash::Any &id);
};
}  // namespace kvstore
#endif  // CVMFS_KVSTORE_H_