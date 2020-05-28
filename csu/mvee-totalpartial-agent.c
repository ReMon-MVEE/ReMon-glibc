static volatile unsigned int          mvee_lock_owner               = 0;
static unsigned char                  mvee_sync_enabled             = 0;
static unsigned char                  mvee_libc_initialized         = 0;
static unsigned char                  mvee_master_variant           = 0;
static unsigned char                  mvee_buffer_valid             = 0;
static unsigned short                 mvee_num_variants             = 0;
static unsigned short                 mvee_my_variant_num           = 0;
static struct mvee_buffer_info*       mvee_lock_buffer_info         = NULL;
static struct mvee_buffer_entry*      mvee_lock_buffer              = NULL;
static struct mvee_callstack_entry*   mvee_callstack_buffer         = NULL;
static __thread unsigned int          mvee_master_thread_id         = 0;
static __thread unsigned long         mvee_prev_flush_cnt           = 0;
static __thread unsigned long         mvee_lock_buffer_prev_pos     = 0;
#ifdef MVEE_CHECK_LOCK_TYPE
static __thread unsigned long         mvee_original_call_site       = 0;
#define INLINEIFNODEBUG __attribute__((noinline))
#else
#define INLINEIFNODEBUG inline
#endif


#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

extern void mvee_infinite_loop(void);

// ========================================================================================================================
// INITIALIZATION FUNCS
// ========================================================================================================================

static void mvee_check_buffer(void)
{
	if (unlikely(!mvee_master_thread_id))
    {
		mvee_master_thread_id = syscall(MVEE_GET_MASTERTHREAD_ID);

		if (!mvee_buffer_valid)
		{
			mvee_buffer_valid = 1;

#ifdef MVEE_PARTIAL_ORDER_REPLICATION
			unsigned long queue_ident = MVEE_LIBC_LOCK_BUFFER_PARTIAL;
#else
			unsigned long queue_ident = MVEE_LIBC_LOCK_BUFFER;
#endif

			// ask the MVEE to allocate the lock buffer
			unsigned long slots = 0;
			long tmp_id = syscall(MVEE_GET_SHARED_BUFFER, 0, queue_ident, &slots, sizeof(struct mvee_buffer_entry));

			// we use some of the slots for buffer_info entries
			slots       = (slots - mvee_num_variants * 64) / sizeof(struct mvee_buffer_entry) - 2;
			
			// Attach to the buffer
			void* tmp_buffer      = (void*)syscall(__NR_shmat, tmp_id, NULL, 0);
			mvee_lock_buffer_info = ((struct mvee_buffer_info*) tmp_buffer) + mvee_my_variant_num;
			mvee_lock_buffer      = ((struct mvee_buffer_entry*) tmp_buffer) + mvee_num_variants;
			mvee_lock_buffer_info->lock = 1;			
			mvee_lock_buffer_info->size = slots;
			mvee_lock_buffer_info->buffer_type = queue_ident;

#ifdef MVEE_LOG_EIPS			
			long callstack_buffer_id = syscall(MVEE_GET_SHARED_BUFFER, 1, queue_ident, NULL,
											   mvee_num_variants * sizeof(struct mvee_callstack_entry),
											   MVEE_STACK_DEPTH);
			mvee_callstack_buffer = (struct mvee_callstack_entry*) syscall(__NR_shmat, callstack_buffer_id, NULL, 0);
#endif
		}
    }
}


static INLINEIFNODEBUG int mvee_should_sync(void)
{
	if (unlikely(!mvee_libc_initialized))
	{
		long res = syscall(MVEE_RUNS_UNDER_MVEE_CONTROL, &mvee_sync_enabled, &mvee_infinite_loop, 
						   &mvee_num_variants, &mvee_my_variant_num, &mvee_master_variant);
		if (!(res < 0 && res > -4095))
			mvee_check_buffer();
		mvee_libc_initialized = 1;
	}
	return mvee_sync_enabled;
}

#define cpu_relax() asm volatile("rep; nop" ::: "memory")

#define gcc_barrier() asm volatile("" ::: "memory")

/*
 * logs a (truncated) stack into the specified "eip" buffer. This stack is logged for EVERY variant,
 * which greatly facilitates debugging. The MVEE can dump the contents of this buffer very efficiently, 
 * including source lines/info
 * 
 * Do note that __builtin_return_address() can trap (SEGV) if the entire stack contains less than
 * MVEE_MAX_STACK_DEPTH entries. The MVEE will however trap the SEGV and if it comes from this function,
 * the offending instruction will be skipped.
 */
static void __attribute__ ((noinline)) mvee_log_stack(unsigned int pos, int start_depth)
{
#ifdef MVEE_LOG_EIPS
	int entries_logged = 0;
	int next_entry = start_depth;

	// make sure that word_ptr and op_type are visible before we log the stack
	__sync_synchronize();

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
#ifdef MVEE_CHECK_LOCK_TYPE
			case 1:
				ret_addr = (unsigned long)mvee_original_call_site;
				break;
#else
			DEF_CASE(1);
#endif

			DEF_CASE(2);
			DEF_CASE(3);
			DEF_CASE(4);
			DEF_CASE(5);
			DEF_CASE(6);
			DEF_CASE(7);
			DEF_CASE(8);
			DEF_CASE(9);
			DEF_CASE(10);
		}
		
		mvee_callstack_buffer[pos * mvee_num_variants + mvee_my_variant_num].callee[entries_logged++] = ret_addr;
		next_entry++;
    }
#endif
}

// ========================================================================================================================
// ASSERTIONS
// ========================================================================================================================

static INLINEIFNODEBUG void mvee_assert_lock_not_owned(void)
{
#ifdef MVEE_CHECK_LOCK_TYPE
	if (mvee_lock_owner)
		*(volatile long*)0 = mvee_lock_owner;
#endif
}

static INLINEIFNODEBUG void mvee_assert_lock_owned(void)
{
#ifdef MVEE_CHECK_LOCK_TYPE
	if (!mvee_lock_owner || 
		mvee_lock_owner != mvee_master_thread_id)
	{
		*(volatile long*)0 = mvee_lock_owner;
	}
#endif
}

static INLINEIFNODEBUG void mvee_assert_operation_matches
(
	unsigned int pos, 
	unsigned long slave_word_ptr, 
	unsigned short slave_op_type
)
{
#ifdef MVEE_CHECK_LOCK_TYPE
	unsigned long master_word_ptr = mvee_lock_buffer[pos].word_ptr;
	unsigned short master_op_type = mvee_lock_buffer[pos].operation_type;

	if ((master_word_ptr & 0xfff) != (slave_word_ptr & 0xfff))
	{
		mvee_log_stack(pos, 1);
		syscall(__NR_gettid, 1337, 10000001, 61, slave_word_ptr, pos);
	}
	
	if (master_op_type != slave_op_type)
	{
		mvee_log_stack(pos, 1);
		syscall(__NR_gettid, 1337, 10000001, 60, 0, pos);
		syscall(__NR_gettid, 1337, 10000001, 59, master_op_type, slave_op_type);
	}
#endif
}

static INLINEIFNODEBUG void mvee_assert_at_end_of_buffer(unsigned int pos)
{
	if (pos != mvee_lock_buffer_info->size)
		*(volatile int*) 0 = 0x0bad1dea;
}

static INLINEIFNODEBUG void mvee_assert_same_callee(unsigned int pos)
{
// check call site, should be ASLR proof
#if defined(MVEE_CHECK_LOCK_TYPE) && defined(MVEE_LOG_EIPS)
	unsigned long parent_eip = mvee_callstack_buffer[pos * mvee_num_variants + mvee_my_variant_num].callee[0];
	unsigned long our_eip = (unsigned long)mvee_original_call_site;

	if ((parent_eip & 0xfff) != (our_eip & 0xfff))
		syscall(__NR_gettid, 1337, 10000001, 90, mvee_lock_buffer[pos].operation_type, parent_eip, our_eip);
#endif
}

// ========================================================================================================================
// MASTER LOGIC
// ========================================================================================================================

static INLINEIFNODEBUG unsigned int mvee_write_lock_result_prepare(void)
{
	while (1)
    {
		if (orig_atomic_decrement_and_test(&mvee_lock_buffer_info->lock))
		{
			mvee_assert_lock_not_owned();
			mvee_lock_owner = mvee_master_thread_id;
			return mvee_lock_buffer_info->pos;
		}
      
		while (mvee_lock_buffer_info->lock <= 0)
			cpu_relax();
    }

	// unreachable
	return mvee_lock_buffer_info->pos;
}

static INLINEIFNODEBUG void mvee_write_lock_result_finish(void)
{
	mvee_assert_lock_owned();
	mvee_lock_owner = 0;
	orig_atomic_increment(&mvee_lock_buffer_info->pos);
	mvee_lock_buffer_info->lock = 1;
}

static INLINEIFNODEBUG void mvee_lock_buffer_flush(void)
{
	mvee_lock_buffer_info->flushing = 1;
	atomic_full_barrier();

	syscall(MVEE_FLUSH_SHARED_BUFFER, mvee_lock_buffer_info->buffer_type);
	mvee_lock_buffer_info->pos = 0;
	atomic_full_barrier();

	mvee_lock_buffer_info->flush_cnt = (++mvee_prev_flush_cnt);
	mvee_lock_buffer_info->flushing = 0;
}

static INLINEIFNODEBUG void mvee_write_lock_result_write(unsigned int pos, unsigned short op_type, void* word_ptr)
{
	while (1)
	{
		if (likely(pos < mvee_lock_buffer_info->size))
		{
			mvee_lock_buffer[pos].word_ptr = (unsigned long) word_ptr;
			mvee_lock_buffer[pos].operation_type = op_type;

#ifdef MVEE_CHECK_LOCK_TYPE
			if (mvee_lock_owner != mvee_master_thread_id)
				*(volatile long*)0 = mvee_lock_owner;
#endif

			mvee_log_stack(pos, 1);

			// This must be stored last. The slave assumes that when
			// master_thread_id becomes non-zero, the word_ptr and operation_type
			// fields are valid too
			mvee_lock_buffer[pos].master_thread_id = mvee_master_thread_id;
			break;
		}
		else
		{      
			// we log the tid of the flushing thread into the last slot
			mvee_lock_buffer[pos].master_thread_id = mvee_master_thread_id;
			mvee_lock_buffer_flush();
			pos = 0;
		}
	}
}

// ========================================================================================================================
// SLAVE LOGIC
// ========================================================================================================================

static INLINEIFNODEBUG unsigned char mvee_op_is_tagged(unsigned long pos)
{
	return mvee_lock_buffer[pos].tags[mvee_my_variant_num];
}

static INLINEIFNODEBUG unsigned char mvee_pos_still_valid(void)
{
	if (mvee_lock_buffer_info->flushing || 
		mvee_lock_buffer_info->flush_cnt != mvee_prev_flush_cnt)
		return 0;
	return 1;
}

//
// Wait until all relevant replication operations in the range [start_pos, end_pos[
// have been tagged.
// 
// if @wait_for_all_ops == 1, this waits until _ALL_ operations get tagged
// if @wait_for_all_ops == 0, this waits until all operations on the same 
// current_word_ptr get tagged
//
static INLINEIFNODEBUG void mvee_wait_for_preceding_ops
(
	unsigned int start_pos, 
	unsigned int end_pos, 
	unsigned char wait_for_all_ops, 
	unsigned long current_word_ptr
)
{
	unsigned char all_tagged;
	unsigned int temppos;

	while (true)
	{
		all_tagged = 1;

		for (temppos = start_pos; temppos < end_pos; ++temppos)
		{		    
			unsigned long word_ptr = mvee_lock_buffer[temppos].word_ptr;

			if (!word_ptr ||
				(!mvee_op_is_tagged(temppos) && (wait_for_all_ops || word_ptr == current_word_ptr)))
			{
				all_tagged = 0;
				start_pos = temppos;
				break;
			}
		}

		if (all_tagged)
		{
			mvee_lock_buffer_prev_pos = temppos;
			break;
		}
		
		syscall(__NR_sched_yield);
	}
}

static INLINEIFNODEBUG void mvee_read_lock_result_wait(unsigned short op_type, void* word_ptr)
{
#ifdef MVEE_PARTIAL_ORDER_REPLICATION
	unsigned long master_word_ptr;
	unsigned int start_pos = 0;
	unsigned int current_pos = mvee_lock_buffer_info->pos;

	if (mvee_lock_buffer_prev_pos > current_pos)
		start_pos = mvee_lock_buffer_prev_pos;
	else
		start_pos = current_pos;

	while(true)
	{
		// STEP 1: FIND THE CORRESPONDING MASTER WORD POINTER
		//
		// Our algorithm relies on the fact that all variants are semantically
		// equivalent. If the first non-replicated operation we find is an
		// operation on address &A, then we know that address &A in the master
		// is equivalent to address <word_ptr> in the slave.
		//
		// Caching: We start the search one item beyond the previously
		// replicated operation by this thread. The index of the previously
		// replicated operation is stored in mvee_lock_buffer_prev_pos.  It
		// remains valid until the queue is flushed.  When the queue is flushed,
		// we increment the flush counter.
		//

		// check if the queue has been flushed since the last operation we've replicated
		while (unlikely(!mvee_pos_still_valid()))
		{
			// it has been flushed, update flush cnt and reset the 
			// position of our previously replicated operation
			mvee_prev_flush_cnt = mvee_lock_buffer_info->flush_cnt;
			mvee_lock_buffer_prev_pos = start_pos = 0;
			syscall(__NR_sched_yield);
		}

		master_word_ptr = 0;

		for (current_pos = start_pos; current_pos <= mvee_lock_buffer_info->size; ++current_pos)
		{
			unsigned int tid = mvee_lock_buffer[current_pos].master_thread_id;

			// no tid => the slaves are running ahead of the master
			// or the master stores are not visible yet
			if (!tid)
			{
				break;
			}
			else if (tid != mvee_master_thread_id || 
					 mvee_op_is_tagged(current_pos))
			{
				continue;
			}

			// if tid is visible, then this will be too
			master_word_ptr = mvee_lock_buffer[current_pos].word_ptr;

			if (mvee_pos_still_valid())
			{				
				// this will only happen if we're at the end of the queue
				// at which point we log a pseudo-operation with master_word_ptr == 0
				if (!master_word_ptr)
				{
					// assert that we're at the end of the buffer			   
					mvee_assert_at_end_of_buffer(current_pos);
					// wait for everything before us to complete
					mvee_wait_for_preceding_ops(start_pos, mvee_lock_buffer_info->size + 1, 1, 0);					
					mvee_lock_buffer_flush();
					current_pos = mvee_lock_buffer_prev_pos = 0;
					break;
				}
				else
				{					
					// We found a non-replicated operation for our thread at position
					// <current_pos>. If the buffer has not been flushed between the moment
					// we determined the start position of our search and the moment
					// we found the non-replicated operation, we can now safely assume that
					// it is not going to be flushed.
					mvee_assert_operation_matches(current_pos, (unsigned long) word_ptr, op_type);
					break;
				}
			}
			else
			{
				// Some other thread flushed the buffer while we were searching for the
				// next non-replicated position.
				master_word_ptr = 0;
				current_pos = 0;
				break;
			}
		}

		if (master_word_ptr)
			break;

		// if we get to this point, it means that the slave 
		// has caught up with the master
		// => we restart the iteration but this time
		// we start at the position we were at
		start_pos = current_pos;
		syscall(__NR_sched_yield);
	}

	// STEP 2: FIND PRECEDING OPERATIONS ON THIS LOCATION
	mvee_wait_for_preceding_ops(mvee_lock_buffer_info->pos, current_pos, 0, master_word_ptr);
	mvee_log_stack(current_pos, 1);

#else // MVEE_TOTAL_ORDER_REPLICATION

	// Super simple compared to the partial order agent. Just wait until
	// the operation poited to by buffer_info->pos is for our thread...
	while (true)
    {
		int current_pos = mvee_lock_buffer_info->pos;
      
		if (current_pos < mvee_lock_buffer_info->size)
		{
			if (mvee_lock_buffer[current_pos].master_thread_id == mvee_master_thread_id)
			{
				mvee_assert_operation_matches(current_pos, (unsigned long)word_ptr, op_type);
				mvee_log_stack(current_pos, 1);
				break;
			}

			syscall(__NR_sched_yield);
		}
		else
		{
			unsigned int tid = mvee_lock_buffer[current_pos].master_thread_id;

			// we have to flush... figure out which thread does the flush
			if (tid == mvee_master_thread_id)
			{
				mvee_lock_buffer_flush();
			}
			else
			{
				while (mvee_lock_buffer_info->pos == mvee_lock_buffer_info->size &&
					   mvee_lock_buffer[current_pos].master_thread_id != mvee_master_thread_id)
				{
					syscall(__NR_sched_yield);
				}
			}
		}
    }

#endif
}

static INLINEIFNODEBUG void mvee_read_lock_result_wake(void)
{
#ifdef MVEE_PARTIAL_ORDER_REPLICATION
	mvee_assert_same_callee(mvee_lock_buffer_prev_pos);
#else
	mvee_assert_same_callee(mvee_lock_buffer_info->pos);
#endif

#ifdef MVEE_PARTIAL_ORDER_REPLICATION
	
	// Check if we can increase the pos so other threads get a smaller scan window
	// Don't try to move beyond prev_pos. Otherwise, the buffer might get flushed while
	// we're tagging our completed operation
	for (unsigned int i = mvee_lock_buffer_info->pos; i < mvee_lock_buffer_prev_pos; ++i)
    {
		if (!mvee_op_is_tagged(i))
		{
			orig_atomic_max(&mvee_lock_buffer_info->pos, i);
			break;
		}
	}
  
	// tag this slot
	mvee_lock_buffer[mvee_lock_buffer_prev_pos].tags[mvee_my_variant_num] = 1;
	
	// make sure that our thread starts from prev_pos + 1 next time
	mvee_lock_buffer_prev_pos++;

#else // MVEE_TOTAL_ORDER_REPLICATION

    mvee_lock_buffer_info->pos++;

#endif // !MVEE_PARTIAL_ORDER_REPLICATION
}

// ========================================================================================================================
// EXTERNAL APIS
// ========================================================================================================================

unsigned char mvee_atomic_preop_internal(unsigned short op_type, void* word_ptr)
{
	if (unlikely(!mvee_should_sync()))
		return 0;
	mvee_check_buffer();
#ifdef MVEE_CHECK_LOCK_TYPE
	if (!mvee_original_call_site)
		mvee_original_call_site = (unsigned long)__builtin_return_address(0);
#endif
	if (likely(mvee_master_variant))
    {
		unsigned int pos = mvee_write_lock_result_prepare();
		mvee_write_lock_result_write(pos, op_type, word_ptr);
		return 1;
    }
	else
    {
		mvee_read_lock_result_wait(op_type, word_ptr);
		return 2;
    }
}

void mvee_atomic_postop_internal(unsigned char preop_result)
{
	if(likely(preop_result) == 1)
		mvee_write_lock_result_finish();
	else if (likely(preop_result) == 2)
		mvee_read_lock_result_wake();
#ifdef MVEE_CHECK_LOCK_TYPE
	mvee_original_call_site = 0;
#endif   
}

unsigned char mvee_atomic_preop(unsigned short op_type, void* word_ptr)
{
#ifdef MVEE_CHECK_LOCK_TYPE
	mvee_original_call_site = (unsigned long)__builtin_return_address(0);
#endif
	return mvee_atomic_preop_internal(op_type + __MVEE_BASE_ATOMICS_MAX__, word_ptr);
}

void mvee_atomic_postop(unsigned char preop_result)
{
	mvee_atomic_postop_internal(preop_result);
}

void mvee_xcheck(unsigned long item)
{
	unsigned char tmp = mvee_atomic_preop_internal(ATOMIC_STORE, (void*) item);
	mvee_atomic_postop_internal(tmp);
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
