static __thread int   mvee_master_thread_id         = 0;
static unsigned char  mvee_sync_enabled             = 0;
static unsigned char  mvee_libc_initialized         = 0;
static unsigned char  mvee_master_variant           = 0;
static unsigned char  mvee_buffer_valid             = 0;
static unsigned short mvee_num_childs               = 0;
static unsigned short mvee_child_num                = 0;
#ifdef MVEE_EXTENDED_QUEUE
static unsigned short mvee_op_number                = 1;
#endif
#ifdef MVEE_LOG_EIPS
DEFINE_MVEE_QUEUE(lock, 1);
#else
DEFINE_MVEE_QUEUE(lock, 0);
#endif
static __thread unsigned long  mvee_prev_flush_cnt           = 0;
static __thread unsigned long  mvee_lock_buffer_prev_pos     = 0;
#if defined(MVEE_EXTENDED_QUEUE) && defined(MVEE_CHECK_LOCK_TYPE)
static __thread unsigned long  mvee_original_call_site       = 0;
#endif

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

extern void mvee_infinite_loop(void);

/* MVEE PATCH:
   Checks wether or not all variants got ALIGNMENT aligned heaps from
   the previous mmap request. If some of them have not, ALL variants
   have to bail out and fall back to another heap allocation method.
   This ensures that the variants stay in sync with respect to future mm
   requests.
*/
#define ALIGNMENT 0x4000000

int
mvee_all_heaps_aligned(char* heap, unsigned long alloc_size)
{
  // if we're not running under MVEE control,
  // just check the alignment of the current heap
  if (!mvee_master_thread_id)
  {
      if ((unsigned long)heap & (ALIGNMENT-1))
		  return 0;
      return 1;
  }

  // We ARE running under MVEE control
  // => ask the MVEE to check the alignments
  // of ALL heaps
  return syscall(MVEE_ALL_HEAPS_ALIGNED, heap, ALIGNMENT, alloc_size);
}

/*
 * logs a (truncated) stack into the specified "eip" buffer. This stack is logged for EVERY variant,
 * which greatly facilitates debugging. The MVEE can dump the contents of this buffer very efficiently, 
 * including source lines/info
 * 
 * Do note that __builtin_return_address() can trap (SEGV) if the entire stack contains less than
 * MVEE_MAX_STACK_DEPTH entries. The MVEE will however trap the SEGV and if it comes from this function,
 * the offending instruction will be skipped.
 */
static void __attribute__ ((noinline)) mvee_log_stack(void* eip_buffer, int eip_buffer_slot_size, volatile unsigned int* eip_buffer_pos_ptr, int start_depth)
{
#ifdef MVEE_LOG_EIPS
	int entries_logged = 0;
	int next_entry = start_depth;

	while (entries_logged < MVEE_STACK_DEPTH)
    {
		unsigned long ret_addr = 0;
		switch (next_entry)
		{
			// __builtin_* intrinics need const arguments so unfortunately, we have to do the following...
#define DEF_CASE(x)														\
			case x:														\
				ret_addr = (unsigned long)__builtin_return_address(x);	\
				break;
			DEF_CASE(0);
#if defined(MVEE_EXTENDED_QUEUE) && defined (MVEE_CHECK_LOCK_TYPE)
			case 1:
				ret_addr = (unsigned long)mvee_original_call_site;
				break;
#else
			DEF_CASE(1);
#endif

#if MVEE_STACK_DEPTH > 1
			DEF_CASE(2);
# if MVEE_STACK_DEPTH > 2
			DEF_CASE(3);
#  if MVEE_STACK_DEPTH > 3
			DEF_CASE(4);
#   if MVEE_STACK_DEPTH > 4
			DEF_CASE(5);
#    if MVEE_STACK_DEPTH > 5
			DEF_CASE(6);
#     if MVEE_STACK_DEPTH > 6
			DEF_CASE(7);
#      if MVEE_STACK_DEPTH > 7
			DEF_CASE(8);
#       if MVEE_STACK_DEPTH > 8
			DEF_CASE(9);
#        if MVEE_STACK_DEPTH > 9
			DEF_CASE(10);
#        endif
#       endif
#      endif
#     endif
#    endif
#   endif
#  endif
# endif
#endif
		}
		*(unsigned long*)((unsigned long)eip_buffer + 
						  eip_buffer_slot_size * (*eip_buffer_pos_ptr) + 
						  MVEE_STACK_DEPTH * sizeof(unsigned long) * mvee_child_num + 
						  sizeof(unsigned long) * entries_logged) 
			= ret_addr;
		entries_logged++;
		next_entry++;
    }
#endif
}

static void mvee_check_buffer(void)
{
	register unsigned short tid asm("al") = mvee_master_thread_id;
	if (unlikely(!tid))
    {
		int tmp_id;
		__asm__ volatile("int $0x80" : "=a" (tmp_id) : "0" (MVEE_GET_MASTERTHREAD_ID) : "memory", "cc");
		mvee_master_thread_id = tmp_id;

		if (!mvee_buffer_valid)
		{
			mvee_buffer_valid = 1;
#ifdef MVEE_PARTIAL_ORDER_REPLICATION
			INIT_MVEE_QUEUE(lock, MVEE_LOCK_QUEUE_SLOT_SIZE, MVEE_LIBC_LOCK_BUFFER_PARTIAL);
#else
			INIT_MVEE_QUEUE(lock, MVEE_LOCK_QUEUE_SLOT_SIZE, MVEE_LIBC_LOCK_BUFFER);
#endif
		}
    }
}


// This function is a bit tricky, especially on x86_64!
// In some contexts, such as syscalls that enable asynchronous cancellation,
// libc expects none of the code it executes to touch registers other than
// %rax and %r11. Consequently, we have to make sure that at most 2 registers
// live at any point during our mvee funcs!
static inline int mvee_should_sync(void)
{
	if (unlikely(!mvee_libc_initialized))
	{
		long res = syscall(MVEE_RUNS_UNDER_MVEE_CONTROL, &mvee_sync_enabled, &mvee_infinite_loop, 
						   &mvee_num_childs, &mvee_child_num, &mvee_master_variant);
		if (!(res < 0 && res > -4095))
			mvee_check_buffer();
		mvee_libc_initialized = 1;
	}
	return mvee_sync_enabled;
}


int mvee_should_sync_tid(void)
{
	return mvee_should_sync();
}

// the buffer initialization is NOT thread-safe. Therefore, it must be initialized
// in single threaded context!!!
void mvee_invalidate_buffer(void)
{
	mvee_buffer_valid = 0;
	mvee_master_thread_id = 0;
}

#define cpu_relax() asm volatile("rep; nop" ::: "memory")

#define gcc_barrier() asm volatile("" ::: "memory")

static inline unsigned int mvee_write_lock_result_prepare(void)
{
	while (1)
    {
		if (orig_atomic_decrement_and_test(mvee_lock_buffer_lock))
			return *mvee_lock_buffer_pos;
      
		while (*mvee_lock_buffer_lock <= 0)
			cpu_relax();
    }

	return *mvee_lock_buffer_pos;
}

static inline void mvee_write_lock_result_finish(void)
{
	gcc_barrier();
	orig_atomic_increment(mvee_lock_buffer_pos);
	*mvee_lock_buffer_lock = 1;
}

#ifdef MVEE_EXTENDED_QUEUE
static inline void mvee_write_lock_result_write(unsigned int pos, unsigned short op_type, void* word_ptr, unsigned char is_store)
#else
	static inline void mvee_write_lock_result_write(unsigned int pos, void* word_ptr, unsigned char is_store)
#endif
{
restart:
	if (likely(pos < mvee_lock_buffer_slots))
    {
#if defined(MVEE_LOG_EIPS) && defined(MVEE_EXTENDED_QUEUE)
		MVEE_LOG_STACK(lock, 1, &pos);
#endif
		gcc_barrier();

#ifdef MVEE_PARTIAL_ORDER_REPLICATION
		MVEE_LOG_QUEUE_DATA(lock, pos, 0, (unsigned long)(((unsigned long)word_ptr) | is_store));
# ifdef MVEE_EXTENDED_QUEUE
		MVEE_LOG_QUEUE_DATA(lock, pos, sizeof(long) + sizeof(short), op_type);
# endif
		MVEE_LOG_QUEUE_DATA(lock, pos, sizeof(long), ((unsigned short)mvee_master_thread_id));
#else // MVEE_TOTAL_ORDER_REPLICATION
# ifdef MVEE_EXTENDED_QUEUE
		MVEE_LOG_QUEUE_DATA(lock, pos, sizeof(long), ((unsigned long)word_ptr));
		MVEE_LOG_QUEUE_DATA(lock, pos, sizeof(short), op_type);
# endif
		MVEE_LOG_QUEUE_DATA(lock, pos, 0, ((unsigned short)mvee_master_thread_id));
#endif // !MVEE_PARTIAL_ORDER_REPLICATION
    }
	else
    {      
		// we log the tid of the flushing thread into the last slot
#ifdef MVEE_PARTIAL_ORDER_REPLICATION
		MVEE_LOG_QUEUE_DATA(lock, pos, sizeof(long), ((unsigned short)mvee_master_thread_id));
		syscall(MVEE_FLUSH_SHARED_BUFFER, MVEE_LIBC_LOCK_BUFFER_PARTIAL);
#else
		MVEE_LOG_QUEUE_DATA(lock, pos, 0, ((unsigned short)mvee_master_thread_id));
		syscall(MVEE_FLUSH_SHARED_BUFFER, MVEE_LIBC_LOCK_BUFFER);
#endif
		*mvee_lock_buffer_pos = pos = 0;
		goto restart;
    }
}

static inline unsigned char mvee_op_is_tagged(unsigned long pos)
{
#ifdef MVEE_EXTENDED_QUEUE
	unsigned short tagged;
	MVEE_READ_QUEUE_DATA(lock, pos, sizeof(long) + sizeof(short) * (mvee_child_num + 1), tagged);
#else
	unsigned char tagged;
	MVEE_READ_QUEUE_DATA(lock, pos, sizeof(long) + sizeof(short) + (mvee_child_num - 1), tagged);
#endif
	return tagged ? 1 : 0;
}

static inline unsigned char mvee_pos_still_valid(void)
{
	if (*mvee_lock_buffer_flushing || *mvee_lock_buffer_flush_cnt != mvee_prev_flush_cnt)
		return 0;
	return 1;
}

#ifdef MVEE_EXTENDED_QUEUE
static inline void mvee_read_lock_result_wait(unsigned short op_type, void* word_ptr)
#else
static inline void mvee_read_lock_result_wait(void)
#endif
{
#ifdef MVEE_PARTIAL_ORDER_REPLICATION
	unsigned char is_store = 0;
	unsigned char all_tagged;
	unsigned long master_word_ptr;
	unsigned int i;
	unsigned int orig_pos;
	unsigned int nextpos = 0;
	unsigned int temppos;

	while(true)
	{
		// STEP 1: FIND THE CORRESPONDING MASTER WORD POINTER
		//
		// Our algorithm relies on the fact that all replicae
		// are semantically equivalent.
		//
		// Therefore, we may safely assume that the first non-tagged
		// operation of this thread's corresponding master thread.
		// is an operation on the same logical word.
		//
		// Caching:
		// We start the search at the previously replicated operation 
		// by this thread. The index of the previously replicated
		// operation is stored in mvee_lock_buffer_prev_pos.
		// It remains valid until the queue is flushed.
		// When the queue is flushed, we increment the flush counter
		// which is stored in the upper 4 bytes of mvee_lock_buffer_pos.
		//

		// check if the queue has been flushed since the last operation we've replicated
		if (unlikely(*mvee_lock_buffer_flush_cnt != mvee_prev_flush_cnt))
		{
			// it has been flushed, update flush cnt and reset the 
			// position of our previously replicated operation
			mvee_prev_flush_cnt = *mvee_lock_buffer_flush_cnt;
			mvee_lock_buffer_prev_pos = 0;
			nextpos = 0;
		}

		temppos = *mvee_lock_buffer_pos;

		// Meanwhile, some other thread _MAY_ have moved the position
		// pointer past our previously replicated operation
		//
		// If so, we start the search there instead
		orig_pos = temppos;
		if (temppos < mvee_lock_buffer_prev_pos)
			nextpos = mvee_lock_buffer_prev_pos;
		else if (!nextpos)
			nextpos = orig_pos;

		master_word_ptr = 0;

		for (temppos = nextpos; temppos <= mvee_lock_buffer_slots; ++temppos)
		{
			unsigned short tid;
			MVEE_READ_QUEUE_DATA(lock, temppos, sizeof(long), tid);
			// MVEE_READ_QUEUE_DATA(lock, temppos, 0, tid);

			// no tid => the slaves are running ahead of the master
			// or the master stores are not visible yet
			if (!tid)
			{
				master_word_ptr = 0;
				break;
			}

			if (tid != (unsigned short)mvee_master_thread_id || mvee_op_is_tagged(temppos))
				continue;

			// if tid is visible, then this will be too
			MVEE_READ_QUEUE_DATA(lock, temppos, 0, master_word_ptr);
			if (master_word_ptr & 1)
				is_store = 1;
			master_word_ptr &= ~(sizeof(long) - 1);

			// this will only happen if we're at the end of the queue
			// at which point we log a pseudo-operation with master_word_ptr == 0
			if (!master_word_ptr)
			{
				if (temppos == mvee_lock_buffer_slots)
				{
					while (true)
					{
						all_tagged = 1;

						for (temppos = orig_pos; temppos < mvee_lock_buffer_slots; ++temppos)
						{		    
							if (!mvee_op_is_tagged(temppos))
							{
								all_tagged = 0;
								orig_pos = temppos;
								break;
							}
						}

						if (!all_tagged)
							syscall(__NR_sched_yield);
						else
							break;
					}

					*mvee_lock_buffer_flushing = 1;
					atomic_full_barrier();

					syscall(MVEE_FLUSH_SHARED_BUFFER, MVEE_LIBC_LOCK_BUFFER_PARTIAL);

#ifdef MVEE_EXTENDED_QUEUE
					mvee_op_number = 1;
#endif

					*mvee_lock_buffer_pos = 0;
					atomic_full_barrier();
					*mvee_lock_buffer_flush_cnt = (++mvee_prev_flush_cnt);
					*mvee_lock_buffer_flushing = 0;

					temppos = mvee_lock_buffer_prev_pos = 0;
					break;
				}

				// we could also see a NULLed out master_word_ptr if another thread is flushing
				temppos = mvee_lock_buffer_prev_pos = 0;
				break;
			}

			// a weird corner case could happen here.
			// it is possible that by the time we get here, some other thread has flushed and the master
			// thread has caught up with us again. In other words,
			// we MIGHT have missed a complete flush cycle
			//
			// => we need to check if our data is still valid!!!
			if (!mvee_pos_still_valid())
			{
				master_word_ptr = 0;
				break;
			}

#if defined(MVEE_EXTENDED_QUEUE) && defined(MVEE_CHECK_LOCK_TYPE)
			unsigned short master_op_type;
			MVEE_READ_QUEUE_DATA(lock, temppos, sizeof(long) + sizeof(short), master_op_type);

			if (master_word_ptr != ((unsigned long)word_ptr & ~(sizeof(long) - 1)) && mvee_pos_still_valid())
			{
#  ifdef MVEE_LOG_EIPS
				MVEE_LOG_STACK(lock, 1, &temppos);
#  endif
				syscall(__NR_gettid, 1337, 10000001, 61, word_ptr, temppos);
			}

			if (master_op_type != op_type && mvee_pos_still_valid())
			{
#  ifdef MVEE_LOG_EIPS
				MVEE_LOG_STACK(lock, 1, &temppos);
#  endif

				syscall(__NR_gettid, 1337, 10000001, 60, mvee_lock_buffer_slot_size, temppos);
				syscall(__NR_gettid, 1337, 10000001, 59, master_op_type, op_type);
				return;
			}

#endif

			break;
		}

		if (master_word_ptr)
			break;

		// if we get to this point, it means that the slave 
		// has caught up with the master
		// => we restart the iteration but this time
		// we start at the position we were at
		nextpos = temppos;
		syscall(__NR_sched_yield);
	}

	// STEP 2: FIND PRECEDING OPERATIONS ON THIS LOCATION
	// 
	// Rules:
	// * Stores can continue if all preceding loads and stores
	// have been replicated
	// * Loads can continue if all preceding stores 
	// have been replicated
	//
	unsigned char seen_preceding_op = 1;
	while (seen_preceding_op)
    {
		seen_preceding_op = 0;
		for (i = orig_pos; i < temppos; ++i)
		{
			unsigned long temp_word_ptr;
			MVEE_READ_QUEUE_DATA(lock, i, 0, temp_word_ptr);

			// check if the results are being logged out of order
			if (!temp_word_ptr)
			{
				seen_preceding_op = 1;
				orig_pos = i;
				break;
			}

			if ((temp_word_ptr & ~(sizeof(long) - 1)) == master_word_ptr)
			{
				if (is_store || (temp_word_ptr & (sizeof(long) - 1)))
				{
					if (!mvee_op_is_tagged(i))
					{
						seen_preceding_op = 1;
						orig_pos = i;
						break;
					}
				}
			}
		}

		// We haven't seen a preceding operation that must be completed
		// before this one
		if (!seen_preceding_op)
		{
			mvee_lock_buffer_prev_pos = temppos;
			break;
		}

		syscall(__NR_sched_yield);
    } 
 
#ifdef MVEE_LOG_EIPS
	MVEE_LOG_STACK(lock, 1, &temppos);
#endif

#else

	while (true)
    {
		int temppos = *(volatile int*)mvee_lock_buffer_pos;
      
		if (temppos < mvee_lock_buffer_slots)
		{
			unsigned short tid, type;

			MVEE_READ_QUEUE_DATA(lock, temppos, 0, tid);
			if (tid == (unsigned short)mvee_master_thread_id)
			{
# if defined(MVEE_EXTENDED_QUEUE) && defined(MVEE_CHECK_LOCK_TYPE)
	      
				MVEE_READ_QUEUE_DATA(lock, temppos, sizeof(short), type);

				if (type != op_type)
				{
#  ifdef MVEE_LOG_EIPS
					MVEE_LOG_STACK(lock, 1, &temppos);
#  endif
					syscall(__NR_gettid, 1337, 10000001, 60, mvee_lock_buffer_slot_size, temppos);
					syscall(__NR_gettid, 1337, 10000001, 59, type, op_type);
				}
# endif
				break;
			}

			syscall(__NR_sched_yield);
		}
		else
		{
			// we have to flush... figure out which thread does the flush
			unsigned short tid;
			MVEE_READ_QUEUE_DATA(lock, temppos, 0, tid);

			if (tid == (unsigned short)mvee_master_thread_id)
			{
				// we should do it...
				syscall(MVEE_FLUSH_SHARED_BUFFER, MVEE_LIBC_LOCK_BUFFER);
				*mvee_lock_buffer_pos = 0;
			}
			else if (!tid)
			{
				// we don't know who should do it yet. wait for more info
				while (temppos >= mvee_lock_buffer_slots && 
					   !tid)
				{
					cpu_relax();
					temppos = *(volatile int*)mvee_lock_buffer_pos;
					MVEE_READ_QUEUE_DATA(lock, temppos, 0, tid);					
				}
			}
			else
			{
				// some other thread should do it. wait for the flush
				while (*mvee_lock_buffer_pos >= mvee_lock_buffer_slots)
					syscall(__NR_sched_yield);
			}
		}
    }
#endif
}

static inline void mvee_read_lock_result_wake(void)
{
#ifdef MVEE_EXTENDED_QUEUE
	int pos;

# ifdef MVEE_PARTIAL_ORDER_REPLICATION
	pos = mvee_lock_buffer_prev_pos;
# else
	pos = *mvee_lock_buffer_pos;
# endif

# ifdef MVEE_LOG_EIPS
//	MVEE_LOG_STACK(lock, 1, &pos);
# endif

	// check call site, should be ASLR proof
# ifdef MVEE_CHECK_LOCK_TYPE
#  ifdef MVEE_LOG_EIPS
	unsigned long parent_eip = *(unsigned long*)((unsigned long)mvee_lock_eip_buffer + sizeof(unsigned long)*MVEE_STACK_DEPTH*mvee_num_childs * pos);
	unsigned long our_eip = (unsigned long)mvee_original_call_site;

	//  if (parent_eip != our_eip)
	if ((parent_eip & 0xfff) != (our_eip & 0xfff))
#  endif
    {
		unsigned short lock_type;
#  ifdef MVEE_PARTIAL_ORDER_REPLICATION
		MVEE_READ_QUEUE_DATA(lock, pos, sizeof(long) + sizeof(short), lock_type);
#  else
		MVEE_READ_QUEUE_DATA(lock, pos, sizeof(short), lock_type);
#  endif
#  ifdef MVEE_LOG_EIPS
		syscall(__NR_gettid, 1337, 10000001, 90, lock_type, parent_eip, our_eip);
#  endif
    }
# endif // !MVEE_CHECK_LOCK_TYPE

#endif // !MVEE_EXTENDED_QUEUE

#ifdef MVEE_PARTIAL_ORDER_REPLICATION
	// see if we can move the position pointer
	unsigned int orig_pos = *mvee_lock_buffer_pos;
	if (!mvee_pos_still_valid())
		return;

	for (unsigned int i = orig_pos; i < mvee_lock_buffer_slots; ++i)
    {
		if (!mvee_op_is_tagged(i) && i != mvee_lock_buffer_prev_pos)
		{
			// if it looks like we're able to move the position pointer, 
			// attempt to do so with a CAS. Again, keep in mind that this
			// operation might happen at the same time as a flush from another thread
			// => check if the original pos pointer is still in place
			// If not, another thread has either increased the pos or has flushed => bail out
			// If yes, the flushing thread will just need an extra iteration to flush
			//
			// we cannot use a regular store here since that might cause us to not flush!
			// i.e. some thread might move us to the last position, which will cause the next atomic op to flush first
			// but some other thread might move use back!
			if (i > orig_pos)
				__sync_bool_compare_and_swap(mvee_lock_buffer_pos, orig_pos, i);
			break;
		}
    }

	// now tag the slot
  
# ifdef MVEE_EXTENDED_QUEUE
	//  unsigned short tag = __sync_fetch_and_add(&mvee_op_number, 1);
	unsigned short tag = 1;
	MVEE_LOG_QUEUE_DATA(lock, mvee_lock_buffer_prev_pos, sizeof(long) + sizeof(short) * (mvee_child_num + 1), tag);
# else  
	unsigned char tag = 1;
	MVEE_LOG_QUEUE_DATA(lock, mvee_lock_buffer_prev_pos, sizeof(long) + sizeof(short) + (mvee_child_num - 1), tag);
# endif

#else // MVEE_TOTAL_ORDER_REPLICATION
    (*mvee_lock_buffer_pos)++;
#endif // !MVEE_PARTIAL_ORDER_REPLICATION
}

#ifdef MVEE_EXTENDED_QUEUE
unsigned char mvee_atomic_preop_internal(unsigned char is_store, void* word_ptr, unsigned short op_type)
#else
	unsigned char mvee_atomic_preop_internal(unsigned char is_store, void* word_ptr)
#endif
{
	if (unlikely(!mvee_should_sync()))
		return 0;
	mvee_check_buffer();
#if defined(MVEE_EXTENDED_QUEUE) && defined(MVEE_CHECK_LOCK_TYPE)
	if (!mvee_original_call_site)
		mvee_original_call_site = (unsigned long)__builtin_return_address(0);
#endif
	if (likely(mvee_master_variant))
    {
		unsigned int pos = mvee_write_lock_result_prepare();
#ifdef MVEE_EXTENDED_QUEUE
		mvee_write_lock_result_write(pos, op_type, word_ptr, is_store);
#else
		mvee_write_lock_result_write(pos, word_ptr, is_store);
#endif
		return 1;
    }
	else
    {
#ifdef MVEE_EXTENDED_QUEUE
		mvee_read_lock_result_wait(op_type, word_ptr);
#else
		mvee_read_lock_result_wait();
#endif
		return 2;
    }
}

void mvee_atomic_postop_internal(unsigned char preop_result)
{
	if(likely(preop_result) == 1)
		mvee_write_lock_result_finish();
	else if (likely(preop_result) == 2)
		mvee_read_lock_result_wake();
#if defined(MVEE_EXTENDED_QUEUE) && defined(MVEE_CHECK_LOCK_TYPE)		
	mvee_original_call_site = 0;
#endif   
}

unsigned char mvee_atomic_preop(unsigned short op_type, void* word_ptr)
{
#ifdef MVEE_EXTENDED_QUEUE
# ifdef MVEE_CHECK_LOCK_TYPE
	mvee_original_call_site = (unsigned long)__builtin_return_address(0);
# endif
	return mvee_atomic_preop_internal(op_type > mvee_atomic_load ? 1 : 0, word_ptr, op_type + __MVEE_BASE_ATOMICS_MAX__);
#else
	return mvee_atomic_preop_internal(op_type > mvee_atomic_load ? 1 : 0, word_ptr);
#endif
}

void mvee_atomic_postop(unsigned char preop_result)
{
	mvee_atomic_postop_internal(preop_result);
}

void mvee_xcheck(unsigned long item)
{
	unsigned char tmp = mvee_atomic_preop(ATOMIC_STORE, (void*) item);
	mvee_atomic_postop(tmp);
}
