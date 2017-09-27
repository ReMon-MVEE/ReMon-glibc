#define MVEE_MAX_COUNTERS 65536

#define MVEE_MALLOC_HOOK(type, msg, sz, ar_ptr, chunk_ptr)

extern void          mvee_atomic_postop_internal (unsigned char preop_result);
extern unsigned char mvee_atomic_preop_internal  (volatile void* word_ptr);
extern int           mvee_should_sync_tid        (void);
extern int           mvee_all_heaps_aligned      (char* heap, unsigned long alloc_size); 
extern void          mvee_invalidate_buffer      (void);
extern unsigned char mvee_should_futex_unlock    (void);

#define MVEE_POSTOP()								\
	mvee_atomic_postop_internal(__tmp_mvee_preop);

#define MVEE_PREOP(op_type, mem, is_store)								\
	register unsigned char  __tmp_mvee_preop = mvee_atomic_preop_internal((volatile void*)mem);
