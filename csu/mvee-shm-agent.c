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

#include <sys/shm.h>

// ========================================================================================================================
// Forward declarations for the original (ifunc) implementations of mem* functions
// ========================================================================================================================
extern __typeof (memcpy)  orig_memcpy;
extern __typeof (memmove) orig_memmove;
extern __typeof (memset)  orig_memset;

// ========================================================================================================================
// Types of SHM operations
// ========================================================================================================================
enum
{
  LOAD            = 0,// Instruction::LoadInst
  STORE           = 1,// Instruction::StoreInst
  ATOMICRMW       = 2,// Instruction::AtomicRMWInst
  ATOMICCMPXCHG   = 3,// Instruction::AtomicCmpXchgInst

  GLIBC_FUNC_BASE = 128,
  MEMCPY          = GLIBC_FUNC_BASE,
  MEMMOVE         = GLIBC_FUNC_BASE + 1,
  MEMSET          = GLIBC_FUNC_BASE + 2,
};


// ========================================================================================================================
// Decoding address for specific variant, using tag
// ========================================================================================================================
unsigned long mvee_shm_tag;

static void* mvee_shm_decode_address(const void* address)
{
  unsigned long high = (unsigned long)address & 0xffffffff00000000ull;
  unsigned long low  = (unsigned long)address & 0x00000000ffffffffull;
  return (void*) ((high ^ mvee_shm_tag) + low);
}

// ========================================================================================================================
// Assertions and errors
// ========================================================================================================================
__attribute__((noinline))
static void mvee_assert_same_address(const void* a, const void* b)
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
static void mvee_assert_same_store(const void* a, const void* b, unsigned long size)
{
	if (memcmp(a, b, size))
		*(volatile long*)0 = 1;
}

__attribute__((noinline))
static void mvee_assert_same_type(unsigned char a, unsigned char b)
{
	if (a != b)
		*(volatile long*)0 = a;
}

__attribute__((noinline))
static void mvee_assert_same_value(int a, int b)
{
	if (a != b)
		*(volatile long*)0 = a;
}

__attribute__((noinline))
static void mvee_error_unsupported_id(unsigned long id)
{
  *(volatile long*)0 = (long)id;
}

__attribute__((noinline))
static void mvee_assert_mapping_exists(void *mapping)
{
  if (!mapping)
    *(volatile long*)0 = 1;
}

__attribute__((noinline))
static void mvee_assert_equal_mapping_size(size_t len, size_t size)
{
  if (len != size)
    *(volatile long*)0 = len - size;
}

__attribute__((noinline))
static void mvee_error_mapping_not_inserted(void *addr)
{
  *(volatile long*)0 = (volatile long) addr;
}

// ========================================================================================================================
// All functionality related to the SHM buffer
// ========================================================================================================================
struct mvee_shm_op_entry {
  const void* address;
  const void* second_address;
  size_t size;
  unsigned char type;
  char data[];
};

struct mvee_shm_op_ret {
  unsigned long val;
  bool cmp;
};

static __thread size_t                mvee_shm_local_pos    = 0; // our position in the thread local queue
static __thread char*                 mvee_shm_buffer       = NULL;
static __thread size_t                mvee_shm_buffer_size  = 0; // nr of slots in the thread local queue

static struct mvee_shm_op_entry* mvee_shm_get_entry(size_t size)
{
  // Get the buffer if we don't have it yet
  if (unlikely(!mvee_shm_buffer))
    mvee_shm_buffer = (char*)syscall(__NR_shmat, syscall(MVEE_GET_SHARED_BUFFER, 0, MVEE_SHM_BUFFER, &mvee_shm_buffer_size, 1), NULL, 0);

  // Find location for entry in buffer
  size_t entry_size = MVEE_ROUND_UP(sizeof(struct mvee_shm_op_entry) + size, sizeof(size_t));
  if (unlikely(mvee_shm_local_pos + entry_size >= mvee_shm_buffer_size))
  {
    syscall(MVEE_FLUSH_SHARED_BUFFER, MVEE_SHM_BUFFER);
    mvee_shm_local_pos = 0;
  }

  // Calculate entry, update pos, and return
  struct mvee_shm_op_entry* entry = (struct mvee_shm_op_entry*) (mvee_shm_buffer + mvee_shm_local_pos);
  mvee_shm_local_pos += entry_size;
  return entry;
}

// type         : type of operation
// shm_address  : (one of) the address(es) of the operation that is on the SHM page
// in_address   : a secondary helper address from which can be read, which might also be on the SHM page
// out_address  : a secondary helper address to which can be written, which might also be on the SHM page
// size         : the number of accessed bytes
// value        : a helper value
static inline void mvee_shm_buffered_op(unsigned char type, const void* shm_address, const void* in_address, void* out_address, size_t size, uint64_t value)
{
  // If the memory operation has no size, don't do it, don't make an entry, and don't even check
  // This is an easier course than making entries with a size of 0, which would fuck up synchronization
  if (!size)
    return;

  // Get an entry
  struct mvee_shm_op_entry* entry = mvee_shm_get_entry(size);

  // Do actual operation, differently for leader and follower(s)
  if (likely(mvee_master_variant))
  {
    entry->address = shm_address;
    entry->type = type;

    switch(type)
    {
      case MEMCPY:
      case MEMMOVE:
          entry->second_address = value ? in_address : NULL;// if value is set, shm_address equals out_address, and in_address is also on a SHM page
      case LOAD:
      case STORE:
        {
          orig_memcpy(&entry->data, in_address, size);
          (type == MEMMOVE) ? orig_memmove(out_address, in_address, size) : orig_memcpy(out_address, in_address, size);
          break;
        }
      case MEMSET:
        {
          orig_memset(&entry->data, value, 1);
          orig_memset(out_address, value, size);
          break;
        }
      default:
        break;// TODO: unsupported operation!
    }

    // Signal entry is ready
    orig_atomic_store_release(&entry->size, size);
  }
  else
  {
    // Wait until entry is ready
    while (!orig_atomic_load_acquire(&entry->size))
      syscall(__NR_sched_yield);

    // Assert we're on the same entry
    mvee_assert_same_address(entry->address, shm_address);
    mvee_assert_same_size(entry->size, size);
    mvee_assert_same_type(entry->type, type);

    switch(type)
    {
      case LOAD:
        {
          orig_memcpy(out_address, &entry->data, size);
          break;
        }
      case STORE:
        {
          mvee_assert_same_store(&entry->data, in_address, size);
          break;
        }
      case MEMCPY:
      case MEMMOVE:
        {
          mvee_assert_same_address(entry->second_address, value ? in_address : NULL);

          // Check if the input comes from a non-SHM page
          if (entry->address != in_address && entry->second_address != in_address)
            mvee_assert_same_store(&entry->data, in_address, size);

          // Check if the output goes to a non-SHM page
          if (entry->address != out_address && entry->second_address != out_address)
            (type == MEMCPY) ? orig_memcpy(out_address, &entry->data, size) : orig_memmove(out_address, &entry->data, size);

          break;
        }
      case MEMSET:
        {
          mvee_assert_same_value(*((int*)entry->data), value);
          break;
        }
      default:
        break;// TODO: unsupported operation!
    }
  }
}

// ========================================================================================================================
// The mvee_shm_op interface used by the wrapping shm_support compiler pass
// ========================================================================================================================
struct mvee_shm_op_ret mvee_shm_op(unsigned char id, bool atomic, void* address, unsigned long size, unsigned long value, unsigned long cmp)
{
  struct mvee_shm_op_ret ret = { 0 };

  address = mvee_shm_decode_address(address);

  if (atomic)
  {
     // Atomic operations, hand over to MVEE
  }
  else
  {
    // Non-atomic operations, use SHM buffer

    if (id == LOAD)
      mvee_shm_buffered_op(id, address, address, &ret.val, size, 0);
    else if (id == STORE)
      mvee_shm_buffered_op(id, address, &value, address, size, 0);
    else
      mvee_error_unsupported_id(id);
  }

  return ret;
}

// ========================================================================================================================
// The MVEE SHM-specific implementations for the mem* functions.
// These functions decode tagged addresses, and use the SHM buffer.
// ========================================================================================================================
void *
mvee_shm_memcpy (void *__restrict dest, const void *__restrict src, size_t n)
{
  /* Decode addresses */
  if ((unsigned long)src & 0x8000000000000000ull)
  {
    if ((unsigned long)dest & 0x8000000000000000ull)
      mvee_shm_buffered_op(MEMCPY, mvee_shm_decode_address(dest), mvee_shm_decode_address(src), mvee_shm_decode_address(dest), n, 1);
    else
      mvee_shm_buffered_op(MEMCPY, mvee_shm_decode_address(src), mvee_shm_decode_address(src), dest, n, 0);
  }
  else if ((unsigned long)dest & 0x8000000000000000ull)
    mvee_shm_buffered_op(MEMCPY, mvee_shm_decode_address(dest), src, mvee_shm_decode_address(dest), n, 0);

  return dest;
}

void *
mvee_shm_memmove (void *dest, const void * src, size_t n)
{
  /* Decode addresses */
  if ((unsigned long)src & 0x8000000000000000ull)
  {
    if ((unsigned long)dest & 0x8000000000000000ull)
      mvee_shm_buffered_op(MEMMOVE, mvee_shm_decode_address(dest), mvee_shm_decode_address(src), mvee_shm_decode_address(dest), n, 1);
    else
      mvee_shm_buffered_op(MEMMOVE, mvee_shm_decode_address(src), mvee_shm_decode_address(src), dest, n, 0);
  }
  else if ((unsigned long)dest & 0x8000000000000000ull)
    mvee_shm_buffered_op(MEMMOVE, mvee_shm_decode_address(dest), src, mvee_shm_decode_address(dest), n, 0);

  return dest;
}

void *
mvee_shm_memset (void *dest, int ch, size_t len)
{
  /* Decode addresses */
  void* shm_dest = mvee_shm_decode_address(dest);

  mvee_shm_buffered_op(MEMSET, shm_dest, NULL, shm_dest, len, ch);
  return dest;
}


// ========================================================================================================================
// bookkeeping
// ========================================================================================================================
struct shared_mapping_info_t
{
  struct shared_mapping_info_t* previous;
  void* base;
  void* shadow;
  void* bitmap;
  size_t size;
  struct shared_mapping_info_t* next;
};
static struct shared_mapping_info_t* shared_mapping_info = NULL;

static void add_shared_mapping_info(void *base, void *shadow, void *bitmap, size_t size)
{
  struct shared_mapping_info_t *to_add = (struct shared_mapping_info_t *) malloc(sizeof(struct shared_mapping_info_t));
  to_add->previous = NULL;
  to_add->base = base;
  to_add->shadow = shadow;
  to_add->bitmap = bitmap;
  to_add->size = size;
  to_add->next = NULL;


  if (shared_mapping_info == NULL)
  {
    shared_mapping_info = to_add;
    return;
  }

  struct shared_mapping_info_t *iterator = shared_mapping_info;

  if (iterator->base >= (base + size))
  {
    iterator->previous = to_add;
    to_add->next = iterator;
    shared_mapping_info = to_add;
    return;
  }

  while (iterator && iterator->next)
  {
    if (((iterator->base + iterator->size) < base) && (iterator->next->base >= (base + size)))
    {
      iterator->next->previous = to_add;
      to_add->next = iterator->next;
      iterator->next = to_add;
      to_add->previous = iterator;
      return;
    }
    iterator = iterator->next;
  }

  if ((iterator->base + iterator->size) < base)
  {
    iterator->next = to_add;
    to_add->previous = iterator;
    return;
  }

  // should be unreachable
  mvee_error_mapping_not_inserted(base);
}

static struct shared_mapping_info_t* find_shared_mapping_info(const void* addr)
{
  struct shared_mapping_info_t* iterator = shared_mapping_info;
  while (iterator)
  {
    if (iterator->base <= addr && (iterator->base + iterator->size) > addr)
      return iterator;
    iterator = iterator->next;
  }
  mvee_assert_mapping_exists(iterator);
  return iterator;
}

#define SHARED_TO_SHADOW_POINTER(__mapping, __address)                                                                 \
__mapping->shadow + (__address - __mapping->base);

static struct shared_mapping_info_t* remove_shared_mapping_info(const void* mapping)
{
  struct shared_mapping_info_t* remove = find_shared_mapping_info(mapping);
  if (remove)
  {
    if (remove->previous)
      remove->previous->next = remove->next;
    if (remove->next)
      remove->next->previous = remove->previous;
  }
  return remove;
}

// ========================================================================================================================
// Hooks for mmap and related functions
// ========================================================================================================================
void *
mvee_shm_mmap (void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
  unsigned long ret = orig_MMAP_CALL(addr, len, prot, flags, fd, offset);
  if ((flags & MAP_SHARED) && fd && fd != -1 && (void *) ret != MAP_FAILED)
  {
    struct stat fd_stat;
    fstat(fd, &fd_stat);
    if (!(fd_stat.st_mode & O_RDWR))
      return (void *) ret;

    // the arguments will be filled in by the MVEE anyway
    void *shadow = (void *) orig_SHMAT_CALL(0, 0, 0);
    if ((void *) shadow == (void *) -1)
    {
      int errno_temp = errno;
      orig_MUNMAP_CALL((void *) ret, len);
      errno = errno_temp;
      return MAP_FAILED;
    }

    // the arguments will be filled in by the MVEE anyway
    void *bitmap = (void *) orig_SHMAT_CALL(0, 0, 0);
    if ((mvee_master_variant && (void *) bitmap == (void *) -1) ||
        (!mvee_master_variant && (void *) bitmap != (void *) -1))
    {
      int errno_temp = errno;
      orig_MUNMAP_CALL((void *) ret, len);
      orig_SHMDT_CALL(shadow);
      errno = errno_temp;
      return MAP_FAILED;
    }

    // Do bookkeeping
    add_shared_mapping_info(mvee_shm_decode_address((void *) ret), shadow, bitmap, len);
  }
  return (void *)ret;
}


void *
mvee_shm_shmat (int shmid, const void *shmaddr, int shmflg)
{
  void* mapping = orig_SHMAT_CALL(shmid, shmaddr, shmflg);
  if (mapping == (void*) -1)
    return mapping;

  // the arguments will be filled in by the MVEE anyway
  void* shadow = (void*) orig_SHMAT_CALL(0, 0, 0);
  if (shadow == (void*) -1)
  {
    int errno_temp = errno;
    orig_SHMDT_CALL(mapping);
    errno = errno_temp;
    return (void*) -1;
  }

  // the arguments will be filled in by the MVEE anyway
  void* bitmap = (void*) orig_SHMAT_CALL(0, 0, 0);
  if ((mvee_master_variant && (void *) bitmap == (void*) -1) ||
      (!mvee_master_variant && (void *) bitmap != (void*) -1))
  {
    int errno_temp = errno;
    orig_SHMDT_CALL(mapping);
    orig_SHMDT_CALL(shadow);
    errno = errno_temp;
    return (void*) -1;
  }

  struct shmid_ds shm_info;
  // just kinda assuming this won't fail at this point, should have succeeded if this point is reached...
  shmctl(shmid, IPC_STAT, &shm_info);
  add_shared_mapping_info(mvee_shm_decode_address(mapping), shadow, bitmap, shm_info.shm_segsz);

  return mapping;
}

int
mvee_shm_shmdt (const void *shmaddr)
{
  if ((unsigned long long) shmaddr & 0x8000000000000000ull)
  {
    struct shared_mapping_info_t* mapping = remove_shared_mapping_info(mvee_shm_decode_address(shmaddr));

    orig_SHMDT_CALL(mapping->shadow);
    orig_SHMDT_CALL(mapping->bitmap); // todo this is -1 in followers, might not be the best idea actually
    free(mapping);
  }
  return orig_SHMDT_CALL(shmaddr);
}

int
mvee_shm_munmap (const void *addr, size_t len)
{
  if ((unsigned long long) addr & 0x8000000000000000ull)
  {
    struct shared_mapping_info_t* mapping = remove_shared_mapping_info(mvee_shm_decode_address(addr));
    // I'm too lazy to do the parial munmap stuff, so just adding this to maybe have an idea when it does happen
    mvee_assert_equal_mapping_size(len, mapping->size);

    orig_SHMDT_CALL(mapping->shadow);
    orig_SHMDT_CALL(mapping->bitmap); // todo this is -1 in followers, might not be the best idea actually
    free(mapping);
  }
  return orig_MUNMAP_CALL(addr, len);
}
