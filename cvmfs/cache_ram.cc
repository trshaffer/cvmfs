/**
 * This file is part of the CernVM File System.
 */
#include "cvmfs_config.h"
#include "cache_ram.h"

#include <errno.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>

#include "kvstore.h"
#include "smalloc.h"
#include "util/posix.h"
#include "util_concurrency.h"

using namespace std;  // NOLINT

namespace cache {


int RamCacheManager::AddFd(const ReadOnlyFd &fd) {
  unsigned i = 0;
  for ( ; i < open_fds_.size(); ++i) {
    if (open_fds_[i].handle == kInvalidHandle) {
      open_fds_[i] = fd;
      return i;
    }
  }
  if (open_fds_.size() < kMaxHandles) {
    open_fds_.push_back(fd);
    return i;
  } else {
    return -ENFILE;
  }
}

bool RamCacheManager::AcquireQuotaManager(QuotaManager *quota_mgr) {
  return false;
}


int RamCacheManager::Open(const shash::Any &id) {
  WriteLockGuard guard(rwlock_);
  return DoOpen(id);
}

int RamCacheManager::DoOpen(const shash::Any &id) {
  bool rc;
  kvstore::MemoryBuffer buf;
  int fd = AddFd(ReadOnlyFd(id, 0));
  if (fd < 0) return fd;

  if (pinned_entries_.IncRef(id)) {
    return fd;
  } else if (regular_entries_.PopBuffer(id, &buf) ||
             volatile_entries_.PopBuffer(id, &buf)) {
    rc = pinned_entries_.Commit(id, buf);
    assert(rc);
    rc = pinned_entries_.IncRef(id);
    assert(rc);
    return fd;
  } else {
    open_fds_[fd].handle = kInvalidHandle;
    return -ENOENT;
  }
}


int64_t RamCacheManager::GetSize(int fd) {
  ReadLockGuard guard(rwlock_);
  if (!IsValid(fd)) return -EBADFD;
  return pinned_entries_.GetSize(open_fds_[fd].handle);
}


int RamCacheManager::Close(int fd) {
  bool rc;
  kvstore::MemoryBuffer buf;
  ReadOnlyFd file_descriptor;
  bool sweep_tail = false;
  {
    ReadLockGuard guard(rwlock_);
    if (!IsValid(fd)) return -EBADFD;
    file_descriptor = open_fds_[fd];
    rc = pinned_entries_.GetBuffer(file_descriptor.handle, &buf);
    assert(rc);
  }

  WriteLockGuard guard(rwlock_);
  rc = pinned_entries_.Unref(file_descriptor.handle);
  assert(rc);
  if (pinned_entries_.GetRefcount(file_descriptor.handle) == 0) {
    switch (buf.object_type) {
    case cache::CacheManager::kTypeRegular:
      rc = pinned_entries_.PopBuffer(file_descriptor.handle, &buf);
      assert(rc);
      rc = regular_entries_.Commit(file_descriptor.handle, buf);
      assert(rc);
      break;
    case cache::CacheManager::kTypeVolatile:
      rc = pinned_entries_.PopBuffer(file_descriptor.handle, &buf);
      assert(rc);
      rc = volatile_entries_.Commit(file_descriptor.handle, buf);
      assert(rc);
      break;
    case cache::CacheManager::kTypePinned:
    case cache::CacheManager::kTypeCatalog:
      // just leave it in the pinned cache
      break;
    }
    open_fds_[fd].handle = kInvalidHandle;
    sweep_tail = (static_cast<unsigned>(fd) == (open_fds_.size() - 1));
  }

  if (sweep_tail) {
    unsigned last_good_idx = open_fds_.size() - 1;
    while (open_fds_[last_good_idx].handle != kInvalidHandle)
      last_good_idx--;
    open_fds_.resize(last_good_idx + 1);
  }

  return 0;
}


int64_t RamCacheManager::Pread(
  int fd,
  void *buf,
  uint64_t size,
  uint64_t offset)
{
  ReadLockGuard guard(rwlock_);
  if (!IsValid(fd)) return -EBADFD;
  return pinned_entries_.Read(open_fds_[fd].handle, buf, size, offset);
}


int RamCacheManager::Dup(int fd) {
  bool rc;
  WriteLockGuard guard(rwlock_);
  ReadOnlyFd file_descriptor;
  if (!IsValid(fd)) return -EBADFD;
  file_descriptor = open_fds_[fd];
  rc = pinned_entries_.IncRef(file_descriptor.handle);
  assert(rc);
  return AddFd(file_descriptor);
}


/**
 * For a RAM cache, read-ahead is a no-op.
 */
int RamCacheManager::Readahead(int fd) {
  ReadLockGuard guard(rwlock_);
  if (!IsValid(fd))
    return -EBADFD;
  return 0;
}


int RamCacheManager::StartTxn(const shash::Any &id, uint64_t size, void *txn) {
  Transaction *transaction = new (txn) Transaction();
  transaction->id = id;
  transaction->pos = 0;
  transaction->expected_size = size;
  transaction->size = (size == kSizeUnknown) ? kPageSize : size;
  if (transaction->size) {
    transaction->buffer = scalloc(1, transaction->size);
  }
  return 0;
}


void RamCacheManager::CtrlTxn(
  const string &description,
  const ObjectType type,
  const int flags,
  void *txn)
{
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);
  transaction->description = description;
  transaction->object_type = type;
}


int64_t RamCacheManager::Write(const void *buf, uint64_t size, void *txn) {
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);

  if (transaction->pos + size > transaction->size) {
    if (transaction->expected_size == kSizeUnknown) {
      transaction->size = max(2*transaction->size, size + transaction->pos);
      transaction->buffer = realloc(transaction->buffer, transaction->size);
      if (!transaction->buffer) return -errno;
    } else {
      return -ENOSPC;
    }
  }


  uint64_t copy_size = min(size, transaction->size - transaction->pos);
  memcpy(static_cast<char *>(transaction->buffer) + transaction->pos,
         buf, copy_size);
  transaction->pos += copy_size;
  return copy_size;
}


int RamCacheManager::Reset(void *txn) {
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);
  transaction->pos = 0;
  return 0;
}


int RamCacheManager::OpenFromTxn(void *txn) {
  WriteLockGuard guard(rwlock_);
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);
  int64_t retval = CommitToKvStore(transaction);
  if (retval < 0) return retval;
  return DoOpen(transaction->id);
}


int RamCacheManager::AbortTxn(void *txn) {
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);
  if (transaction->buffer)
    free(transaction->buffer);
  return 0;
}


int RamCacheManager::CommitTxn(void *txn) {
  WriteLockGuard guard(rwlock_);
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);
  return CommitToKvStore(transaction);
}


int64_t RamCacheManager::CommitToKvStore(Transaction *transaction) {
  bool rc;
  kvstore::MemoryBuffer buf;
  buf.address = transaction->buffer;
  if (transaction->expected_size == kSizeUnknown) {
    buf.size = transaction->pos;
    buf.address = realloc(buf.address, buf.size);
    if (!buf.address) return -errno;
  } else {
    buf.size = transaction->size;
  }
  buf.refcount = 0;
  buf.object_type = transaction->object_type;

  // this entire section needs to be protected by a write lock or the accounting
  // will be all messed up
  uint64_t pinned_size = pinned_entries_.GetUsed();
  uint64_t regular_size = regular_entries_.GetUsed();
  uint64_t volatile_size = volatile_entries_.GetUsed();
  uint64_t total_size = pinned_size + regular_size + volatile_size + buf.size;
  if (total_size > max_size_) {
    if (pinned_size + regular_size + buf.size <= max_size_) {
      rc = volatile_entries_.ShrinkTo(
        volatile_size - (total_size - max_size_));
      assert(rc);
    } else if (pinned_size + buf.size <= max_size_) {
      rc = volatile_entries_.ShrinkTo(0);
      assert(rc);
      rc = regular_entries_.ShrinkTo(
        regular_size - (total_size - max_size_) + volatile_size);
      assert(rc);
    } else {
      return -ENOSPC;
    }
  }

  switch (buf.object_type) {
  case cache::CacheManager::kTypeRegular:
    if (!regular_entries_.Commit(transaction->id, buf)) return -EEXIST;
    break;
  case cache::CacheManager::kTypeVolatile:
    if (!volatile_entries_.Commit(transaction->id, buf)) return -EEXIST;
    break;
  case cache::CacheManager::kTypePinned:
  case cache::CacheManager::kTypeCatalog:
    if (!pinned_entries_.Commit(transaction->id, buf)) return -EEXIST;
    break;
  }
  return 0;
}

}  // namespace cache
