#include "mvee-agent-shared.h"

#include <fcntl.h>
#include <libc-lock.h>
#include <mmap_internal.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <assert.h>
#include <atomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ========================================================================================================================
// Forward declarations for the original (ifunc) implementations of mem* functions
// ========================================================================================================================
extern __typeof (memcpy)  orig_memcpy;
extern __typeof (memmove) orig_memmove;
extern __typeof (memset)  orig_memset;
extern __typeof (memchr)  orig_memchr;

// ========================================================================================================================
// Types of SHM operations
// ========================================================================================================================
enum
{
  LOAD            = 0,// Instruction::LoadInst
  STORE           = 1,// Instruction::StoreInst
  ATOMICCMPXCHG   = 2,// Instruction::AtomicCmpXchgInst
  ATOMICRMW_FIRST = 3,// Instruction::AtomicRMWInst
  ATOMICRMW_XCHG  = ATOMICRMW_FIRST +  0,// Instruction::AtomicRMWInst::Xchg, *p = v
  ATOMICRMW_ADD   = ATOMICRMW_FIRST +  1,// Instruction::AtomicRMWInst::Add, *p = old + v
  ATOMICRMW_SUB   = ATOMICRMW_FIRST +  2,// Instruction::AtomicRMWInst::Sub, *p = old - v
  ATOMICRMW_AND   = ATOMICRMW_FIRST +  3,// Instruction::AtomicRMWInst::And, *p = old & v
  ATOMICRMW_NAND  = ATOMICRMW_FIRST +  4,// Instruction::AtomicRMWInst::Nand, *p = ~(old & v)
  ATOMICRMW_OR    = ATOMICRMW_FIRST +  5,// Instruction::AtomicRMWInst::Or, *p = old | v
  ATOMICRMW_XOR   = ATOMICRMW_FIRST +  6,// Instruction::AtomicRMWInst::Xor, *p = old ^ v
  ATOMICRMW_MAX   = ATOMICRMW_FIRST +  7,// Instruction::AtomicRMWInst::Max, *p = old >signed v ? old : v
  ATOMICRMW_MIN   = ATOMICRMW_FIRST +  8,// Instruction::AtomicRMWInst::Min, *p = old <signed v ? old : v
  ATOMICRMW_UMAX  = ATOMICRMW_FIRST +  9,// Instruction::AtomicRMWInst::UMax, *p = old >unsigned v ? old : v
  ATOMICRMW_UMIN  = ATOMICRMW_FIRST + 10,// Instruction::AtomicRMWInst::UMin, *p = old <unsigned v ? old : v
  ATOMICRMW_FADD  = ATOMICRMW_FIRST + 11,// Instruction::AtomicRMWInst::FAdd, *p = old + v
  ATOMICRMW_FSUB  = ATOMICRMW_FIRST + 12,// Instruction::AtomicRMWInst::FSub, *p = old - v
  ATOMICRMW_LAST  = ATOMICRMW_FSUB,

  GLIBC_FUNC_BASE = 128,
  MEMCPY          = GLIBC_FUNC_BASE + 0,
  MEMMOVE         = GLIBC_FUNC_BASE + 1,
  MEMSET          = GLIBC_FUNC_BASE + 2,
  MEMCHR          = GLIBC_FUNC_BASE + 3,
};

#define ATOMICRMW_LEADER(__operation) \
entry->value = value;                                                                                              \
uint32_t shm_val = __operation((uint32_t*)in_address, value, __ATOMIC_RELAXED);                                    \
uint32_t shadow_val = __operation((uint32_t*)SHARED_TO_SHADOW_POINTER(in, in_address), value, __ATOMIC_RELAXED);   \
*((uint32_t*)out_address) = shm_val;                                                                               \
entry->data_present = shm_val != shadow_val;                                                                       \
if (entry->data_present)                                                                                           \
  orig_memcpy(&entry->data, &shm_val, size);

#define ATOMICRMW_FOLLOWER(__operation)                                                                            \
mvee_assert_same_value(entry->value, value);                                                                       \
if (entry->data_present)                                                                                           \
  memcpy(SHARED_TO_SHADOW_POINTER(in, in_address), &entry->data, size);                                            \
*((uint32_t*)out_address) = __operation((uint32_t*)SHARED_TO_SHADOW_POINTER(in, in_address), value, __ATOMIC_RELAXED);

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
static void mvee_assert_equal_mapping_size(size_t len, size_t size)
{
  if (len != size)
    *(volatile long*)0 = len - size;
}

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
static void mvee_error_unsupported_operation(unsigned long id)
{
  *(volatile long*)0 = (long)id;
}

__attribute__((noinline))
static void mvee_error_shm_entry_not_present(const void *addr)
{
  *(volatile long*)0 = (volatile long) addr;
}

// ========================================================================================================================
// Everything table-related
// ========================================================================================================================
typedef struct mvee_shm_table_entry {
  void* address;
  void* shadow;
  size_t len;
  struct mvee_shm_table_entry* prev;
  struct mvee_shm_table_entry* next;
} mvee_shm_table_entry;

static mvee_shm_table_entry* mvee_shm_table_head = NULL;

__libc_rwlock_define_initialized (, mvee_shm_table_lock attribute_hidden)

void mvee_shm_table_add_entry(void* address, void* shadow, size_t len)
{
  __libc_rwlock_wrlock(mvee_shm_table_lock);

  /* Prepare entry */
  mvee_shm_table_entry *entry = (mvee_shm_table_entry *) malloc(sizeof(mvee_shm_table_entry));
  entry->address = address;
  entry->shadow = shadow;
  entry->len = len;
  entry->prev = NULL;
  entry->next = NULL;

  /* Insert entry, or make it the new head if none exists yet */
  if (mvee_shm_table_head)
  {
    mvee_shm_table_entry *iterator = mvee_shm_table_head;

    /* Insert in a sorted order, so make our entry the new head if necessary */
    if ((address + len) <= iterator->address)
    {
      iterator->prev = entry;
      entry->next = iterator;
      mvee_shm_table_head = entry;
    }
    else
    {
      /* Insert half-way? */
      while (iterator->next)
      {
        if (((iterator->address + iterator->len) <= address) && ((address + len) <= iterator->next->address))
        {
          iterator->next->prev = entry;
          entry->next = iterator->next;
          iterator->next = entry;
          entry->prev = iterator;
          break;
        }
        iterator = iterator->next;
      }

      /* Insert at the end */
      if (!iterator->next)
      {
        /* Do sanity check, should not be possible. */
        if (address < (iterator->address + iterator->len))
          mvee_error_shm_entry_not_present(address);

        iterator->next = entry;
        entry->prev = iterator;
      }
    }
  }
  else
    mvee_shm_table_head = entry;

  __libc_rwlock_unlock(mvee_shm_table_lock);
}

static mvee_shm_table_entry* mvee_shm_table_get_entry(const void* address)
{
  __libc_rwlock_rdlock(mvee_shm_table_lock);

  mvee_shm_table_entry* entry = mvee_shm_table_head;
  while (entry)
  {
    /* If we found the entry, quit */
    if (((uintptr_t)entry->address <= (uintptr_t)address) && ((uintptr_t)address < ((uintptr_t)entry->address + entry->len)))
      break;

    entry = entry->next;
  }

  __libc_rwlock_unlock(mvee_shm_table_lock);

  return entry;
}

static bool mvee_shm_table_delete_entry(mvee_shm_table_entry* remove)
{
  if (remove)
  {
    __libc_rwlock_wrlock(mvee_shm_table_lock);

    /* Special case for head */
    if (remove == mvee_shm_table_head)
      mvee_shm_table_head = remove->next;

    /* Unlink */
    if (remove->prev)
      remove->prev->next = remove->next;
    if (remove->next)
      remove->next->prev = remove->prev;

    /* Free memory */
    free(remove);

    __libc_rwlock_unlock(mvee_shm_table_lock);
    return true;
  }

  return false;
}

#define SHARED_TO_SHADOW_POINTER(__mapping, __address)                                                                 \
(__mapping->shadow + (__address - __mapping->address))

// ========================================================================================================================
// All functionality related to the SHM buffer
// ========================================================================================================================
typedef struct mvee_shm_op_entry {
  const void* address;
  const void* second_address;
  size_t size;
  uint32_t value;
  uint32_t cmp;
  unsigned char type;
  bool data_present;
  char data[];
} mvee_shm_op_entry;

static __thread size_t                mvee_shm_local_pos    = 0; // our position in the thread local queue
static __thread char*                 mvee_shm_buffer       = NULL;
static __thread size_t                mvee_shm_buffer_size  = 0; // nr of slots in the thread local queue

static mvee_shm_op_entry* mvee_shm_get_entry(size_t size)
{
  // Get the buffer if we don't have it yet
  if (unlikely(!mvee_shm_buffer))
    mvee_shm_buffer = (char*)syscall(__NR_shmat, syscall(MVEE_GET_SHARED_BUFFER, 0, MVEE_SHM_BUFFER, &mvee_shm_buffer_size, 1), NULL, 0);

  // Find location for entry in buffer
  size_t entry_size = MVEE_ROUND_UP(sizeof(mvee_shm_op_entry) + size, sizeof(size_t));
  if (unlikely(mvee_shm_local_pos + entry_size >= mvee_shm_buffer_size))
  {
    syscall(MVEE_FLUSH_SHARED_BUFFER, MVEE_SHM_BUFFER);
    mvee_shm_local_pos = 0;
  }

  // Calculate entry, update pos, and return
  mvee_shm_op_entry* entry = (mvee_shm_op_entry*) (mvee_shm_buffer + mvee_shm_local_pos);
  mvee_shm_local_pos += entry_size;
  return entry;
}

// type         : type of operation
// in_address   : the input address from which can be read, which might be on the SHM page
// in           : the SHM metadata for the input address, or NULL if it isn't in shared memory
// out_address  : the output address to which can be written, which might be on the SHM page
// out          : the SHM metadata for the output address, or NULL if it isn't in shared memory
// size         : the number of accessed bytes
// value        : a helper value
// cmp          : a comparison value
static inline bool mvee_shm_buffered_op(unsigned char type, const void* in_address, const mvee_shm_table_entry* in, void* out_address, const mvee_shm_table_entry* out, size_t size, uint32_t value, uint32_t cmp)
{
  bool ret = false;
  // If the memory operation has no size, don't do it, don't make an entry, and don't even check
  // This is an easier course than making entries with a size of 0, which would fuck up synchronization
  if (!size)
    return ret;

  // Get an entry
  mvee_shm_op_entry* entry = mvee_shm_get_entry(size);
  const void* shm_address = out ? out_address : in_address;
  const void* shm_address2 = (in && out) ? in_address : NULL;

  // Do actual operation, differently for leader and follower(s)
  if (likely(mvee_master_variant))
  {
    entry->address = shm_address;
    entry->type = type;

    switch(type)
    {
      case MEMCPY:
      case MEMMOVE:
          entry->second_address = shm_address2;
      case LOAD:
      case STORE:
          {
           /* When doing reads/writes we will use memcpy, **or** memmove if so requested. We can relax certain memmove's however, if we now
            * **for a fact** that the destination and source won't overlap. For example, when handling MEMMOVE we can still perform writes from
            * non-SHM pages to local shadow copy pages using memcpy.
            */
            if (in)
            {
              /* The input comes from a SHM page. Check whether it differs from our local copy or not. If it doesn't, we use our local copy as input.
               * If it **does** differ, we copy the modified data on the SHM page to the buffer, and use that copy as input.
               */
              entry->data_present = memcmp(SHARED_TO_SHADOW_POINTER(in, in_address), in_address, size);
              if (entry->data_present)
                orig_memcpy(&entry->data, in_address, size);
              const void* buf_or_shadow = (entry->data_present) ? &entry->data : SHARED_TO_SHADOW_POINTER(in, in_address);

              if (out)
              {
                /* We're reading/writing to and from a SHM page. */
                /* Write to actual SHM page, from (non-overlapping) buffer or local shadow copy */
                orig_memcpy(out_address, buf_or_shadow, size);

                /* Write local shadow copy, from buffer (can memcpy!) or local shadow copy (memmove, if requested) */
                if (type == MEMMOVE && !entry->data_present)
                  orig_memmove(SHARED_TO_SHADOW_POINTER(out, out_address), buf_or_shadow, size);
                else
                  orig_memcpy(SHARED_TO_SHADOW_POINTER(out, out_address), buf_or_shadow, size);
              }
              else
              {
                /* The input comes from a SHM page, the output goes to a non-SHM page. */
                /* Write to non-SHM page, from (non-overlapping) buffer or local shadow copy */
                orig_memcpy(out_address, buf_or_shadow, size);
              }
            }
            else
            {
              /* The input comes from a non-SHM page, fill in the buffer */
              orig_memcpy(&entry->data, in_address, size);

              /* Write to actual SHM page, from (non-overlapping) non-SHM page */
              orig_memcpy(out_address, in_address, size);

              /* Write local shadow copy, from (non-overlapping) non-SHM page */
              orig_memcpy(SHARED_TO_SHADOW_POINTER(out, out_address), in_address, size);
            }
            break;
          }
      case MEMSET:
        {
          /* Fill in entry */
          entry->value = value;

          /* Write to actual SHM page */
          orig_memset(out_address, value, size);

          /* Write local shadow copy */
          orig_memset(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
          break;
        }
      case MEMCHR:
        {
          /* Fill in entry */
          entry->value = value;

          /* Search on actual SHM page */
          void* shm_ret = orig_memchr(in_address, value, size);

          /* Search on local shadow copy */
          void* local_ret = orig_memchr(SHARED_TO_SHADOW_POINTER(in, in_address), value, size);

          /* Write offset as return value */
          *(ptrdiff_t*)out_address = shm_ret - in_address;

          /* In case these differ, we have to return.. A different pointer in the followers? We'll encode the pointer offset in the buffer, reusing the second_address field */
          entry->data_present = (shm_ret != local_ret);
          if (entry->data_present)
            entry->second_address = (void*)(shm_ret - in_address);

          break;
        }
      case ATOMICCMPXCHG:
        {
          entry->value = value;
          entry->cmp = cmp;
          ret = __atomic_compare_exchange_n((uint32_t*)in_address, &cmp, value, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
          bool shadow_cmp = __atomic_compare_exchange_n((uint32_t*)SHARED_TO_SHADOW_POINTER(in, in_address), &cmp, value, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
          entry->data_present = (ret != shadow_cmp);
          if (entry->data_present)
             entry->data[0] = ret;
          break;
        }
      case ATOMICRMW_XCHG:
        {
          ATOMICRMW_LEADER(__atomic_exchange_n);
          break;
        }
      case ATOMICRMW_ADD:
        {
          ATOMICRMW_LEADER(__atomic_fetch_add);
          break;
        }
      case ATOMICRMW_SUB:
        {
          ATOMICRMW_LEADER(__atomic_fetch_sub);
          break;
        }
      case ATOMICRMW_AND:
        {
          ATOMICRMW_LEADER(__atomic_fetch_and);
          break;
        }
      case ATOMICRMW_NAND:
        {
          ATOMICRMW_LEADER(__atomic_fetch_nand);
          break;
        }
      case ATOMICRMW_OR:
        {
          ATOMICRMW_LEADER(__atomic_fetch_or);
          break;
        }
      case ATOMICRMW_XOR:
        {
          ATOMICRMW_LEADER(__atomic_fetch_xor);
          break;
        }
      default:
        mvee_error_unsupported_operation(type);
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
      case MEMCPY:
      case MEMMOVE:
        mvee_assert_same_address(entry->second_address, shm_address2);
      case LOAD:
      case STORE:
        {
          if (in)
          {
            /* The input comes from a SHM page. Check whether to read from the buffer or from the local shadow copy. */
            const void* buf_or_shadow = (entry->data_present) ? &entry->data : SHARED_TO_SHADOW_POINTER(in, in_address);

            if (out)
            {
              /* We're reading/writing to and from a SHM page. Write to the local shadow copy using a memcpy or memmove, depending
               * on whether we can relax any requested MEMMOVEs (if we **know** source and destination won't overlap).
               */
              if (type == MEMMOVE && !entry->data_present)
                orig_memmove(SHARED_TO_SHADOW_POINTER(out, out_address), buf_or_shadow, size);
              else
                orig_memcpy(SHARED_TO_SHADOW_POINTER(out, out_address), buf_or_shadow, size);
            }
            else
            {
              /* The input comes from a SHM page, the output goes to a non-SHM page. As we know **for a fact** that destination buffer does
               * not overlap with the input buffer (either in the SHM buffer, or the local shadow page), we use a memcpy, even for MEMMOVE.
               */
              orig_memcpy(out_address, buf_or_shadow, size);
            }
          }
          else
          {
            /* The input comes from a non-SHM page, check its correctness */
            mvee_assert_same_store(&entry->data, in_address, size);

            /* Write local shadow copy */
            orig_memcpy(SHARED_TO_SHADOW_POINTER(out, out_address), in_address, size);
          }
          break;
        }
      case MEMSET:
        {
          mvee_assert_same_value(entry->value, value);

          /* Write local shadow copy */
          orig_memset(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
          break;
        }
      case MEMCHR:
        {
          mvee_assert_same_value(entry->value, value);

          /* Something changed in the leader, leading to a different return value. Get it from buffer */
          if (entry->data_present)
            *(ptrdiff_t*)out_address = (ptrdiff_t)entry->second_address;
          else
          {
            /* Search on local shadow copy */
            void* local_ret = orig_memchr(SHARED_TO_SHADOW_POINTER(in, in_address), value, size);

            /* Write offset as return value */
            *(ptrdiff_t*)out_address = local_ret - in_address;
          }

          break;
        }
      case ATOMICCMPXCHG:
        {
          mvee_assert_same_value(entry->value, value);
          mvee_assert_same_value(entry->cmp, cmp);
          if (entry->data_present)
          {
             ret = entry->data[0];
             if (ret)
               __atomic_exchange_n((uint32_t*)SHARED_TO_SHADOW_POINTER(in, in_address), value, __ATOMIC_RELAXED);
          }
          else
            ret = __atomic_compare_exchange_n((uint32_t*)SHARED_TO_SHADOW_POINTER(in, in_address), &cmp, value, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
          break;
        }
      case ATOMICRMW_XCHG:
        {
          ATOMICRMW_FOLLOWER(__atomic_exchange_n);
          break;
        }
      case ATOMICRMW_ADD:
        {
          ATOMICRMW_FOLLOWER(__atomic_fetch_add);
          break;
        }
      case ATOMICRMW_SUB:
        {
          ATOMICRMW_FOLLOWER(__atomic_fetch_sub);
          break;
        }
      case ATOMICRMW_AND:
        {
          ATOMICRMW_FOLLOWER(__atomic_fetch_and);
          break;
        }
      case ATOMICRMW_NAND:
        {
          ATOMICRMW_FOLLOWER(__atomic_fetch_nand);
          break;
        }
      case ATOMICRMW_OR:
        {
          ATOMICRMW_FOLLOWER(__atomic_fetch_or);
          break;
        }
      case ATOMICRMW_XOR:
        {
          ATOMICRMW_FOLLOWER(__atomic_fetch_xor);
          break;
        }
      default:
        mvee_error_unsupported_operation(type);
    }
  }

#ifdef MVEE_LOG_SHM_MEM_OPS
  if (GLIBC_FUNC_BASE <= type)
    syscall(MVEE_LOG_SHM_MEM_OP, type, size);
#endif

  return ret;
}

// ========================================================================================================================
// The mvee_shm_op interface used by the wrapping shm_support compiler pass
// ========================================================================================================================
typedef struct mvee_shm_op_ret {
  unsigned long val;
  bool cmp;
} mvee_shm_op_ret;

mvee_shm_op_ret mvee_shm_op(unsigned char id, bool atomic, void* address, unsigned long size, unsigned long value, unsigned long cmp)
{
  mvee_shm_op_ret ret = { 0 };

  // Use SHM buffer
  address = mvee_shm_decode_address(address);
  mvee_shm_table_entry* entry = mvee_shm_table_get_entry(address);
  if (!entry)
    mvee_error_shm_entry_not_present(address);

  switch (id)
  {
    case LOAD:
      mvee_shm_buffered_op(id, address, entry, &ret.val, NULL, size, 0, 0);
      break;
    case STORE:
      mvee_shm_buffered_op(id, &value, NULL, address, entry, size, 0, 0);
      break;
    case ATOMICCMPXCHG:
      if (size != 4)
        mvee_error_unsupported_operation(id);
      ret.cmp = mvee_shm_buffered_op(id, address, entry, NULL, NULL, size, value, cmp);
      break;
    case ATOMICRMW_XCHG:
    case ATOMICRMW_ADD:
    case ATOMICRMW_SUB:
    case ATOMICRMW_AND:
    case ATOMICRMW_NAND:
    case ATOMICRMW_OR:
    case ATOMICRMW_XOR:
      if (size != 4)
        mvee_error_unsupported_operation(id);
      mvee_shm_buffered_op(id, address, entry, &ret.val, NULL, size, value, 0);
      break;
      // We don't support these yet, haven't encountered them
    case ATOMICRMW_MAX:
    case ATOMICRMW_MIN:
    case ATOMICRMW_UMAX:
    case ATOMICRMW_UMIN:
    case ATOMICRMW_FADD:
    case ATOMICRMW_FSUB:
    default:
      mvee_error_unsupported_operation(id);
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
  void* shm_dest = mvee_shm_decode_address(dest);
  void* shm_src = mvee_shm_decode_address(src);
  mvee_shm_table_entry* dest_entry = mvee_shm_table_get_entry(shm_dest);
  mvee_shm_table_entry* src_entry = mvee_shm_table_get_entry(shm_src);
  if (!dest_entry && !src_entry)
    mvee_error_shm_entry_not_present(dest);

  mvee_shm_buffered_op(MEMCPY, src_entry ? shm_src : src, src_entry, dest_entry ? shm_dest : dest, dest_entry, n, 0, 0);

  return dest;
}

void *
mvee_shm_memmove (void *dest, const void * src, size_t n)
{
  /* Decode addresses */
  void* shm_dest = mvee_shm_decode_address(dest);
  void* shm_src = mvee_shm_decode_address(src);
  mvee_shm_table_entry* dest_entry = mvee_shm_table_get_entry(shm_dest);
  mvee_shm_table_entry* src_entry = mvee_shm_table_get_entry(shm_src);
  if (!dest_entry && !src_entry)
    mvee_error_shm_entry_not_present(dest);

  mvee_shm_buffered_op(MEMMOVE, src_entry ? shm_src : src, src_entry, dest_entry ? shm_dest : dest, dest_entry, n, 0, 0);

  return dest;
}

void *
mvee_shm_memset (void *dest, int ch, size_t len)
{
  /* Decode addresses */
  void* shm_dest = mvee_shm_decode_address(dest);
  mvee_shm_table_entry* entry = mvee_shm_table_get_entry(shm_dest);
  if (!entry)
    mvee_error_shm_entry_not_present(dest);

  mvee_shm_buffered_op(MEMSET, NULL, NULL, shm_dest, entry, len, ch, 0);
  return dest;
}

void *
mvee_shm_memchr (void const *src, int c_in, size_t n)
{
  /* Decode addresses */
  void* shm_src = mvee_shm_decode_address(src);
  mvee_shm_table_entry* entry = mvee_shm_table_get_entry(shm_src);
  if (!entry)
    mvee_error_shm_entry_not_present(src);

  ptrdiff_t ret = 0;
  mvee_shm_buffered_op(MEMCHR, shm_src, entry, &ret, NULL, n, c_in, 0);
  return (void*)src + ret;
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
    if (!((fd_stat.st_mode & S_IRUSR) && (fd_stat.st_mode & S_IWUSR)))
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

    // Do bookkeeping
    mvee_shm_table_add_entry(mvee_shm_decode_address((void *) ret), shadow, len);
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

  struct shmid_ds shm_info;
  // just kinda assuming this won't fail at this point, should have succeeded if this point is reached...
  shmctl(shmid, IPC_STAT, &shm_info);
  mvee_shm_table_add_entry(mvee_shm_decode_address(mapping), shadow, shm_info.shm_segsz);

  return mapping;
}

int
mvee_shm_shmdt (const void *shmaddr)
{
  if ((unsigned long long) shmaddr & 0x8000000000000000ull)
  {
    mvee_shm_table_entry* mapping = mvee_shm_table_get_entry(mvee_shm_decode_address(shmaddr));
    if (!mapping)
      mvee_error_shm_entry_not_present(shmaddr);

    orig_SHMDT_CALL(mapping->shadow);
    mvee_shm_table_delete_entry(mapping);
  }
  return orig_SHMDT_CALL(shmaddr);
}

int
mvee_shm_munmap (const void *addr, size_t len)
{
  if ((unsigned long long) addr & 0x8000000000000000ull)
  {
    mvee_shm_table_entry* mapping = mvee_shm_table_get_entry(mvee_shm_decode_address(addr));
    if (!mapping)
      mvee_error_shm_entry_not_present(addr);

    // I'm too lazy to do the parial munmap stuff, so just adding this to maybe have an idea when it does happen
    mvee_assert_equal_mapping_size(len, mapping->len);

    orig_SHMDT_CALL(mapping->shadow);
    mvee_shm_table_delete_entry(mapping);
  }
  return orig_MUNMAP_CALL(addr, len);
}
