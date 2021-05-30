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
  STRCMP          = GLIBC_FUNC_BASE + 6,
};

#define LOAD_BY_SIZE(out_address, in_address, size)                                                                \
  do {                                                                                                             \
    if (size == 1)                                                                                                 \
      *(uint8_t*)out_address = *(uint8_t*)in_address;                                                              \
    else if (size == 2)                                                                                            \
      *(uint16_t*)out_address = *(uint16_t*)in_address;                                                            \
    else if (size == 4)                                                                                            \
      *(uint32_t*)out_address = *(uint32_t*)in_address;                                                            \
    else if (size == 8)                                                                                            \
      *(uint64_t*)out_address = *(uint64_t*)in_address;                                                            \
  } while(0)

#define STORE_BY_SIZE(out_address, val, size)                                                                      \
  do {                                                                                                             \
    if (size == 1)                                                                                                 \
      *(uint8_t*)out_address = (uint8_t)val;                                                                       \
    else if (size == 2)                                                                                            \
      *(uint16_t*)out_address = (uint16_t)val;                                                                     \
    else if (size == 4)                                                                                            \
      *(uint32_t*)out_address = (uint32_t)val;                                                                     \
    else if (size == 8)                                                                                            \
      *(uint64_t*)out_address = (uint64_t)val;                                                                     \
  } while(0)

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
    /* This returns the original value at the address */                                                           \
    utype __shm_val = operation((utype*)out_address, value, __ATOMIC_SEQ_CST);                                     \
    ret.val = __shm_val;                                                                                           \
    /* Compare with shadow memory: do the original values differ? */                                               \
    if (out->shadow)                                                                                               \
    {                                                                                                              \
      utype __shadow_val = operation((utype*)SHARED_TO_SHADOW_POINTER(out, out_address), value, __ATOMIC_SEQ_CST); \
      data_in_buffer = (__shm_val != __shadow_val);                                                                \
    }                                                                                                              \
    /* If no shadow memory, always use buffer */                                                                   \
    else                                                                                                           \
      data_in_buffer = true;                                                                                       \
    /* Put the original value in the buffer */                                                                     \
    if (data_in_buffer)                                                                                            \
      orig_memcpy(&entry->data, &__shm_val, sizeof(utype));                                                        \
  } while(0)

#define ATOMICRMW_LEADER_BY_SIZE(operation, size)                                                                  \
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
    if (out->shadow)                                                                                               \
    {                                                                                                              \
      /* If the original value differed in the leader, update it before doing the operation */                     \
      if (data_in_buffer)                                                                                          \
        orig_memcpy(SHARED_TO_SHADOW_POINTER(out, out_address), &entry->data, sizeof(utype));                      \
      ret.val = operation((utype*)SHARED_TO_SHADOW_POINTER(out, out_address), value, __ATOMIC_SEQ_CST);            \
    }                                                                                                              \
    else                                                                                                           \
      ret.val = *(utype*)&entry->data;                                                                             \
  } while(0)

#define ATOMICRMW_FOLLOWER_BY_SIZE(operation, size)                                                                \
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

void* mvee_shm_decode_address(const volatile void* address)
{
  unsigned long high = (unsigned long)address & 0xffffffff00000000ull;
  unsigned long low  = (unsigned long)address & 0x00000000ffffffffull;
  return (void*) ((high ^ mvee_shm_tag) + low);
}

static void* mvee_shm_decode_address_leader(const volatile void* address)
{
  static unsigned long leader_tag = 0;
  if (!leader_tag)
    leader_tag = syscall(MVEE_GET_LEADER_SHM_TAG);
  unsigned long high = (unsigned long)address & 0xffffffff00000000ull;
  unsigned long low  = (unsigned long)address & 0x00000000ffffffffull;
  return (void*) ((high ^ leader_tag) + low);
}

// ========================================================================================================================
// Assertions and errors
// ========================================================================================================================
static inline bool mvee_are_pointers_equivalent(const void* a, const void* b)
{
  /* Decode both potential pointers */
  void* pa_dec = mvee_shm_decode_address_leader(a);
  void* pb_dec = mvee_shm_decode_address(b);

  return pa_dec == pb_dec;
}

__attribute__((noinline))
static void mvee_assert_equal_mapping_size(size_t len, size_t size)
{
  if (unlikely(len != size))
    *(volatile long*)0 = len - size;
}

static inline void mvee_assert_same_address(const void* a, const void* b)
{
  if (unlikely(a != b))
    syscall(__NR_gettid, 1337, 10000001, 101, a, b);
}

static inline void mvee_assert_same_size(size_t a, size_t b)
{
  if (unlikely(a != b))
    syscall(__NR_gettid, 1337, 10000001, 102, a, b);
}

static void mvee_assert_same_store(const void* a, const void* b, const unsigned long size, bool might_contain_pointers)
{
  /* Check if the buffers are the same. If they are, no issue! */
  if (!orig_memcmp(a, b, size))
    return;

  /* Check if the buffers are equivalent. We implemented this check based on three assumptions:
   * 1. The only type of data that can differ yet still be equivalent is pointers.
   * 2. Pointers can only be stored in an aligned manner (this is not correct!).
   * 3. Pointers are stored using single stores, not memcpy. The overhead of comparing the
   * complete buffers first, and doing an element-wise comparison of the entire buffer after
   * is thus negligible.
   */
  if (might_contain_pointers)
  {
    /* If there is **any** difference in the entire buffer. Start looking for the difference, byte per byte */
    size_t offset;
    for (offset = 0; (offset + sizeof(void*)) <= size; offset += sizeof(void*))
    {
      /* Read the data that constitutes the potential pointers */
      void* pa = *((void**)(a + offset));
      void* pb = *((void**)(b + offset));

      /* No difference, move on */
      if (pa == pb)
        continue;

      /* If the decoded pointers differ, they're actually differing data. Inform the monitor. */
      if (!mvee_are_pointers_equivalent(pa, pb))
        syscall(__NR_gettid, 1337, 10000001, 103, a, b, size);
    }
  }
  /* The buffers **have** to be the same, so this is a divergence. Inform the monitor. */
  else
    syscall(__NR_gettid, 1337, 10000001, 103, a, b, size);
}

static inline void mvee_assert_same_type(unsigned char a, unsigned char b)
{
  if (unlikely(a != b))
    syscall(__NR_gettid, 1337, 10000001, 104, a, b);
}

/* A version where the compared values are int */
static inline void mvee_assert_same_value1(int a, int b)
{
  if (unlikely(a != b))
    syscall(__NR_gettid, 1337, 10000001, 105, a, b);
}

/* A version where the compared values are pointer-sized, and possible pointers */
static inline void mvee_assert_same_value2(uint64_t a, uint64_t b, bool might_contain_pointers)
{
  if (a == b)
    return;

  if (might_contain_pointers)
  {
    if (unlikely(!mvee_are_pointers_equivalent((void*)a, (void*)b)))
      syscall(__NR_gettid, 1337, 10000001, 105, a, b);
  }
  /* The values **have** to be the same, so this is a divergence. Inform the monitor. */
  else
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

__libc_lock_define_initialized (static, mvee_shm_table_lock)

void mvee_shm_table_add_entry(void* address, void* shadow, size_t len)
{
  __libc_lock_lock(mvee_shm_table_lock);

  /* Prepare entry */
  mvee_shm_table_entry *entry = (mvee_shm_table_entry *) malloc(sizeof(mvee_shm_table_entry));
  entry->address = address;
  entry->shadow = shadow;
  entry->len = len;
  entry->prev = NULL;
  entry->next = NULL;

  /* Insert entry, or make it the new head if none exists yet */
  mvee_shm_table_entry *iterator = mvee_shm_table_head;
  if (iterator)
  {
    /* Insert in a sorted order, so make our entry the new head if necessary */
    if ((address + len) <= iterator->address)
    {
      entry->next = iterator;
      iterator->prev = entry;
      orig_atomic_store_release(&mvee_shm_table_head, entry);
    }
    else
    {
      /* Insert half-way? */
      while (iterator->next)
      {
        if (((iterator->address + iterator->len) <= address) && ((address + len) <= iterator->next->address))
        {
          entry->next = iterator->next;
          entry->prev = iterator;
          iterator->next->prev = entry;
          orig_atomic_store_release(&iterator->next, entry);
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

        entry->prev = iterator;
        orig_atomic_store_release(&iterator->next, entry);
      }
    }
  }
  else
    orig_atomic_store_release(&mvee_shm_table_head, entry);

  __libc_lock_unlock(mvee_shm_table_lock);
}

/* There's no need for locks in this function, the store-release semantics used
 * by the functions updating this table means a load-acquire should be enough
 * here.
 */
static mvee_shm_table_entry* mvee_shm_table_get_entry(const void* address)
{
  mvee_shm_table_entry* entry = orig_atomic_load_acquire(&mvee_shm_table_head);
  while (entry)
  {
    /* If we found the entry, quit */
    if (((uintptr_t)entry->address <= (uintptr_t)address) && ((uintptr_t)address < ((uintptr_t)entry->address + entry->len)))
      break;

    entry = orig_atomic_load_acquire(&entry->next);
  }

  return entry;
}

static bool mvee_shm_table_delete_entry(mvee_shm_table_entry* remove)
{
  if (remove)
  {
    __libc_lock_lock(mvee_shm_table_lock);

    /* Unlink */
    mvee_shm_table_entry* next = remove->next;
    mvee_shm_table_entry* prev = remove->prev;
    if (next)
      next->prev = prev;
    if (prev)
      orig_atomic_store_release(&prev->next, next);
    else if (remove == mvee_shm_table_head)/* Special case for head */
      orig_atomic_store_release(&mvee_shm_table_head, next);

    /* Free memory */
    free(remove);

    __libc_lock_unlock(mvee_shm_table_lock);
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
  unsigned short nr_of_variants_checked;
  unsigned char type;
  unsigned char replication_type;// 0 is no replication, 1 is replication from shadow memory, 2 is replication from buffer
  char data[];
} mvee_shm_op_entry;

typedef struct mvee_shm_op_ret {
  unsigned long val;
  bool cmp;
} mvee_shm_op_ret;

static __thread size_t                mvee_shm_local_pos    = 0; // our position in the thread local queue
static __thread char*                 mvee_shm_buffer       = NULL;
static __thread size_t                mvee_shm_buffer_size  = 0; // nr of slots in the thread local queue

static mvee_shm_op_entry* mvee_shm_get_entry(size_t size)
{
  // Get the buffer if we don't have it yet
  if (unlikely(!mvee_shm_buffer))
  {
    mvee_shm_buffer = (char*)syscall(__NR_shmat, syscall(MVEE_GET_SHARED_BUFFER, 0, MVEE_SHM_BUFFER, &mvee_shm_buffer_size, 1, 0), NULL, 0);
    syscall(MVEE_RESET_ATFORK, &mvee_shm_buffer, sizeof(mvee_shm_buffer));
    mvee_shm_local_pos = 0;
  }

  // Find location for entry in buffer
  size_t entry_size = MVEE_ROUND_UP(sizeof(mvee_shm_op_entry) + size, 64);
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
static inline mvee_shm_op_ret mvee_shm_buffered_op(const unsigned char type, const void* in_address, const mvee_shm_table_entry* in, void* out_address, const mvee_shm_table_entry* out, const size_t size, const uint64_t value, const uint64_t cmp)
{
  mvee_shm_op_ret ret = { 0 };
  // If the memory operation has no size, don't do it, don't make an entry, and don't even check
  if (unlikely(!size))
    return ret;

#ifdef MVEE_LOG_SHM_OPS
  syscall(__NR_gettid, 1337, 10000001, 100, type, size);
#endif

  // Get an entry
  mvee_shm_op_entry* entry = mvee_shm_get_entry(size);

  // Do equivalence checking, unique access, and replication
  // These differ between the variants, who need to synchronize with each other
  if (likely(mvee_master_variant))
  {
    ////////////////////////////////////////////////////////////////////////////////
    // Equivalence checking: leader fills in entry.
    ////////////////////////////////////////////////////////////////////////////////
    entry->address = in_address;
    entry->second_address = out_address;
    entry->size = size;
    entry->value = value;
    entry->cmp = cmp;
    entry->type = type;

    /* The input comes from a non-SHM page, fill in the buffer */
    if (unlikely(!in && ((type == MEMCPY) || (type == MEMMOVE))))
      orig_memcpy(&entry->data, in_address, size);

    // Signal to followers that entry is available
    orig_atomic_store_release(&entry->nr_of_variants_checked, 1);

    ////////////////////////////////////////////////////////////////////////////////
    // Unique access: leader does access (on actual and shadow memory),
    // enters replication data in buffer if required.
    ////////////////////////////////////////////////////////////////////////////////

    // Wait for followers to signal they finished checking. This is only necessary when we might write to shm (aka, when 'out' has a value).
    while (out && (orig_atomic_load_acquire(&entry->nr_of_variants_checked) != mvee_num_variants))
        arch_cpu_relax();

    bool data_in_buffer = false;
    switch(type)
    {
      case LOAD:
        {
          /* Load from actual SHM page */
          LOAD_BY_SIZE(out_address, in_address, size);

          /* If we have a shadow copy, compare */
          if (in->shadow)
          {
            /* Load from local shadow copy */
            char local_ret[8];
            LOAD_BY_SIZE(&local_ret, SHARED_TO_SHADOW_POINTER(in, in_address), size);

            /* If these two loads differ, put the load from the SHM page in the buffer */
            data_in_buffer = orig_memcmp(out_address, &local_ret, size);
          }
          /* If no shadow memory, always use buffer */
          else
            data_in_buffer = true;

          if (data_in_buffer)
            orig_memcpy(&entry->data, out_address, size);

          break;
        }
      case STORE:
        {
          /* Write to actual SHM page, from (non-overlapping) non-SHM page */
          STORE_BY_SIZE(out_address, value, size);

          /* Write local shadow copy, from (non-overlapping) non-SHM page */
          if (out->shadow)
            STORE_BY_SIZE(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
          break;
        }
      case MEMCPY:
      case MEMMOVE:
          {
           /* When doing reads/writes we will use memcpy, **or** memmove if so requested. We can relax certain memmove's however, if we now
            * **for a fact** that the destination and source won't overlap. For example, when handling MEMMOVE we can still perform writes from
            * non-SHM pages to local shadow copy pages using memcpy.
            */
            if (in)
            {
              /* The input comes from a SHM page. We handle this differently depending on whether there's shadow memory or not. */

              /* If there is a shadow copy, check whether it differs from our local copy or not. If it doesn't, we use our local copy as input.
               * If it **does** differ, we copy the modified data on the SHM page to the buffer, and use that copy as input.
               */
              if (in->shadow)
                data_in_buffer = orig_memcmp(SHARED_TO_SHADOW_POINTER(in, in_address), in_address, size);
              /* If no shadow memory, always use buffer */
              else
                data_in_buffer = true;

              if (data_in_buffer)
                orig_memcpy(&entry->data, in_address, size);
              const void* buf_or_shadow = data_in_buffer ? &entry->data : SHARED_TO_SHADOW_POINTER(in, in_address);

              if (out)
              {
                /* We're reading/writing to and from a SHM page. */
                /* Write to actual SHM page, from (non-overlapping) buffer or local shadow copy */
                orig_memcpy(out_address, buf_or_shadow, size);

                /* Write local shadow copy, from buffer (can memcpy!) or local shadow copy (memmove, if requested) */
                if (out->shadow)
                {
                  if (type == MEMMOVE && !data_in_buffer)
                    orig_memmove(SHARED_TO_SHADOW_POINTER(out, out_address), buf_or_shadow, size);
                  else
                    orig_memcpy(SHARED_TO_SHADOW_POINTER(out, out_address), buf_or_shadow, size);
                }
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
              /* The input comes from a non-SHM page */
              /* Write to actual SHM page, from (non-overlapping) non-SHM page */
              orig_memcpy(out_address, in_address, size);

              /* Write local shadow copy, from (non-overlapping) non-SHM page */
              if (out->shadow)
                orig_memcpy(SHARED_TO_SHADOW_POINTER(out, out_address), in_address, size);
            }
            break;
          }
      case MEMSET:
        {
          /* Write to actual SHM page */
          orig_memset(out_address, value, size);

          /* Write local shadow copy */
          if (out->shadow)
            orig_memset(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
          break;
        }
      case MEMCHR:
        {
          /* Search on actual SHM page */
          void* shm_ret = orig_memchr(in_address, value, size);

          /* Use offset as return value */
          ret.val = shm_ret - in_address;

          /* If we have a shadow copy, compare */
          if (in->shadow)
          {
            /* Search on local shadow copy */
            void* local_ret = orig_memchr(SHARED_TO_SHADOW_POINTER(in, in_address), value, size);

            /* In case these differ, we have to return.. A different pointer in the followers? We'll encode the pointer offset in the buffer, reusing the second_address field */
            data_in_buffer = (shm_ret != local_ret);
          }
          /* If no shadow memory, always use buffer */
          else
            data_in_buffer = true;

          if (data_in_buffer)
            entry->second_address = (void*)(shm_ret - in_address);

          break;
        }
      case ATOMICLOAD:
        {
          /* Load from actual SHM page */
          ATOMICLOAD_BY_SIZE(out_address, in_address, size);

          /* If we have a shadow copy, compare */
          if (in->shadow)
          {
            /* Load from local shadow copy */
            char local_ret[8];
            ATOMICLOAD_BY_SIZE(&local_ret, SHARED_TO_SHADOW_POINTER(in, in_address), size);

            /* If these two loads differ, put the load from the SHM page in the buffer */
            data_in_buffer = orig_memcmp(out_address, &local_ret, size);
          }
          /* If no shadow memory, always use buffer */
          else
            data_in_buffer = true;

          if (data_in_buffer)
            orig_memcpy(&entry->data, out_address, size);

          break;
        }
      case ATOMICSTORE:
        {
          /* Store on actual SHM page */
          ATOMICSTORE_BY_SIZE(out_address, value, size);

          /* Store on local shadow copy */
          if (out->shadow)
            ATOMICSTORE_BY_SIZE(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
          break;
        }
      case ATOMICCMPXCHG:
        {
          ret.cmp = ATOMICCMPXCHG_BY_SIZE(out_address, &cmp, value, size);
          entry->data[0] = ret.cmp;
          /* Comparison failed, cmp := *out_address */
          if (!ret.cmp)
          {
            orig_memcpy(&entry->data[1], &cmp, size);
            ret.val = cmp;
          }
          /* Comparison succeeded, *out_address := value . Adjust shadow memory accordingly. */
          else if (out->shadow)
            ATOMICSTORE_BY_SIZE(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);

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

    // Signal followers that replication data (or the sign of its absence) is available. Only necessary when actually reading from shm (aka, when 'in' has a value).
    if (in)
      orig_atomic_store_release(&entry->replication_type, data_in_buffer ? 2 : 1);
  }
  else
  {
    ////////////////////////////////////////////////////////////////////////////////
    // Equivalence checking: follower checks entry for equivalence with own operation
    ////////////////////////////////////////////////////////////////////////////////

    // Wait for leader to signal availability of the entry
    while (!orig_atomic_load_acquire(&entry->nr_of_variants_checked))
        arch_cpu_relax();

    // Assert we're on the same entry
    mvee_assert_same_address(entry->address, in_address);
    mvee_assert_same_size(entry->size, size);
    mvee_assert_same_type(entry->type, type);

    switch(type)
    {
      case MEMCPY:
      case MEMMOVE:
        {
          mvee_assert_same_address(entry->second_address, out_address);
          /* The input comes from a non-SHM page, check its correctness */
          if (!in)
            mvee_assert_same_store(&entry->data, in_address, size, out->shadow);
          break;
        }
      case ATOMICCMPXCHG:
        mvee_assert_same_value2(entry->cmp, cmp, out->shadow);
      case ATOMICRMW_XCHG:
      case ATOMICRMW_ADD:
      case ATOMICRMW_SUB:
      case ATOMICRMW_AND:
      case ATOMICRMW_NAND:
      case ATOMICRMW_OR:
      case ATOMICRMW_XOR:
      case ATOMICSTORE:
      case STORE:
        {
          mvee_assert_same_value2(entry->value, value, out->shadow);
          break;
        }
      case MEMCHR:
      case MEMSET:
        {
          mvee_assert_same_value1(entry->value, value);
          break;
        }
      default:
        break;
    }

    // Signal the leader that the check has succeeded. We only need to do this when we might write to shm (aka, when 'out' has a value).
    if (out)
      orig_atomic_increment(&entry->nr_of_variants_checked);

    ////////////////////////////////////////////////////////////////////////////////
    // Unique access: follower does access on shadow memory, reads replication data
    // from buffer if available.
    ////////////////////////////////////////////////////////////////////////////////

    // Wait for leader to signal that replication data is available. Only necessary when actually reading from shm (aka, when 'in' has a value).
    unsigned char replication_type = 0;
    while (in && ((replication_type = orig_atomic_load_acquire(&entry->replication_type)) == 0))
        arch_cpu_relax();

    bool data_in_buffer = (replication_type == 2);
    switch(type)
    {
      case LOAD:
        {
          /* If data present, copy from the buffer. Otherwise, load from local shadow copy */
          if (data_in_buffer)
            orig_memcpy(out_address, &entry->data, size);
          else
            LOAD_BY_SIZE(out_address, SHARED_TO_SHADOW_POINTER(in, in_address), size);
          break;
        }
      case STORE:
        {
          /* Write local shadow copy, from (non-overlapping) non-SHM page */
          if (out->shadow)
              STORE_BY_SIZE(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
          break;
        }
      case MEMCPY:
      case MEMMOVE:
        {
          if (in)
          {
            /* The input comes from a SHM page. Check whether to read from the buffer or from the local shadow copy. */
            const void* buf_or_shadow = data_in_buffer ? &entry->data : SHARED_TO_SHADOW_POINTER(in, in_address);

            if (out && out->shadow)
            {
              /* We're reading/writing to and from a SHM page. Write to the local shadow copy using a memcpy or memmove, depending
               * on whether we can relax any requested MEMMOVEs (if we **know** source and destination won't overlap).
               */
              if (type == MEMMOVE && !data_in_buffer)
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
            /* The input comes from a non-SHM page */
            /* Write local shadow copy */
            if (out->shadow)
              orig_memcpy(SHARED_TO_SHADOW_POINTER(out, out_address), in_address, size);
          }
          break;
        }
      case MEMSET:
        {
          /* Write local shadow copy */
          if (out->shadow)
            orig_memset(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
          break;
        }
      case MEMCHR:
        {
          /* Something changed in the leader, leading to a different return value. Get it from buffer */
          if (data_in_buffer)
            ret.val = (unsigned long)entry->second_address;
          else
          {
            /* Search on local shadow copy */
            void* local_ret = orig_memchr(SHARED_TO_SHADOW_POINTER(in, in_address), value, size);

            /* Use offset as return value */
            ret.val = local_ret - in_address;
          }

          break;
        }
      case ATOMICLOAD:
        {
          /* If data present, copy from the buffer. Otherwise, load from local shadow copy */
          if (data_in_buffer)
            orig_memcpy(out_address, &entry->data, size);
          else
            ATOMICLOAD_BY_SIZE(out_address, SHARED_TO_SHADOW_POINTER(in, in_address), size);
          break;
        }
      case ATOMICSTORE:
        {
          /* Store on local shadow copy */
          if (out->shadow)
            ATOMICSTORE_BY_SIZE(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
          break;
        }
      case ATOMICCMPXCHG:
        {
          ret.cmp = entry->data[0];

          /* Comparison failed, cmp := *out_address */
          if (!ret.cmp)
          {
            orig_memcpy(&ret.val, &entry->data[1], size);
          }
          /* Comparison succeeded, *out_address := value . Adjust shadow memory accordingly. */
          else if (out->shadow)
            ATOMICSTORE_BY_SIZE(SHARED_TO_SHADOW_POINTER(out, out_address), value, size);
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

  return ret;
}

// ========================================================================================================================
// The mvee_shm_op interface used by the wrapping shm_support compiler pass
// ========================================================================================================================
mvee_shm_op_ret mvee_shm_op(unsigned char id, void* address, unsigned long size, unsigned long value, unsigned long cmp)
{
  mvee_shm_op_ret ret = { 0 };

  // Use SHM buffer
  address = mvee_shm_decode_address(address);
  mvee_shm_table_entry* entry = mvee_shm_table_get_entry(address);
  if (unlikely(!entry))
    mvee_error_shm_entry_not_present(address);

  switch (id)
  {
    case ATOMICLOAD:
    case LOAD:
      mvee_shm_buffered_op(id, address, entry, &ret.val, NULL, size, 0, 0);
      break;
    case ATOMICSTORE:
    case STORE:
      mvee_shm_buffered_op(id, NULL, NULL, address, entry, size, value, 0);
      break;
    case ATOMICCMPXCHG:
      ret = mvee_shm_buffered_op(id, address, entry, address, entry, size, value, cmp);
      break;
    case ATOMICRMW_XCHG:
    case ATOMICRMW_ADD:
    case ATOMICRMW_SUB:
    case ATOMICRMW_AND:
    case ATOMICRMW_NAND:
    case ATOMICRMW_OR:
    case ATOMICRMW_XOR:
      ret = mvee_shm_buffered_op(id, address, entry, address, entry, size, value, 0);
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
  if (unlikely(!dest_entry && !src_entry))
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
  if (unlikely(!dest_entry && !src_entry))
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
  if (unlikely(!entry))
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
  if (unlikely(!entry))
    mvee_error_shm_entry_not_present(src);

  mvee_shm_op_ret ret = mvee_shm_buffered_op(MEMCHR, shm_src, entry, NULL, NULL, n, c_in, 0);
  return (void*)src + ret.val;
}

int
mvee_shm_memcmp (const void *s1, const void *s2, size_t len)
{
  /* Decode addresses */
  const void* shm_s1 = mvee_shm_decode_address(s1);
  const void* shm_s2 = mvee_shm_decode_address(s2);
  mvee_shm_table_entry* s1_entry = mvee_shm_table_get_entry(shm_s1);
  mvee_shm_table_entry* s2_entry = mvee_shm_table_get_entry(shm_s2);
  if (unlikely(!s1_entry && !s2_entry))
    mvee_error_shm_entry_not_present(s1_entry);

  // Get an entry in the replication buffer.
  // If both addresses are shared memory pointer, we will need a buffer of double length.
  mvee_shm_op_entry* entry = mvee_shm_get_entry((s1_entry && s2_entry) ? (len * 2) : len);
  if (likely(mvee_master_variant))
  {
      // Fill in the entry.
    entry->address = s1_entry ? shm_s1 : shm_s2;
    entry->second_address = (s1_entry && s2_entry) ? shm_s2 : NULL;
    entry->size = len;
    entry->type = MEMCMP;

    // When the first pointer is a shared memory pointer, check if the shared memory contents are still the
    // same as in the shadow mapping, update the replication entry accordingly.
    unsigned char replication_type = 4;
    if (s1_entry)
    {
      // Copy the shared memory contents to the buffer and replace the pointer we'll be using from now on.
      // This ensures that our view of shared memory does not change over the course of this function.
      void* temp = entry->data;
      orig_memcpy(temp, shm_s1, len);

      // Update entry if the contents of shared memory differ with the shadow
      if (s1_entry->shadow)
      {
        if (orig_memcmp(temp, SHARED_TO_SHADOW_POINTER(s1_entry, shm_s1), len))
          replication_type |= 1;
      }
      /* If no shadow memory, always use buffer */
      else
        replication_type |= 1;
      shm_s1 = temp;
    }
    else
      shm_s1 = s1;
    // same as for first pointer
    if (s2_entry)
    {
      // Copy the shared memory contents to the buffer and replace the pointer we'll be using from now on.
      // This ensures that our view of shared memory does not change over the course of this function.
      void* temp = entry->data + ((s1_entry && s2_entry) ? len : 0);
      orig_memcpy(temp, shm_s2, len);

      // Update entry if the contents of shared memory differ with the shadow
      if (s2_entry->shadow)
      {
        if (orig_memcmp(temp, SHARED_TO_SHADOW_POINTER(s2_entry, shm_s2), len))
          replication_type |= 2;
      }
      /* If no shadow memory, always use buffer */
      else
        replication_type |= 2;
      shm_s2 = temp;
    }
    else
      shm_s2 = s2;

    // save the return value for the memcmp.
    entry->value = orig_memcmp(shm_s1, shm_s2, len);
    orig_atomic_store_release(&entry->nr_of_variants_checked, 1);
    orig_atomic_store_release(&entry->replication_type, replication_type);
  }
  else
  {
    // Wait until entry is ready
    while (!orig_atomic_load_acquire(&entry->replication_type))
        arch_cpu_relax();

    // Check entry
    mvee_assert_same_address(entry->address, (s1_entry ? shm_s1 : shm_s2));
    mvee_assert_same_address(entry->second_address, ((s1_entry && s2_entry) ? shm_s2 : NULL));
    mvee_assert_same_size(entry->size, len);
    mvee_assert_same_type(entry->type, MEMCMP);

    if (s1_entry)
    {
      if (entry->replication_type & 1)
        shm_s1 = entry->data;
      else
        shm_s1 = SHARED_TO_SHADOW_POINTER(s1_entry, shm_s1);
    }
    else
      shm_s1 = s1;
    if (s2_entry)
    {
      if (entry->replication_type & 2)
        shm_s2 = entry->data + (s1_entry ? len : 0);
      else
        shm_s2 = SHARED_TO_SHADOW_POINTER(s2_entry, shm_s2);
    }
    else
      shm_s2 = s2;

    // perform the memcmp
    int return_value = orig_memcmp(shm_s1, shm_s2, len);

    // the return value for memcmp should be the same
    mvee_assert_same_value1(entry->value, return_value);
  }

  return entry->value;
}

int
mvee_shm_strcmp (const char *str1, const char *str2)
{
  /* Decode addresses */
  void* shm_str1 = mvee_shm_decode_address(str1);
  void* shm_str2 = mvee_shm_decode_address(str2);
  mvee_shm_table_entry* str1_entry = mvee_shm_table_get_entry(shm_str1);
  mvee_shm_table_entry* str2_entry = mvee_shm_table_get_entry(shm_str2);
  if (unlikely(!str1_entry && !str2_entry))
    mvee_error_shm_entry_not_present(str1_entry);

  // Get an entry in the replication buffer.
  mvee_shm_op_entry* entry = mvee_shm_get_entry(sizeof(int));
  if (likely(mvee_master_variant))
  {
      // Fill in the entry.
    entry->address = str1_entry ? shm_str1 : shm_str2;
    entry->second_address = (str1_entry && str2_entry) ? shm_str2 : NULL;
    entry->type = STRCMP;

    // save the return value
    *(int*)entry->data = orig_strcmp(str1_entry ? shm_str1 : str1, str2_entry ? shm_str2 : str2);
    orig_atomic_store_release(&entry->nr_of_variants_checked, 1);
    orig_atomic_store_release(&entry->replication_type, 2);
  }
  else
  {
    // Wait until entry is ready
    while (!orig_atomic_load_acquire(&entry->replication_type))
        arch_cpu_relax();

    // Check entry
    mvee_assert_same_address(entry->address, (str1_entry ? shm_str1 : shm_str2));
    mvee_assert_same_address(entry->second_address, ((str1_entry && str2_entry) ? shm_str2 : NULL));
    mvee_assert_same_type(entry->type, STRCMP);
  }

  return entry->value;
}

size_t
mvee_shm_strlen (const char *str)
{
  const char *shm_str = mvee_shm_decode_address(str);
  mvee_shm_table_entry* shm_entry = mvee_shm_table_get_entry(shm_str);
  if (unlikely(!shm_entry))
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
    orig_atomic_store_release(&entry->nr_of_variants_checked, 1);
    orig_atomic_store_release(&entry->replication_type, 2);
  }
  else
  {
    // Wait until entry is ready
    while (!orig_atomic_load_acquire(&entry->replication_type))
        arch_cpu_relax();

    // Check entry
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
  if ((void *) ret != MAP_FAILED && (long) ret < 0)
  {
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

    if (mapping->shadow)
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

    if (mapping->shadow)
      orig_SHMDT_CALL(mapping->shadow);
    mvee_shm_table_delete_entry(mapping);
  }
  return orig_MUNMAP_CALL(addr, len);
}
