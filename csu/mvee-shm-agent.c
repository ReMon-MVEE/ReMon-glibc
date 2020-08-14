#include "mvee-agent-shared.h"

#include <fcntl.h>
#include <mmap_internal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <assert.h>
#include <atomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct mvee_shm_op_entry {
  void* address;
  size_t size;
  bool store;
  char data[];
};

struct mvee_shm_op_ret {
  unsigned long val;
  bool cmp;
};

static __thread size_t                mvee_shm_local_pos    = 0; // our position in the thread local queue
static __thread char*                 mvee_shm_buffer       = NULL;
static __thread size_t                mvee_shm_buffer_size  = 0; // nr of slots in the thread local queue

unsigned long mvee_shm_tag;

static void* mvee_shm_decode_address(const void* address)
{
  unsigned long high = (unsigned long)address & 0xffffffff00000000ull;
  unsigned long low  = (unsigned long)address & 0x00000000ffffffffull;
  return (void*) ((high ^ mvee_shm_tag) + low);
}

// ========================================================================================================================
// ASSERTIONS
// ========================================================================================================================

__attribute__((noinline))
static void mvee_assert_is_load(bool store)
{
	if (store)
		*(volatile long*)0 = store;
}

__attribute__((noinline))
static void mvee_assert_is_store(bool store)
{
	if (!store)
		*(volatile long*)0 = store;
}

__attribute__((noinline))
static void mvee_assert_same_address(void* a, void* b)
{
	if (a != b)
		*(volatile long*)0 = (long)a;
}

__attribute__((noinline))
static void mvee_assert_same_size(unsigned long a, unsigned long b)
{
	if (a != b)
		*(volatile long*)0 = a;
}

__attribute__((noinline))
static void mvee_assert_same_store(void* a, void* b, unsigned long size)
{
	if (memcmp(a, b, size))
		*(volatile long*)0 = 1;
}

struct mvee_shm_op_ret mvee_shm_op(unsigned char id, bool atomic, void* address, unsigned long size, unsigned long value, unsigned long cmp)
{
  struct mvee_shm_op_ret ret = { 0 };

  if (unlikely(!mvee_shm_buffer))
    mvee_shm_buffer = (char*)syscall(__NR_shmat, syscall(MVEE_GET_SHARED_BUFFER, 0, MVEE_SHM_BUFFER, &mvee_shm_buffer_size, 1), NULL, 0);

  address = mvee_shm_decode_address(address);

  if (atomic) // Atomic operations
  {
    // Hand over to MVEE
  }
  else // Non-atomic operations
  {
    size_t entry_size = MVEE_ROUND_UP(sizeof(struct mvee_shm_op_entry) + size, sizeof(size_t));
    mvee_shm_local_pos += entry_size;

    if (unlikely(mvee_shm_local_pos >= mvee_shm_buffer_size))
    {
      syscall(MVEE_FLUSH_SHARED_BUFFER, MVEE_SHM_BUFFER);
      mvee_shm_local_pos = 0;
    }

    struct mvee_shm_op_entry* entry = (struct mvee_shm_op_entry*) (mvee_shm_buffer + mvee_shm_local_pos);
    if (likely(mvee_master_variant))
    {
      entry->address = address;

      if (id == 0) // Instruction::Load
      {
        entry->store = false;
        memcpy(&entry->data, address, size);
        memcpy(&ret.val, address, size);
      }
      else // Instruction::Store
      {
        entry->store = true;
        memcpy(&entry->data, &value, size);
        memcpy(address, &value, size);
      }

      // Signal entry is ready
      orig_atomic_store_release(&entry->size, size);
    }
    else
    {
      // Wait until entry is ready
      while (!orig_atomic_load_acquire(&entry->size))
        syscall(__NR_sched_yield);

      mvee_assert_same_address(entry->address, address);
      mvee_assert_same_size(entry->size, size);

      if (id == 0) // Instruction::Load
      {
        mvee_assert_is_load(entry->store);

        memcpy(&ret.val, &entry->data, size);
      }
      else // Instruction::Store
      {
        mvee_assert_is_store(entry->store);
        mvee_assert_same_store(&entry->data, &value, size);
      }
    }
  }

  return ret;
}
