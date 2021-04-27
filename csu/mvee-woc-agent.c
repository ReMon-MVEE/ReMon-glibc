#define MVEE_SLAVE_YIELD
#define MVEE_TOTAL_CLOCK_COUNT   2048
#define MVEE_CLOCK_GROUP_SIZE    64
#define MVEE_TOTAL_CLOCK_GROUPS  (MVEE_TOTAL_CLOCK_COUNT / MVEE_CLOCK_GROUP_SIZE)

struct mvee_counter
{
  volatile unsigned long lock;
  volatile unsigned long counter;
  unsigned char padding[64 - 2 * sizeof(unsigned long)]; // prevents false sharing
};

struct mvee_op_entry
{
  volatile unsigned long  counter_and_idx; // the value we must see in mvee_counters[idx] before we can replay the operation
};

static __thread unsigned long         mvee_thread_local_pos         = 0; // our position in the thread local queue
static __thread  
    struct mvee_op_entry*             mvee_thread_local_queue       = NULL;
static __thread unsigned long         mvee_thread_local_queue_size  = 0; // nr of slots in the thread local queue
static __thread unsigned short        mvee_prev_idx                 = 0;

__attribute__((aligned (64)))
static struct mvee_counter            mvee_counters[MVEE_TOTAL_CLOCK_COUNT + 1];

void mvee_invalidate_buffer(void)
{
	mvee_thread_local_queue = NULL;
}

unsigned char mvee_atomic_preop_internal(volatile void* word_ptr)
{
	if (unlikely(!mvee_sync_enabled))
		return 0;

	if (unlikely(!mvee_thread_local_queue))
    {
		long mvee_thread_local_queue_id = syscall(MVEE_GET_SHARED_BUFFER, &mvee_counters, MVEE_LIBC_ATOMIC_BUFFER, &mvee_thread_local_queue_size, &mvee_thread_local_pos, NULL);
		syscall(MVEE_RESET_ATFORK, &mvee_thread_local_queue, sizeof(mvee_thread_local_queue));
		mvee_thread_local_queue_size   /= sizeof(struct mvee_op_entry);
		mvee_thread_local_queue         = (void*)syscall(__NR_shmat, mvee_thread_local_queue_id, NULL, 0);     
		mvee_thread_local_pos = 0;
    }

	if (unlikely(mvee_thread_local_pos >= mvee_thread_local_queue_size))
    {
		syscall(MVEE_FLUSH_SHARED_BUFFER, MVEE_LIBC_ATOMIC_BUFFER);
		mvee_thread_local_pos = 0;
    }

	if (likely(mvee_master_variant))
    {
		// page number defines the clock group
		// offset within page defines the clock within that group
		mvee_prev_idx = (((((unsigned long)word_ptr >> 24) % MVEE_TOTAL_CLOCK_GROUPS) * (MVEE_CLOCK_GROUP_SIZE) 
						  + ((((unsigned long)word_ptr & 4095) >> 6) % MVEE_CLOCK_GROUP_SIZE))
						 & 0xFFF) + 1;

		while (!__sync_bool_compare_and_swap(&mvee_counters[mvee_prev_idx].lock, 0, 1))
			arch_cpu_relax();
		
		unsigned long pos = mvee_counters[mvee_prev_idx].counter;    

		mvee_thread_local_queue[mvee_thread_local_pos++].counter_and_idx 
			= (pos << 12) | mvee_prev_idx;

		atomic_full_barrier();

		return 1;
    }
	else
    {
		unsigned long counter_and_idx = 0;

		while (unlikely(1))
		{
			counter_and_idx = mvee_thread_local_queue[mvee_thread_local_pos].counter_and_idx;

			if (likely(counter_and_idx))
				break;

#ifdef MVEE_SLAVE_YIELD
			syscall(__NR_sched_yield);
#else
			arch_cpu_relax();
#endif
		}

		mvee_prev_idx = counter_and_idx & 0xFFF;
		counter_and_idx &= ~0xFFF;

		atomic_full_barrier();

		while ((mvee_counters[mvee_prev_idx].counter << 12) != counter_and_idx)
#ifdef MVEE_SLAVE_YIELD
			syscall(__NR_sched_yield);
#else
		arch_cpu_relax();
#endif

		atomic_full_barrier();
		
		return 2;
    }
}

void mvee_atomic_postop_internal(unsigned char preop_result)
{
	atomic_full_barrier();
	
	if(likely(preop_result == 1))
	{
		gcc_barrier();
		orig_atomic_increment(&mvee_counters[mvee_prev_idx].counter);
		atomic_full_barrier();
		mvee_counters[mvee_prev_idx].lock = 0;
	}
	else if (likely(preop_result == 2))
	{
		gcc_barrier();
		mvee_counters[mvee_prev_idx].counter++;
		mvee_thread_local_pos++;
	}
}

/* Checks if all variants got ALIGNMENT aligned heaps from
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
	if (!mvee_thread_local_queue)
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

int mvee_should_sync_tid(void)
{
	return mvee_sync_enabled ? 1 : 0;
}

unsigned char mvee_atomic_preop(unsigned short op_type, void* word_ptr)
{
	return mvee_atomic_preop_internal(word_ptr);
}

void mvee_atomic_postop(unsigned char preop_result)
{
	mvee_atomic_postop_internal(preop_result);
}

unsigned char mvee_should_futex_unlock(void)
{
	return (!mvee_master_variant && mvee_sync_enabled) ? 1 : 0;
}
