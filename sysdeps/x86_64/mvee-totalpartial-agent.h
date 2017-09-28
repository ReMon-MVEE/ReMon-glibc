//
// MVEE_PARTIAL_ORDER_REPLICATION: when defined, slaves will use
// queue projection to replay synchronization operations in
// partial order rather than total order. In other words,
// the slaves will only respect the order in which the master
// has performed its synchronization operations on a per-word
// basis
//
#define MVEE_PARTIAL_ORDER_REPLICATION
//
// MVEE_EXTENDED_QUEUE: when defined, the locking operation and
// mutex pointer are also logged in the queue.
//
#define MVEE_EXTENDED_QUEUE
//
// MVEE_LOG_EIPS: when defined, libc logs return addresses for
// all locking operations into a seperate queue
//
// WARNING: enabling EIP logging _CAN_ trigger crashes! We're
// using __builtin_return_address(2) to fetch the eip of the 
// caller of the locking function. Unfortunately, libc uses inline
// __libc_lock_* operations every now and then. When it does, 
// the __builtin_... call will return the wrong caller and in some
// cases (e.g. in do_system) it might try to fetch the eip beyond
// the end of the stack!
//
#define MVEE_LOG_EIPS
#define MVEE_STACK_DEPTH 5
//
// MVEE_CHECK_LOCK_TYPE: if this is defined, the slave will check
// whether or not it's replaying a lock of the same type
// (only works with the extended queue)
//
#define MVEE_CHECK_LOCK_TYPE
//
// MVEE_DEBUG_MALLOC: if this is defined, the slaves will check whether
// their malloc behavior is synced with the master
//
#define MVEE_DEBUG_MALLOC

#define DEFINE_MVEE_QUEUE(name, has_eip_queue)				\
  static unsigned long             mvee_##name##_buffer_data_start  = 0; \
  static volatile unsigned int*    mvee_##name##_buffer_pos         = 0; \
  static volatile unsigned int*    mvee_##name##_buffer_lock        = 0; \
  static volatile unsigned int*    mvee_##name##_buffer_flush_cnt   = 0; \
  static volatile unsigned char*   mvee_##name##_buffer_flushing    = 0; \
  static unsigned long             mvee_##name##_buffer_slots       = 0; \
  static void*                     mvee_##name##_eip_buffer         = NULL; \
  static unsigned char             mvee_##name##_buffer_log_eips    = has_eip_queue;

// this is extremely wasteful but required to prevent false sharing in the producer...
#ifdef MVEE_PARTIAL_ORDER_REPLICATION
# ifdef MVEE_EXTENDED_QUEUE
      #define MVEE_LOCK_QUEUE_SLOT_SIZE (sizeof(long) + sizeof(short) * (mvee_num_childs + 1))
# else
      #define MVEE_LOCK_QUEUE_SLOT_SIZE (sizeof(long) + sizeof(short) + (mvee_num_childs - 1))
# endif
#else
# ifdef MVEE_EXTENDED_QUEUE
      #define MVEE_LOCK_QUEUE_SLOT_SIZE (3*sizeof(long))
# else
      #define MVEE_LOCK_QUEUE_SLOT_SIZE sizeof(short)
# endif
#endif
#define mvee_lock_buffer_slot_size 64
#define mvee_malloc_buffer_slot_size 64

// In the new queue layout, we want each replica's lock, position, 
// flush_cnt and flushing word on one and the same cache line
// Therefore, we round up the buffer ptr to a multiple of 64 for the master replica.
// Each subsequent replica has its variables aligned on the next cache line boundary

#define INIT_MVEE_QUEUE(name, slot_size, queue_ident)			\
  if (!mvee_##name##_buffer_data_start)					\
    {									\
      long tmp_id = syscall(MVEE_GET_SHARED_BUFFER, 0, queue_ident, &mvee_##name##_buffer_slots, MVEE_LOCK_QUEUE_SLOT_SIZE); \
      mvee_##name##_buffer_slots      = (mvee_##name##_buffer_slots - mvee_num_childs * 64) / mvee_##name##_buffer_slot_size - 2; \
      void* tmp_buffer                = (void*)syscall(__NR_shmat, tmp_id, NULL, 0); \
      mvee_##name##_buffer_lock       = (volatile unsigned int*)  (MVEE_ROUND_UP((unsigned long)tmp_buffer, 64) + mvee_child_num * 64); \
      mvee_##name##_buffer_pos        = (volatile unsigned int*)  (MVEE_ROUND_UP((unsigned long)tmp_buffer, 64) + mvee_child_num * 64 + sizeof(int)); \
      mvee_##name##_buffer_flush_cnt  = (volatile unsigned int*)  (MVEE_ROUND_UP((unsigned long)tmp_buffer, 64) + mvee_child_num * 64 + sizeof(int) * 2); \
      mvee_##name##_buffer_flushing   = (volatile unsigned char*) (MVEE_ROUND_UP((unsigned long)tmp_buffer, 64) + mvee_child_num * 64 + sizeof(int) * 3); \
     *mvee_##name##_buffer_lock       = 1; \
      mvee_##name##_buffer_data_start = MVEE_ROUND_UP((unsigned long)tmp_buffer, 64) + mvee_num_childs * 64; \
      if (mvee_##name##_buffer_log_eips)				\
	{								\
	  long eip_buffer_id = syscall(MVEE_GET_SHARED_BUFFER, 1,	\
				      queue_ident, NULL, mvee_num_childs * sizeof(long) * MVEE_STACK_DEPTH, MVEE_STACK_DEPTH); \
	  mvee_##name##_eip_buffer = (void*)syscall(__NR_shmat, eip_buffer_id, NULL, 0); \
	}								\
    }									

#define MVEE_LOG_QUEUE_DATA(name, pos, offset, data)			\
  *(typeof(data)*)(mvee_##name##_buffer_data_start + mvee_##name##_buffer_slot_size * (pos) + offset) = data;

#define MVEE_READ_QUEUE_DATA(name, pos, offset, result)			\
  result = *(typeof(result)*)(mvee_##name##_buffer_data_start + mvee_##name##_buffer_slot_size * pos + offset);

#define MVEE_LOG_STACK(name, start_depth, pos)				\
  mvee_log_stack(mvee_##name##_eip_buffer, sizeof(long) * mvee_num_childs * MVEE_STACK_DEPTH, pos, start_depth);


extern void mvee_invalidate_buffer      (void);
extern void mvee_atomic_postop_internal (unsigned char preop_result);
extern int  mvee_should_sync_tid        (void);
extern int  mvee_all_heaps_aligned      (char* heap, unsigned long alloc_size); 
extern void mvee_xcheck                 (unsigned long item);

#define MVEE_POSTOP() \
  mvee_atomic_postop_internal(__tmp_mvee_preop);

#ifdef MVEE_EXTENDED_QUEUE
 extern unsigned char     mvee_atomic_preop_internal             (unsigned char is_store, void* word_ptr, unsigned short op_type);
# define MVEE_PREOP(op_type, mem, is_store)					\
	register unsigned char __tmp_mvee_preop = mvee_atomic_preop_internal(is_store, (void*)mem, op_type);
#else
 extern unsigned char     mvee_atomic_preop_internal            (unsigned char is_store, void* word_ptr);
# define MVEE_PREOP(op_type, mem, is_store) \
	register unsigned char __tmp_mvee_preop = mvee_atomic_preop_internal(is_store, (void*)mem);
#endif // !MVEE_EXTENDED_QUEUE
