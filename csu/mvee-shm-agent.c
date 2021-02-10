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
extern __typeof (memcmp)  orig_memcmp;
extern __typeof (strlen)  orig_strlen;
extern __typeof (strcmp)  orig_strcmp;

// ========================================================================================================================
// Types of SHM operations
// ========================================================================================================================
enum
{
  LOAD            = 0,// Instruction::LoadInst
  STORE           = 1,// Instruction::StoreInst
  ATOMICLOAD      = 2,// Instruction::LoadInst, but atomic
  ATOMICSTORE     = 3,// Instruction::StoreInst, but atomic
  ATOMICCMPXCHG   = 4,// Instruction::AtomicCmpXchgInst
  ATOMICRMW_FIRST = 5,// Instruction::AtomicRMWInst
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
  MEMCMP          = GLIBC_FUNC_BASE + 4,
  STRLEN          = GLIBC_FUNC_BASE + 5,
  STRCMP          = GLIBC_FUNC_BASE + 5,
};

#define ATOMICLOAD_BY_SIZE(out_address, in_address, size)                                                          \
  do {                                                                                                             \
    if (size == 1)                                                                                                 \
      __atomic_load((uint8_t*)in_address, (uint8_t*)out_address, __ATOMIC_ACQUIRE);                                \
    else if (size == 2)                                                                                            \
      __atomic_load((uint16_t*)in_address, (uint16_t*)out_address, __ATOMIC_ACQUIRE);                              \
    else if (size == 4)                                                                                            \
      __atomic_load((uint32_t*)in_address, (uint32_t*)out_address, __ATOMIC_ACQUIRE);                              \
    else if (size == 8)                                                                                            \
      __atomic_load((uint64_t*)in_address, (uint64_t*)out_address, __ATOMIC_ACQUIRE);                              \
  } while(0)

#define ATOMICSTORE_BY_SIZE(out_address, val, size)                                                                \
  do {                                                                                                             \
    if (size == 1)                                                                                                 \
      __atomic_store_n((uint8_t*)out_address, (uint8_t)val, __ATOMIC_RELEASE);                                     \
    else if (size == 2)                                                                                            \
      __atomic_store_n((uint16_t*)out_address, (uint16_t)val, __ATOMIC_RELEASE);                                   \
    else if (size == 4)                                                                                            \
      __atomic_store_n((uint32_t*)out_address, (uint32_t)val, __ATOMIC_RELEASE);                                   \
    else if (size == 8)                                                                                            \
      __atomic_store_n((uint64_t*)out_address, (uint64_t)val, __ATOMIC_RELEASE);                                   \
  } while(0)

#define ATOMICCMPXCHG_BY_SIZE(addr, cmp, value, size)                                                              \
  ({ bool __ret = false;                                                                                           \
if (size == 1)                                                                                                     \
  __ret = __atomic_compare_exchange_n((uint8_t*)addr, (uint8_t*)cmp, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);\
else if (size == 2)                                                                                                \
  __ret = __atomic_compare_exchange_n((uint16_t*)addr, (uint16_t*)cmp, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);\
else if (size == 4)                                                                                                \
  __ret = __atomic_compare_exchange_n((uint32_t*)addr, (uint32_t*)cmp, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);\
else if (size == 8)                                                                                                \
  __ret = __atomic_compare_exchange_n((uint64_t*)addr, (uint64_t*)cmp, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);\
  __ret;})

#define ATOMICRMW_LEADER(operation, utype)                                                                         \
  do {                                                                                                             \
    utype __shm_val = operation((utype*)in_address, value, __ATOMIC_SEQ_CST);                                      \
    utype __shadow_val = operation((utype*)SHARED_TO_SHADOW_POINTER(in, in_address), value, __ATOMIC_SEQ_CST);     \
    *((utype*)out_address) = __shm_val;                                                                            \
    entry->data_present = (__shm_val != __shadow_val);                                                             \
    if (entry->data_present)                                                                                       \
      orig_memcpy(&entry->data, &__shm_val, sizeof(utype));                                                        \
  } while(0)

#define ATOMICRMW_LEADER_BY_SIZE(operation, size)                                                                  \
entry->value = value;                                                                                              \
if (size == 1)                                                                                                     \
  ATOMICRMW_LEADER(operation, uint8_t);                                                                            \
else if (size == 2)                                                                                                \
  ATOMICRMW_LEADER(operation, uint16_t);                                                                           \
else if (size == 4)                                                                                                \
  ATOMICRMW_LEADER(operation, uint32_t);                                                                           \
else if (size == 8)                                                                                                \
  ATOMICRMW_LEADER(operation, uint64_t);

#define ATOMICRMW_FOLLOWER(operation, utype)                                                                       \
  do {                                                                                                             \
    if (entry->data_present)                                                                                       \
      orig_memcpy(SHARED_TO_SHADOW_POINTER(in, in_address), &entry->data, sizeof(utype));                          \
    *((utype*)out_address) = operation((utype*)SHARED_TO_SHADOW_POINTER(in, in_address), value, __ATOMIC_SEQ_CST); \
  } while(0)

#define ATOMICRMW_FOLLOWER_BY_SIZE(operation, size)                                                                \
mvee_assert_same_value(entry->value, value);                                                                       \
if (size == 1)                                                                                                     \
  ATOMICRMW_FOLLOWER(operation, uint8_t);                                                                          \
else if (size == 2)                                                                                                \
  ATOMICRMW_FOLLOWER(operation, uint16_t);                                                                         \
else if (size == 4)                                                                                                \
  ATOMICRMW_FOLLOWER(operation, uint32_t);                                                                         \
else if (size == 8)                                                                                                \
  ATOMICRMW_FOLLOWER(operation, uint64_t);

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
    syscall(__NR_gettid, 1337, 10000001, 101, a, b);
}

__attribute__((noinline))
static void mvee_assert_same_size(unsigned long a, unsigned long b)
{
  if (a != b)
    syscall(__NR_gettid, 1337, 10000001, 102, a, b);
}

__attribute__((noinline))
static void mvee_assert_same_store(const void* a, const void* b, unsigned long size)
{
  if (orig_memcmp(a, b, size))
    ;// Do nothing!
    //syscall(__NR_gettid, 1337, 10000001, 103, a, b, size);
}

__attribute__((noinline))
static void mvee_assert_same_type(unsigned char a, unsigned char b)
{
  if (a != b)
    syscall(__NR_gettid, 1337, 10000001, 104, a, b);
}

__attribute__((noinline))
static void mvee_assert_same_value(int a, int b)
{
  if (a != b)
    syscall(__NR_gettid, 1337, 10000001, 105, a, b);
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
  uint64_t value;
  uint64_t cmp;
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
    mvee_shm_buffer = (char*)syscall(__NR_shmat, syscall(MVEE_GET_SHARED_BUFFER, 0, MVEE_SHM_BUFFER, &mvee_shm_buffer_size, 1, 0, &mvee_shm_buffer), NULL, 0);

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
static inline bool mvee_shm_buffered_op(unsigned char type, const void* in_address, const mvee_shm_table_entry* in, void* out_address, const mvee_shm_table_entry* out, size_t size, uint64_t value, uint64_t cmp)
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
              entry->data_present = orig_memcmp(SHARED_TO_SHADOW_POINTER(in, in_address), in_address, size);
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
      case ATOMICLOAD:
        {
          /* Load from actual SHM page */
          ATOMICLOAD_BY_SIZE(out_address, in_address, size);

          /* Load from local shadow copy */
          char local_ret[8];
          ATOMICLOAD_BY_SIZE(&local_ret, SHARED_TO_SHADOW_POINTER(in, in_address), size);

          /* If these two loads differ, put the load from the SHM page in the buffer */
          entry->data_present = orig_memcmp(out_address, &local_ret, size);
          if (entry->data_present)
            orig_memcpy(&entry->data, out_address, size);

          break;
        }
      case ATOMICSTORE:
        {
          entry->value = value;

          /* Store on actual SHM page */
          ATOMICSTORE_BY_SIZE(out_address, value, size);

          /* Store on local shadow copy */
          ATOMICSTORE_BY_SIZE(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
          break;
        }
      case ATOMICCMPXCHG:
        {
          entry->value = value;
          entry->cmp = cmp;

          ret = ATOMICCMPXCHG_BY_SIZE(in_address, &cmp, value, size);
          bool shadow_cmp = ATOMICCMPXCHG_BY_SIZE(SHARED_TO_SHADOW_POINTER(in, in_address), &cmp, value, size);
          entry->data_present = (ret != shadow_cmp);
          if (entry->data_present)
             entry->data[0] = ret;
          break;
        }
      case ATOMICRMW_XCHG:
        {
          ATOMICRMW_LEADER_BY_SIZE (__atomic_exchange_n, size);
          break;
        }
      case ATOMICRMW_ADD:
        {
          ATOMICRMW_LEADER_BY_SIZE(__atomic_fetch_add, size);
          break;
        }
      case ATOMICRMW_SUB:
        {
          ATOMICRMW_LEADER_BY_SIZE(__atomic_fetch_sub, size);
          break;
        }
      case ATOMICRMW_AND:
        {
          ATOMICRMW_LEADER_BY_SIZE(__atomic_fetch_and, size);
          break;
        }
      case ATOMICRMW_NAND:
        {
          ATOMICRMW_LEADER_BY_SIZE(__atomic_fetch_nand, size);
          break;
        }
      case ATOMICRMW_OR:
        {
          ATOMICRMW_LEADER_BY_SIZE(__atomic_fetch_or, size);
          break;
        }
      case ATOMICRMW_XOR:
        {
          ATOMICRMW_LEADER_BY_SIZE(__atomic_fetch_xor, size);
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
      case ATOMICLOAD:
        {
          /* If data present, copy from the buffer. Otherwise, load from local shadow copy */
          if (entry->data_present)
            orig_memcpy(out_address, &entry->data, size);
          else
            ATOMICLOAD_BY_SIZE(out_address, SHARED_TO_SHADOW_POINTER(in, in_address), size);
          break;
        }
      case ATOMICSTORE:
        {
          mvee_assert_same_value(entry->value, value);

          /* Store on local shadow copy */
          ATOMICSTORE_BY_SIZE(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
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
               ATOMICSTORE_BY_SIZE(SHARED_TO_SHADOW_POINTER(in, in_address), value, size);
          }
          else
            ret = ATOMICCMPXCHG_BY_SIZE(SHARED_TO_SHADOW_POINTER(in, in_address), &cmp, value, size);
          break;
        }
      case ATOMICRMW_XCHG:
        {
          ATOMICRMW_FOLLOWER_BY_SIZE(__atomic_exchange_n, size);
          break;
        }
      case ATOMICRMW_ADD:
        {
          ATOMICRMW_FOLLOWER_BY_SIZE(__atomic_fetch_add, size);
          break;
        }
      case ATOMICRMW_SUB:
        {
          ATOMICRMW_FOLLOWER_BY_SIZE(__atomic_fetch_sub, size);
          break;
        }
      case ATOMICRMW_AND:
        {
          ATOMICRMW_FOLLOWER_BY_SIZE(__atomic_fetch_and, size);
          break;
        }
      case ATOMICRMW_NAND:
        {
          ATOMICRMW_FOLLOWER_BY_SIZE(__atomic_fetch_nand, size);
          break;
        }
      case ATOMICRMW_OR:
        {
          ATOMICRMW_FOLLOWER_BY_SIZE(__atomic_fetch_or, size);
          break;
        }
      case ATOMICRMW_XOR:
        {
          ATOMICRMW_FOLLOWER_BY_SIZE(__atomic_fetch_xor, size);
          break;
        }
      default:
        mvee_error_unsupported_operation(type);
    }
  }

#ifdef MVEE_LOG_SHM_OPS
  syscall(__NR_gettid, 1337, 10000001, 100, type, size);
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

mvee_shm_op_ret mvee_shm_op(unsigned char id, void* address, unsigned long size, unsigned long value, unsigned long cmp)
{
  mvee_shm_op_ret ret = { 0 };

  // Use SHM buffer
  address = mvee_shm_decode_address(address);
  mvee_shm_table_entry* entry = mvee_shm_table_get_entry(address);
  if (!entry)
    mvee_error_shm_entry_not_present(address);

  switch (id)
  {
    case ATOMICLOAD:
    case LOAD:
      mvee_shm_buffered_op(id, address, entry, &ret.val, NULL, size, 0, 0);
      break;
    case ATOMICSTORE:
      mvee_shm_buffered_op(id, NULL, NULL, address, entry, size, value, 0);
      break;
    case STORE:
      mvee_shm_buffered_op(id, &value, NULL, address, entry, size, 0, 0);
      break;
    case ATOMICCMPXCHG:
      ret.cmp = mvee_shm_buffered_op(id, address, entry, NULL, NULL, size, value, cmp);
      break;
    case ATOMICRMW_XCHG:
    case ATOMICRMW_ADD:
    case ATOMICRMW_SUB:
    case ATOMICRMW_AND:
    case ATOMICRMW_NAND:
    case ATOMICRMW_OR:
    case ATOMICRMW_XOR:
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

int
mvee_shm_memcmp (const void *s1, const void *s2, size_t len)
{
  // The pointers we're actually gonna be using.
  void* shm_s1 = (void*)s1;
  void* shm_s2 = (void*)s2;

  // Entries in the shared memory bookkeeping.
  mvee_shm_table_entry* s1_entry = NULL;
  mvee_shm_table_entry* s2_entry = NULL;

  // If first pointer is shared memory pointer decode it and get the entry for it.
  if ((unsigned long)s1 & 0x8000000000000000ull)
  {
    shm_s1 = mvee_shm_decode_address(s1);
    s1_entry = mvee_shm_table_get_entry(shm_s1);
    // If we're here, it should exist...
    if (!s1_entry)
      mvee_error_shm_entry_not_present(s1);
  }
  // If second pointer is shared memory pointer decode it and get the entry for it.
  if ((unsigned long)s2 & 0x8000000000000000ull)
  {
    shm_s2 = mvee_shm_decode_address(s2);
    s2_entry = mvee_shm_table_get_entry(shm_s2);
    // If we're here, it should exist...
    if (!s2_entry)
      mvee_error_shm_entry_not_present(s2);
  }


  // Get an entry in the replication buffer.
  // If both addresses are shared memory pointer, we will need a buffer of double length.
  mvee_shm_op_entry* entry = mvee_shm_get_entry((s1_entry && s2_entry) ? (len * 2) : len);
  if (likely(mvee_master_variant))
  {
      // Fill in the entry.
    entry->address = s1_entry ? shm_s1 : shm_s2;
    entry->second_address = (s1_entry && s2_entry) ? shm_s2 : NULL;
    entry->type = MEMCMP;
    entry->cmp = 0;

    // When the first pointer is a shared memory pointer, check if the shared memory contents are still the
    // same as in the shadow mapping, update the replication entry accordingly.
    if (s1_entry)
    {
      // Copy the shared memory contents to the buffer and replace the pointer we'll be using from now on.
      // This ensures that our view of shared memory does not change over the course of this function.
      void* temp = entry->data;
      orig_memcpy(temp, shm_s1, len);

      // Update entry if the contents of shared memory differ with the shadow
      if (orig_memcmp(temp, SHARED_TO_SHADOW_POINTER(s1_entry, shm_s1), len))
        entry->cmp |= 1;
      shm_s1 = temp;
    }
    // same as for first pointer
    if (s2_entry)
    {
      // Copy the shared memory contents to the buffer and replace the pointer we'll be using from now on.
      // This ensures that our view of shared memory does not change over the course of this function.
      void* temp = entry->data + ((s1_entry && s2_entry) ? len : 0);
      orig_memcpy(temp, shm_s2, len);

      // Update entry if the contents of shared memory differ with the shadow
      if (orig_memcmp(temp, SHARED_TO_SHADOW_POINTER(s2_entry, shm_s2), len))
        entry->cmp |= 2;
      shm_s2 = temp;
    }

    // save the return value for the memcmp.
    entry->value = orig_memcmp(shm_s1, shm_s2, len);
    orig_atomic_store_release(&entry->size, len);
  }
  else
  {
    // Wait until entry is ready
    while (!orig_atomic_load_acquire(&entry->size))
      syscall(__NR_sched_yield);

    // memcmp should be called on the same relative pointers if they are shared memory pointers
    mvee_assert_same_address(entry->address, (s1_entry ? shm_s1 : shm_s2));
    mvee_assert_same_address(entry->second_address, ((s1_entry && s2_entry) ? shm_s2 : NULL));
    // memcmp should be called with the same size
    mvee_assert_same_size(entry->size, len);
    // type check
    mvee_assert_same_type(entry->type, MEMCMP);

    if (s1_entry)
    {
      if (entry->cmp & 1)
        shm_s1 = entry->data;
      else
        shm_s1 = SHARED_TO_SHADOW_POINTER(s1_entry, shm_s1);
    }
    if (s2_entry)
    {
      if (entry->cmp & 2)
        shm_s2 = entry->data + (s1_entry ? len : 0);
      else
        shm_s2 = SHARED_TO_SHADOW_POINTER(s2_entry, shm_s2);
    }

    // perform the memcmp
    int return_value = orig_memcmp(shm_s1, shm_s2, len);

    // the return value for memcmp should be the same
    mvee_assert_same_value(entry->value, return_value);
  }

  return entry->value;
}

int
mvee_shm_strcmp (const char *str1, const char *str2)
{
  // The pointers we're actually gonna be using.
  void* shm_str1 = (void*)str1;
  void* shm_str2 = (void*)str2;

  // Entries in the shared memory bookkeeping.
  mvee_shm_table_entry* str1_entry = NULL;
  mvee_shm_table_entry* str2_entry = NULL;

  // If first pointer is shared memory pointer decode it and get the entry for it.
  if ((unsigned long)str1 & 0x8000000000000000ull)
  {
    shm_str1 = mvee_shm_decode_address(str1);
    str1_entry = mvee_shm_table_get_entry(shm_str1);
    // If we're here, it should exist...
    if (!str1_entry)
      mvee_error_shm_entry_not_present(str1);
  }
  // If second pointer is shared memory pointer decode it and get the entry for it.
  if ((unsigned long)str2 & 0x8000000000000000ull)
  {
    shm_str2 = mvee_shm_decode_address(str2);
    str2_entry = mvee_shm_table_get_entry(shm_str2);
    // If we're here, it should exist...
    if (!str2_entry)
      mvee_error_shm_entry_not_present(str2);
  }


  // Get an entry in the replication buffer.
  // If both addresses are shared memory pointer, we will need a buffer of double length.
  mvee_shm_op_entry* entry = mvee_shm_get_entry(sizeof(int));
  if (likely(mvee_master_variant))
  {
      // Fill in the entry.
    entry->address = str1_entry ? shm_str1 : shm_str2;
    entry->second_address = (str1_entry && str2_entry) ? shm_str2 : NULL;
    entry->type = STRCMP;
    entry->cmp = 0;

    // save the return value for the memcmp.
    *(int*)entry->data = orig_strcmp(shm_str1, shm_str2);
    orig_atomic_store_release(&entry->size, sizeof(int));
  }
  else
  {
    // Wait until entry is ready
    while (!orig_atomic_load_acquire(&entry->size))
      syscall(__NR_sched_yield);

    // memcmp should be called on the same relative pointers if they are shared memory pointers
    mvee_assert_same_address(entry->address, (str1_entry ? shm_str1 : shm_str2));
    mvee_assert_same_address(entry->second_address, ((str1_entry && str2_entry) ? shm_str2 : NULL));
    // type check
    mvee_assert_same_type(entry->type, STRCMP);
  }

  return entry->value;
}

size_t
mvee_shm_strlen (const char *str)
{
  const char *shm_str = mvee_shm_decode_address(str);
  mvee_shm_table_entry* shm_entry = mvee_shm_table_get_entry(shm_str);
  if (!shm_entry)
    mvee_error_shm_entry_not_present(str);

  // We're allocating sizeof(size_t) data since entry->value is only uint32_t.
  mvee_shm_op_entry* entry = mvee_shm_get_entry(sizeof(size_t));
  if (likely(mvee_master_variant))
  {
    entry->address = shm_str;
    entry->type    = STRLEN;


    // There isn't much point to any complicated shadow mapping stuff here.
    // If the result for the shared and shadow mapping is the same, we could use either one. If it's different, we'd
    // have to use the result from the shared mapping... So, we might as well just use the result from the shared
    // mapping from the start. Maybe we could force the shadow results in both variants to be the same?
    *(size_t*)entry->data = orig_strlen(shm_str);
    orig_atomic_store_release(&entry->size, sizeof(size_t));
  }
  else
  {
    // Wait until entry is ready
    while (!orig_atomic_load_acquire(&entry->size))
      syscall(__NR_sched_yield);

    mvee_assert_same_type(entry->type, STRLEN);
    mvee_assert_same_address(entry->address, shm_str);
  }

  return *(size_t*)entry->data;
}

// ========================================================================================================================
// Hooks for mmap and related functions
// ========================================================================================================================
void *
mvee_shm_mmap (void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
  unsigned long ret = orig_MMAP_CALL(addr, len, prot, flags, fd, offset);
  if ((flags & MAP_SHARED) && !(prot & PROT_EXEC) && fd && fd != -1 && (void *) ret != MAP_FAILED)
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
