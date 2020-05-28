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

//
// Latest version of the replication buffer layout:
//
// struct mvee_buffer_info for variant <0>
// ...
// struct mvee_buffer_info for variant <N>
// struct mvee_buffer_entry for replicated operation <0>
// ...
// struct mvee_buffer_entry for replication operation <number of requested slots - 1>
//

//
// The callstack buffer is separate and is just laid out like:
//
// struct mvee_callstack_entry for replicated operation <0> in variant <0>
// ...
// struct mvee_callstack_entry for replicated operation <0> in variant <N>
// ...
// ...
// struct mvee_callstack_entry for replicated operation <number of requested slots - 1> in variant <0>
// ...
// struct mvee_callstack_entry for replicated operation <number of requested slots - 1> in variant <N>
//

struct mvee_buffer_info
{
	// The master must acquire this lock before writing into the buffer
	volatile int lock;
    // In the master, pos is the index of the next element we're going to write
    // In the slave, pos is the index of the first element that hasn't been replicated yet
	volatile unsigned int pos;
	// How many elements fit inside the buffer?
	// This does not include the position entries
	unsigned int size;
    // How many times has the buffer been flushed?
    volatile unsigned int flush_cnt;
    // Are we flushing the buffer right now?
    volatile unsigned char flushing;
	// Type of the buffer. Must be MVEE_LIBC_LOCK_BUFFER or MVEE_LIBC_LOCK_BUFFER_PARTIAL
	unsigned char buffer_type;
	// Pad to the next cache line boundary
	unsigned char padding[64 - sizeof(int) * 4 - sizeof(unsigned char) * 2];
};

struct mvee_buffer_entry
{
	// the memory location that is being accessed atomically
	unsigned long word_ptr;
	// the thread id of the master variant thread that accessed the field
	unsigned int master_thread_id;
	// type of the operation
	unsigned short operation_type;
	// Pad to the next cache line boundary. We use this to write tags in the partial order buffer
	unsigned char tags[64 - sizeof(long) - sizeof(int) - sizeof(short)];
};

struct mvee_callstack_entry
{
    // might be zero
    unsigned long callee[MVEE_STACK_DEPTH];
};

extern void mvee_invalidate_buffer      (void);
extern void mvee_atomic_postop_internal (unsigned char preop_result);
extern int  mvee_should_sync_tid        (void);
extern int  mvee_all_heaps_aligned      (char* heap, unsigned long alloc_size); 
extern void mvee_xcheck                 (unsigned long item);

#define MVEE_POSTOP() \
  mvee_atomic_postop_internal(__tmp_mvee_preop);

extern unsigned char     mvee_atomic_preop_internal (unsigned short op_type, void* word_ptr);
#define MVEE_PREOP(op_type, mem, is_store)					\
	register unsigned char __tmp_mvee_preop = mvee_atomic_preop_internal(op_type, (void*)mem);
