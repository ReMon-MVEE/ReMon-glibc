/* Atomic operations.  Pure ARM version.
   Copyright (C) 2002-2020 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library.  If not, see
   <https://www.gnu.org/licenses/>.  */

#include <stdint.h>

typedef int8_t atomic8_t;
typedef uint8_t uatomic8_t;
typedef int_fast8_t atomic_fast8_t;
typedef uint_fast8_t uatomic_fast8_t;

typedef int32_t atomic32_t;
typedef uint32_t uatomic32_t;
typedef int_fast32_t atomic_fast32_t;
typedef uint_fast32_t uatomic_fast32_t;

typedef intptr_t atomicptr_t;
typedef uintptr_t uatomicptr_t;
typedef intmax_t atomic_max_t;
typedef uintmax_t uatomic_max_t;

#define __HAVE_64B_ATOMICS 0
#define USE_ATOMIC_COMPILER_BUILTINS 0
#define ATOMIC_EXCHANGE_USES_CAS 1

void __arm_link_error (void);

#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4
# define atomic_full_barrier() __sync_synchronize ()
#else
# define atomic_full_barrier() __arm_assisted_full_barrier ()
#endif

/* An OS-specific atomic-machine.h file will define this macro if
   the OS can provide something.  If not, we'll fail to build
   with a compiler that doesn't supply the operation.  */
#ifndef __arm_assisted_full_barrier
# define __arm_assisted_full_barrier()  __arm_link_error()
#endif

/* Use the atomic builtins provided by GCC in case the backend provides
   a pattern to do this efficiently.  */
#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4

# define orig_atomic_exchange_acq(mem, value)                                \
  __atomic_val_bysize (orig___arch_exchange, int, mem, value, __ATOMIC_ACQUIRE)

# define orig_atomic_exchange_rel(mem, value)                                \
  __atomic_val_bysize (orig___arch_exchange, int, mem, value, __ATOMIC_RELEASE)

/* Atomic exchange (without compare).  */

# define orig___arch_exchange_8_int(mem, newval, model)      \
  (__arm_link_error (), (typeof (*mem)) 0)

# define orig___arch_exchange_16_int(mem, newval, model)     \
  (__arm_link_error (), (typeof (*mem)) 0)

# define orig___arch_exchange_32_int(mem, newval, model)     \
  __atomic_exchange_n (mem, newval, model)

# define orig___arch_exchange_64_int(mem, newval, model)     \
  (__arm_link_error (), (typeof (*mem)) 0)

/* Compare and exchange with "acquire" semantics, ie barrier after.  */

# define orig_atomic_compare_and_exchange_bool_acq(mem, new, old)    \
  __atomic_bool_bysize (orig___arch_compare_and_exchange_bool, int,  \
                        mem, new, old, __ATOMIC_ACQUIRE)

# define orig_atomic_compare_and_exchange_val_acq(mem, new, old)     \
  __atomic_val_bysize (orig___arch_compare_and_exchange_val, int,    \
                       mem, new, old, __ATOMIC_ACQUIRE)

/* Compare and exchange with "release" semantics, ie barrier before.  */

# define orig_atomic_compare_and_exchange_val_rel(mem, new, old)      \
  __atomic_val_bysize (orig___arch_compare_and_exchange_val, int,    \
                       mem, new, old, __ATOMIC_RELEASE)

/* Compare and exchange.
   For all "bool" routines, we return FALSE if exchange succesful.  */

# define orig___arch_compare_and_exchange_bool_8_int(mem, newval, oldval, model) \
  ({__arm_link_error (); 0; })

# define orig___arch_compare_and_exchange_bool_16_int(mem, newval, oldval, model) \
  ({__arm_link_error (); 0; })

# define orig___arch_compare_and_exchange_bool_32_int(mem, newval, oldval, model) \
  ({                                                                    \
    typeof (*mem) __oldval = (oldval);                                  \
    !__atomic_compare_exchange_n (mem, (void *) &__oldval, newval, 0,   \
                                  model, __ATOMIC_RELAXED);             \
  })

# define orig___arch_compare_and_exchange_bool_64_int(mem, newval, oldval, model) \
  ({__arm_link_error (); 0; })

# define orig___arch_compare_and_exchange_val_8_int(mem, newval, oldval, model) \
  ({__arm_link_error (); oldval; })

# define orig___arch_compare_and_exchange_val_16_int(mem, newval, oldval, model) \
  ({__arm_link_error (); oldval; })

# define orig___arch_compare_and_exchange_val_32_int(mem, newval, oldval, model) \
  ({                                                                    \
    typeof (*mem) __oldval = (oldval);                                  \
    __atomic_compare_exchange_n (mem, (void *) &__oldval, newval, 0,    \
                                 model, __ATOMIC_RELAXED);              \
    __oldval;                                                           \
  })

# define orig___arch_compare_and_exchange_val_64_int(mem, newval, oldval, model) \
  ({__arm_link_error (); oldval; })

#else
# define orig___arch_compare_and_exchange_val_32_acq(mem, newval, oldval) \
  __arm_assisted_compare_and_exchange_val_32_acq ((mem), (newval), (oldval))
#endif

#ifndef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4
/* We don't support atomic operations on any non-word types.
   So make them link errors.  */
# define __arch_compare_and_exchange_val_8_acq(mem, newval, oldval) \
  ({ __arm_link_error (); oldval; })

# define __arch_compare_and_exchange_val_16_acq(mem, newval, oldval) \
  ({ __arm_link_error (); oldval; })

# define __arch_compare_and_exchange_val_64_acq(mem, newval, oldval) \
  ({ __arm_link_error (); oldval; })
#endif

/* An OS-specific atomic-machine.h file will define this macro if
   the OS can provide something.  If not, we'll fail to build
   with a compiler that doesn't supply the operation.  */
#ifndef __arm_assisted_compare_and_exchange_val_32_acq
# define __arm_assisted_compare_and_exchange_val_32_acq(mem, newval, oldval) \
  ({ __arm_link_error (); oldval; })
#endif

/*--------------------------------------------------------------------------------
                                  MVEE PATCHES
--------------------------------------------------------------------------------*/
#define USE_MVEE_LIBC

#ifdef USE_MVEE_LIBC
// NOTE: This is different from the base value for x86 because the ARM syscall
// instruction can only encode small immediates as syscall numbers
#define MVEE_FAKE_SYSCALL_BASE          0x6FF 
#define MVEE_GET_MASTERTHREAD_ID        MVEE_FAKE_SYSCALL_BASE + 3
#define MVEE_GET_SHARED_BUFFER          MVEE_FAKE_SYSCALL_BASE + 4
#define MVEE_FLUSH_SHARED_BUFFER        MVEE_FAKE_SYSCALL_BASE + 5
#define MVEE_SET_INFINITE_LOOP_PTR      MVEE_FAKE_SYSCALL_BASE + 6
#define MVEE_TOGGLESYNC                 MVEE_FAKE_SYSCALL_BASE + 7
#define MVEE_SET_SHARED_BUFFER_POS_PTR  MVEE_FAKE_SYSCALL_BASE + 8
#define MVEE_RUNS_UNDER_MVEE_CONTROL    MVEE_FAKE_SYSCALL_BASE + 9
#define MVEE_GET_THREAD_NUM             MVEE_FAKE_SYSCALL_BASE + 10
#define MVEE_SET_SYNC_PRIMITIVES_PTR    MVEE_FAKE_SYSCALL_BASE + 12
#define MVEE_ALL_HEAPS_ALIGNED          MVEE_FAKE_SYSCALL_BASE + 13
#define MVEE_GET_VIRTUALIZED_ARGV0      MVEE_FAKE_SYSCALL_BASE + 17
#define MVEE_LOG_SHM_MEM_OP             MVEE_FAKE_SYSCALL_BASE + 20
#define MVEE_LIBC_LOCK_BUFFER           3
#define MVEE_LIBC_MALLOC_DEBUG_BUFFER   11
#define MVEE_LIBC_ATOMIC_BUFFER         13
#define MVEE_FUTEX_WAIT_TID             30

enum mvee_alloc_types
{
	LIBC_MALLOC,
	LIBC_FREE,
	LIBC_REALLOC,
	LIBC_MEMALIGN,
	LIBC_CALLOC,
	MALLOC_TRIM,
	HEAP_TRIM,
	MALLOC_CONSOLIDATE,
	ARENA_GET2,
	_INT_MALLOC,
	_INT_FREE,
	_INT_REALLOC
};

//
// Atomic operations list. Keep this in sync with MVEE/Inc/MVEE_shm.h
//
enum mvee_atomics
{
    // LOAD OPERATIONS FIRST!!! DO NOT CHANGE THIS CONVENTION
    ATOMIC_FORCED_READ,
    ATOMIC_LOAD,
    // THE FOLLOWING IS NOT AN ACTUAL ATOMIC OPERATION, IT JUST DENOTES THE END OF THE LOAD-ONLY ATOMICS!!!
    ATOMIC_LOAD_MAX,
    // STORES AFTER LOADS
    CATOMIC_AND,
    CATOMIC_OR,
    CATOMIC_EXCHANGE_AND_ADD,
    CATOMIC_ADD,
    CATOMIC_INCREMENT,
    CATOMIC_DECREMENT,
    CATOMIC_MAX,
    ATOMIC_COMPARE_AND_EXCHANGE_VAL,
    ATOMIC_COMPARE_AND_EXCHANGE_BOOL,
    ATOMIC_EXCHANGE,
    ATOMIC_EXCHANGE_AND_ADD,
    ATOMIC_INCREMENT_AND_TEST,
    ATOMIC_DECREMENT_AND_TEST,
	ATOMIC_ADD_NEGATIVE,
    ATOMIC_ADD_ZERO,
    ATOMIC_ADD,
	ATOMIC_OR,
	ATOMIC_OR_VAL,
    ATOMIC_INCREMENT,
    ATOMIC_DECREMENT,
    ATOMIC_BIT_TEST_SET,
    ATOMIC_BIT_SET,
    ATOMIC_AND,
	ATOMIC_AND_VAL,
    ATOMIC_STORE,
	ATOMIC_MIN,
    ATOMIC_MAX,
    ATOMIC_DECREMENT_IF_POSITIVE,
	ATOMIC_FETCH_ADD,
	ATOMIC_FETCH_AND,
	ATOMIC_FETCH_OR,
	ATOMIC_FETCH_XOR,
    ___UNKNOWN_LOCK_TYPE___,
    __MVEE_ATOMICS_MAX__
};

#define MVEE_ROUND_UP(x, multiple)				\
	((x + (multiple - 1)) & ~(multiple -1))
#define MVEE_MIN(a, b) ((a > b) ? (b) : (a))

# ifdef MVEE_USE_TOTALPARTIAL_AGENT
#  warning "The total and partial order agents have not been tested on ARM and will probably break!"
#  include "mvee-totalpartial-agent.h"
# else
#  include "mvee-woc-agent.h"
# endif

#endif

// We don't use our sync agent in the dynamic loader so just use the original atomics everywhere
#if IS_IN (rtld) || !defined(USE_MVEE_LIBC)

//
// generic atomics (include/atomic.h and sysdeps/arch/atomic-machine.h)
//
#define __arch_c_compare_and_exchange_val_8_acq(mem, newval, oldval) orig___arch_c_compare_and_exchange_val_8_acq(mem, newval, oldval)
#define __arch_c_compare_and_exchange_val_16_acq(mem, newval, oldval) orig___arch_c_compare_and_exchange_val_16_acq(mem, newval, oldval)
#define __arch_c_compare_and_exchange_val_32_acq(mem, newval, oldval) orig___arch_c_compare_and_exchange_val_32_acq(mem, newval, oldval)
#define __arch_c_compare_and_exchange_val_64_acq(mem, newval, oldval) orig___arch_c_compare_and_exchange_val_64_acq(mem, newval, oldval)
#define atomic_compare_and_exchange_val_acq(mem, newval, oldval) orig_atomic_compare_and_exchange_val_acq(mem, newval, oldval)
#define atomic_compare_and_exchange_val_rel(mem, newval, oldval) orig_atomic_compare_and_exchange_val_rel(mem, newval, oldval)
#define atomic_compare_and_exchange_bool_acq(mem, newval, oldval) orig_atomic_compare_and_exchange_bool_acq(mem, newval, oldval)
#define atomic_compare_and_exchange_bool_rel(mem, newval, oldval) orig_atomic_compare_and_exchange_bool_rel(mem, newval, oldval)
#define atomic_exchange_acq(mem, newvalue) orig_atomic_exchange_acq(mem, newvalue)
#define atomic_exchange_rel(mem, newvalue) orig_atomic_exchange_rel(mem, newvalue)
#define atomic_exchange_and_add(mem, value) orig_atomic_exchange_and_add(mem, value)
#define atomic_exchange_and_add_acq(mem, value) orig_atomic_exchange_and_add_acq(mem, value)
#define atomic_exchange_and_add_rel(mem, value) orig_atomic_exchange_and_add_rel(mem, value)
#define atomic_add(mem, value) orig_atomic_add(mem, value)
#define atomic_increment(mem) orig_atomic_increment(mem)
#define atomic_increment_and_test(mem) orig_atomic_increment_and_test(mem)
#define atomic_increment_val(mem) orig_atomic_increment_val(mem)
#define atomic_decrement(mem) orig_atomic_decrement(mem)
#define atomic_decrement_and_test(mem) orig_atomic_decrement_and_test(mem)
#define atomic_decrement_val(mem) orig_atomic_decrement_val(mem)
#define atomic_add_negative(mem, value) orig_atomic_add_negative(mem, value)
#define atomic_add_zero(mem, value) orig_atomic_add_zero(mem, value)
#define atomic_bit_set(mem, bit) orig_atomic_bit_set(mem, bit)
#define atomic_bit_test_set(mem, bit) orig_atomic_bit_test_set(mem, bit)
#define atomic_and(mem, mask) orig_atomic_and(mem, mask)
#define atomic_or(mem, mask) orig_atomic_or(mem, mask)
#define atomic_max(mem, value) orig_atomic_max(mem, value)
#define atomic_min(mem, value) orig_atomic_min(mem, value)
#define atomic_decrement_if_positive(mem) orig_atomic_decrement_if_positive(mem)
#define atomic_and_val(mem, mask) orig_atomic_and_val(mem, mask)
#define atomic_or_val(mem, mask) orig_atomic_or_val(mem, mask)
#define atomic_forced_read(x) orig_atomic_forced_read(x)
#define catomic_compare_and_exchange_val_acq(mem, newval, oldval) orig_catomic_compare_and_exchange_val_acq(mem, newval, oldval)
#define catomic_compare_and_exchange_val_rel(mem, newval, oldval) orig_catomic_compare_and_exchange_val_rel(mem, newval, oldval)
#define catomic_compare_and_exchange_bool_acq(mem, newval, oldval) orig_catomic_compare_and_exchange_bool_acq(mem, newval, oldval)
#define catomic_compare_and_exchange_bool_rel(mem, newval, oldval) orig_catomic_compare_and_exchange_bool_rel(mem, newval, oldval)
#define catomic_exchange_and_add(mem, value) orig_catomic_exchange_and_add(mem, value)
#define catomic_add(mem, value) orig_catomic_add(mem, value)
#define catomic_increment(mem) orig_catomic_increment(mem)
#define catomic_increment_val(mem) orig_catomic_increment_val(mem)
#define catomic_decrement(mem) orig_catomic_decrement(mem)
#define catomic_decrement_val(mem) orig_catomic_decrement_val(mem)
#define catomic_and(mem, mask) orig_catomic_and(mem, mask)
#define catomic_or(mem, mask) orig_catomic_or(mem, mask)
#define catomic_max(mem, value) orig_catomic_max(mem, value)

//
// C11-style atomics (include/atomic.h)
//
#define atomic_load_relaxed(mem) orig_atomic_load_relaxed(mem)
#define atomic_load_acquire(mem) orig_atomic_load_acquire(mem)
#define atomic_store_relaxed(mem, val) orig_atomic_store_relaxed(mem, val)
#define atomic_store_release(mem, val) orig_atomic_store_release(mem, val)
#define atomic_compare_exchange_weak_relaxed(mem, expected, desired) orig_atomic_compare_exchange_weak_relaxed(mem, expected, desired)
#define atomic_compare_exchange_weak_acquire(mem, expected, desired) orig_atomic_compare_exchange_weak_acquire(mem, expected, desired) 
#define atomic_compare_exchange_weak_release(mem, expected, desired) orig_atomic_compare_exchange_weak_release(mem, expected, desired)
#define atomic_exchange_relaxed(mem, desired) orig_atomic_exchange_relaxed(mem, desired)
#define atomic_exchange_acquire(mem, desired) orig_atomic_exchange_acquire(mem, desired)
#define atomic_exchange_release(mem, desired) orig_atomic_exchange_release(mem, desired)
#define atomic_fetch_add_relaxed(mem, operand) orig_atomic_fetch_add_relaxed(mem, operand)
#define atomic_fetch_add_acquire(mem, operand) orig_atomic_fetch_add_acquire(mem, operand)
#define atomic_fetch_add_release(mem, operand) orig_atomic_fetch_add_release(mem, operand)
#define atomic_fetch_add_acq_rel(mem, operand) orig_atomic_fetch_add_acq_rel(mem, operand)
#define atomic_fetch_and_relaxed(mem, operand) orig_atomic_fetch_and_relaxed(mem, operand)
#define atomic_fetch_and_acquire(mem, operand) orig_atomic_fetch_and_acquire(mem, operand)
#define atomic_fetch_and_release(mem, operand) orig_atomic_fetch_and_release(mem, operand)
#define atomic_fetch_or_relaxed(mem, operand) orig_atomic_fetch_or_relaxed(mem, operand) 
#define atomic_fetch_or_acquire(mem, operand) orig_atomic_fetch_or_acquire(mem, operand) 
#define atomic_fetch_or_release(mem, operand) orig_atomic_fetch_or_release(mem, operand) 
#define atomic_fetch_xor_release(mem, operand) orig_atomic_fetch_xor_release(mem, operand) 

//
// MVEE additions
//
#define THREAD_ATOMIC_GETMEM(descr, member) THREAD_GETMEM(descr, member)
#define THREAD_ATOMIC_SETMEM(descr, member, val) THREAD_SETMEM(descr, member, val)


#else // !IS_IN_rtld

//
// architecture-specific atomics (atomic-machine.h)
//
#define __arch_c_compare_and_exchange_val_8_acq(mem, newval, oldval)	\
	({																	\
		typeof(*mem) ____result;										\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_VAL, mem, 1);			\
		____result = orig___arch_c_compare_and_exchange_val_8_acq(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define __arch_c_compare_and_exchange_val_16_acq(mem, newval, oldval)	\
	({																	\
		typeof(*mem) ____result;										\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_VAL, mem, 1);			\
		____result = orig___arch_c_compare_and_exchange_val_16_acq(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define __arch_c_compare_and_exchange_val_32_acq(mem, newval, oldval)	\
	({																	\
		typeof(*mem) ____result;										\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_VAL, mem, 1);			\
		____result = orig___arch_c_compare_and_exchange_val_32_acq(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define __arch_c_compare_and_exchange_val_64_acq(mem, newval, oldval)	\
	({																	\
		typeof(*mem) ____result;										\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_VAL, mem, 1);			\
		____result = orig___arch_c_compare_and_exchange_val_64_acq(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define atomic_compare_and_exchange_val_acq(mem, newval, oldval)		\
	({																	\
		typeof(*mem) ____result;										\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_VAL, mem, 1);			\
		____result = orig_atomic_compare_and_exchange_val_acq(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define atomic_compare_and_exchange_val_rel(mem, newval, oldval)		\
	({																	\
		typeof(*mem) ____result;										\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_VAL, mem, 1);			\
		____result = orig_atomic_compare_and_exchange_val_rel(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define atomic_compare_and_exchange_bool_acq(mem, newval, oldval)		\
	({																	\
		bool ____result;												\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_BOOL, mem, 1);			\
		____result = orig_atomic_compare_and_exchange_bool_acq(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define atomic_compare_and_exchange_bool_rel(mem, newval, oldval)		\
	({																	\
		bool ____result;												\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_BOOL, mem, 1);			\
		____result = orig_atomic_compare_and_exchange_bool_rel(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define atomic_exchange_acq(mem, newvalue)						\
	({															\
		typeof(*mem) ____result;								\
		MVEE_PREOP(ATOMIC_EXCHANGE, mem, 1);					\
		____result = orig_atomic_exchange_acq(mem, newvalue);	\
		MVEE_POSTOP();											\
		____result;												\
	})

#define atomic_exchange_rel(mem, newvalue)						\
	({															\
		typeof(*mem) ____result;								\
		MVEE_PREOP(ATOMIC_EXCHANGE, mem, 1);					\
		____result = orig_atomic_exchange_rel(mem, newvalue);	\
		MVEE_POSTOP();											\
		____result;												\
	})

#define atomic_exchange_and_add(mem, value)						\
	({															\
		typeof(*mem) ____result;								\
		MVEE_PREOP(ATOMIC_EXCHANGE_AND_ADD, mem, 1);			\
		____result = orig_atomic_exchange_and_add(mem, value);	\
		MVEE_POSTOP();											\
		____result;												\
	})

#define atomic_exchange_and_add_acq(mem, value)					\
	({															\
		typeof(*mem) ____result;								\
		MVEE_PREOP(ATOMIC_EXCHANGE_AND_ADD, mem, 1);			\
		____result = orig_atomic_exchange_and_add_acq(mem, value);	\
		MVEE_POSTOP();											\
		____result;												\
	})

#define atomic_exchange_and_add_rel(mem, value)					\
	({															\
		typeof(*mem) ____result;								\
		MVEE_PREOP(ATOMIC_EXCHANGE_AND_ADD, mem, 1);			\
		____result = orig_atomic_exchange_and_add_rel(mem, value);	\
		MVEE_POSTOP();											\
		____result;												\
	})

#define atomic_add(mem, value)					\
	({											\
		MVEE_PREOP(ATOMIC_ADD, mem, 1);			\
		orig_atomic_add(mem, value);			\
		MVEE_POSTOP();							\
	})

#define atomic_increment(mem)					\
	({											\
		MVEE_PREOP(ATOMIC_INCREMENT, mem, 1);	\
		orig_atomic_increment(mem);				\
		MVEE_POSTOP();							\
	})

#define atomic_increment_and_test(mem)						\
	({														\
		unsigned char ____result;							\
		MVEE_PREOP(ATOMIC_INCREMENT_AND_TEST, mem, 1);		\
		____result = orig_atomic_increment_and_test(mem);	\
		MVEE_POSTOP();										\
		____result;											\
	})

#define atomic_increment_val(mem)				\
	({											\
		typeof(*mem) ____result;				\
		MVEE_PREOP(ATOMIC_INCREMENT, mem, 1);	\
		____result = orig_atomic_increment_val(mem);	\
		MVEE_POSTOP();							\
		____result;								\
	})

#define atomic_decrement(mem)					\
	({											\
		MVEE_PREOP(ATOMIC_DECREMENT, mem, 1);	\
		orig_atomic_decrement(mem);				\
		MVEE_POSTOP();							\
	})

#define atomic_decrement_and_test(mem)						\
	({														\
		unsigned char ____result;							\
		MVEE_PREOP(ATOMIC_DECREMENT_AND_TEST, mem, 1);		\
		____result = orig_atomic_decrement_and_test(mem);	\
		MVEE_POSTOP();										\
		____result;											\
	})

#define atomic_decrement_val(mem)				\
	({											\
		typeof(*mem) ____result;				\
		MVEE_PREOP(ATOMIC_DECREMENT, mem, 1);	\
		____result = orig_atomic_decrement_val(mem);	\
		MVEE_POSTOP();							\
		____result;								\
	})

#define atomic_add_negative(mem, value)						\
	({														\
		unsigned char ____result;							\
		MVEE_PREOP(ATOMIC_ADD, mem, 1);						\
		____result = orig_atomic_add_negative(mem, value);	\
		MVEE_POSTOP();										\
		____result;											\
	})

#define atomic_add_zero(mem, value)						\
	({													\
		unsigned char ____result;						\
		MVEE_PREOP(ATOMIC_ADD_ZERO, mem, 1);			\
		____result = orig_atomic_add_zero(mem, value);	\
		MVEE_POSTOP();									\
		____result;										\
	})

#define atomic_bit_set(mem, bit)				\
	({											\
		MVEE_PREOP(ATOMIC_BIT_SET, mem, 1);		\
		orig_atomic_bit_set(mem, bit);			\
		MVEE_POSTOP();							\
	})

#define atomic_bit_test_set(mem, bit)						\
	({														\
		unsigned char ____result;							\
		MVEE_PREOP(ATOMIC_BIT_TEST_SET, mem, 1);			\
		____result = orig_atomic_bit_test_set(mem, bit);	\
		MVEE_POSTOP();										\
		____result;											\
	})

#define atomic_and(mem, mask)					\
	({											\
		MVEE_PREOP(ATOMIC_AND, mem, 1);			\
		orig_atomic_and(mem, mask);				\
		MVEE_POSTOP();							\
	})

#define atomic_or(mem, mask)					\
	({											\
		MVEE_PREOP(ATOMIC_OR, mem, 1);			\
		orig_atomic_or(mem, mask);				\
		MVEE_POSTOP();							\
	})

#define atomic_max(mem, value)					\
	({											\
		MVEE_PREOP(ATOMIC_MAX, mem, 1);			\
		orig_atomic_max(mem, value);			\
		MVEE_POSTOP();							\
	})

#define atomic_min(mem, value)					\
	({											\
		MVEE_PREOP(ATOMIC_MIN, mem, 1);			\
		orig_atomic_max(mem, value);			\
		MVEE_POSTOP();							\
	})

#define atomic_decrement_if_positive(mem)					\
	({														\
		__typeof(*mem) __result;							\
		MVEE_PREOP(ATOMIC_DECREMENT_IF_POSITIVE, mem, 1);	\
		__result = orig_atomic_decrement_if_positive(mem);	\
		MVEE_POSTOP();										\
		__result;											\
	})

#define atomic_and_val(mem, mask)							\
	({														\
		__typeof(*mem) __result;							\
		MVEE_PREOP(ATOMIC_AND_VAL, mem, 1);					\
		__result = orig_atomic_and_val(mem, mask);				\
		MVEE_POSTOP();										\
		__result;											\
	})

#define atomic_or_val(mem, mask)							\
	({														\
		__typeof(*mem) __result;							\
		MVEE_PREOP(ATOMIC_OR_VAL, mem, 1);					\
		__result = orig_atomic_or_val(mem, mask);					\
		MVEE_POSTOP();										\
		__result;											\
	})

#define atomic_forced_read(x)						\
	({												\
		typeof(x) ____result;						\
		MVEE_PREOP(ATOMIC_FORCED_READ, &x, 0);		\
		____result = orig_atomic_forced_read(x);	\
		MVEE_POSTOP();								\
		____result;									\
	})

#define catomic_compare_and_exchange_val_acq(mem, newval, oldval)		\
	({																	\
		typeof(*mem) ____result;										\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_VAL, mem, 1);			\
		____result = orig_catomic_compare_and_exchange_val_acq(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define catomic_compare_and_exchange_val_rel(mem, newval, oldval)		\
	({																	\
		typeof(*mem) ____result;										\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_VAL, mem, 1);			\
		____result = orig_catomic_compare_and_exchange_val_rel(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define catomic_compare_and_exchange_bool_acq(mem, newval, oldval)		\
	({																	\
		bool ____result;												\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_BOOL, mem, 1);			\
		____result = orig_catomic_compare_and_exchange_bool_acq(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define catomic_compare_and_exchange_bool_rel(mem, newval, oldval)		\
	({																	\
		bool ____result;												\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_BOOL, mem, 1);			\
		____result = orig_catomic_compare_and_exchange_bool_rel(mem, newval, oldval); \
		MVEE_POSTOP();													\
		____result;														\
	})

#define catomic_exchange_and_add(mem, value)					\
	({															\
		typeof(*mem) ____result;								\
		MVEE_PREOP(CATOMIC_EXCHANGE_AND_ADD, mem, 1);			\
		____result = orig_catomic_exchange_and_add(mem, value);	\
		MVEE_POSTOP();											\
		____result;												\
	})

#define catomic_add(mem, value)					\
	({											\
		MVEE_PREOP(CATOMIC_ADD, mem, 1);		\
		orig_catomic_add(mem, value);			\
		MVEE_POSTOP();							\
	})

#define catomic_increment(mem)					\
	({											\
		MVEE_PREOP(CATOMIC_INCREMENT, mem, 1);	\
		orig_catomic_increment(mem);			\
		MVEE_POSTOP();							\
	})

#define catomic_increment_val(mem)						\
	({													\
		typeof(*mem) ____result;						\
		MVEE_PREOP(CATOMIC_INCREMENT, mem, 1);			\
		____result = orig_catomic_increment_val(mem);	\
		MVEE_POSTOP();									\
		____result;										\
	})

#define catomic_decrement(mem)					\
	({											\
		MVEE_PREOP(CATOMIC_DECREMENT, mem, 1);	\
		orig_catomic_decrement(mem);			\
		MVEE_POSTOP();							\
	})

#define catomic_decrement_val(mem)						\
	({													\
		typeof(*mem) ____result;						\
		MVEE_PREOP(CATOMIC_DECREMENT, mem, 1);			\
		____result = orig_catomic_decrement_val(mem);	\
		MVEE_POSTOP();									\
		____result;										\
	})


#define catomic_and(mem, mask)					\
	({											\
		MVEE_PREOP(CATOMIC_AND, mem, 1);		\
		orig_catomic_and(mem, mask);			\
		MVEE_POSTOP();							\
	})

#define catomic_or(mem, mask)					\
	({											\
		MVEE_PREOP(CATOMIC_OR, mem, 1);			\
		orig_catomic_or(mem, mask);				\
		MVEE_POSTOP();							\
	})

#define catomic_max(mem, value)					\
	({											\
		MVEE_PREOP(CATOMIC_MAX, mem, 1);		\
		orig_catomic_max(mem, value);			\
		MVEE_POSTOP();							\
	})


//
// generic C11-style atomics (include/atomic.h)
//
#define atomic_load_relaxed(mem)					\
	({												\
		__typeof(*mem) ____result;					\
		MVEE_PREOP(ATOMIC_LOAD, mem, 0);			\
		____result = orig_atomic_load_relaxed(mem);	\
		MVEE_POSTOP();								\
		____result;									\
	})

#define atomic_load_acquire(mem)					\
	({												\
		__typeof(*mem) ____result;					\
		MVEE_PREOP(ATOMIC_LOAD, mem, 0);			\
		____result = orig_atomic_load_acquire(mem);	\
		MVEE_POSTOP();								\
		____result;									\
	})

#define atomic_store_relaxed(mem, val)			\
	(void)({									\
		MVEE_PREOP(ATOMIC_STORE, mem, 1);		\
		orig_atomic_store_relaxed(mem, val);	\
		MVEE_POSTOP();							\
	})

#define atomic_store_release(mem, val)			\
	(void)({									\
		MVEE_PREOP(ATOMIC_STORE, mem, 1);		\
		orig_atomic_store_release(mem, val);	\
		MVEE_POSTOP();							\
	})

#define atomic_compare_exchange_weak_relaxed(mem, expected, desired)	\
	({																	\
		bool __result;													\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_BOOL, mem, 1);			\
		__result = orig_atomic_compare_exchange_weak_relaxed(mem, expected, desired); \
		MVEE_POSTOP();													\
		__result;														\
	})

#define atomic_compare_exchange_weak_acquire(mem, expected, desired)	\
	({																	\
		bool __result;													\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_BOOL, mem, 1);			\
		__result = orig_atomic_compare_exchange_weak_acquire(mem, expected, desired); \
		MVEE_POSTOP();													\
		__result;														\
	})

#define atomic_compare_exchange_weak_release(mem, expected, desired)	\
	({																	\
		bool __result;													\
		MVEE_PREOP(ATOMIC_COMPARE_AND_EXCHANGE_BOOL, mem, 1);			\
		__result = orig_atomic_compare_exchange_weak_release(mem, expected, desired); \
		MVEE_POSTOP();													\
		__result;														\
	})

#define atomic_exchange_relaxed(mem, desired)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_EXCHANGE, mem, 1);						\
		____result = orig_atomic_exchange_relaxed(mem, desired);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_exchange_acquire(mem, desired)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_EXCHANGE, mem, 1);						\
		____result = orig_atomic_exchange_acquire(mem, desired);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_exchange_release(mem, desired)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_EXCHANGE, mem, 1);						\
		____result = orig_atomic_exchange_release(mem, desired);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_fetch_add_relaxed(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_ADD, mem, 1);						\
		____result = orig_atomic_fetch_add_relaxed(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_fetch_add_acquire(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_ADD, mem, 1);						\
		____result = orig_atomic_fetch_add_acquire(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_fetch_add_release(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_ADD, mem, 1);						\
		____result = orig_atomic_fetch_add_release(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_fetch_add_acq_rel(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_ADD, mem, 1);						\
		____result = orig_atomic_fetch_add_acq_rel(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_fetch_and_relaxed(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_AND, mem, 1);						\
		____result = orig_atomic_fetch_and_relaxed(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_fetch_and_acquire(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_AND, mem, 1);						\
		____result = orig_atomic_fetch_and_acquire(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_fetch_and_release(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_AND, mem, 1);						\
		____result = orig_atomic_fetch_and_release(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})


#define atomic_fetch_or_relaxed(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_OR, mem, 1);						\
		____result = orig_atomic_fetch_or_relaxed(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_fetch_or_acquire(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_OR, mem, 1);						\
		____result = orig_atomic_fetch_or_acquire(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_fetch_or_release(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_OR, mem, 1);						\
		____result = orig_atomic_fetch_or_release(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})

#define atomic_fetch_xor_release(mem, operand)						\
	({																\
		typeof(*mem) ____result;									\
		MVEE_PREOP(ATOMIC_FETCH_XOR, mem, 1);						\
		____result = orig_atomic_fetch_xor_release(mem, operand);	\
		MVEE_POSTOP();												\
		____result;													\
	})

//
// MVEE additions
//
#define THREAD_ATOMIC_GETMEM(descr, member)			\
	({												\
		__typeof(descr->member) ____result;			\
		MVEE_PREOP(ATOMIC_LOAD, &descr->member, 1);	\
		____result = THREAD_GETMEM(descr, member);	\
		MVEE_POSTOP();								\
		____result;									\
	})

#define THREAD_ATOMIC_SETMEM(descr, member, val)			\
	(void)({												\
			MVEE_PREOP(ATOMIC_STORE, &descr->member, 1);	\
			THREAD_SETMEM(descr, member, val);				\
			MVEE_POSTOP();									\
		})

//
// sys_futex with FUTEX_WAKE_OP usually overwrites the value of the futex.
// We have to make sure that we include the futex write in our sync buf ordering
//
#define lll_futex_wake_unlock(futexp, nr_wake, nr_wake2, futexp2, private) \
	({																	\
		INTERNAL_SYSCALL_DECL (__err);									\
		long int __ret;													\
		MVEE_PREOP(___UNKNOWN_LOCK_TYPE___, futexp2, 1);				\
		__ret = INTERNAL_SYSCALL (futex, __err, 6, (futexp),			\
								  __lll_private_flag (FUTEX_WAKE_OP, private), \
								  (nr_wake), (nr_wake2), (futexp2),		\
								  FUTEX_OP_CLEAR_WAKE_IF_GT_ONE);		\
		if (mvee_should_futex_unlock())									\
		{																\
			*futexp2 = 0;												\
		}																\
		MVEE_POSTOP();													\
		INTERNAL_SYSCALL_ERROR_P (__ret, __err);						\
	})

#define arch_cpu_relax() __asm__ __volatile__("mov\tr0,r0\t@ nop\n\t");

#endif // !IS_IN (rtld)
