#ifndef _MVEE_AGENTS_H_DECLS
#define _MVEE_AGENTS_H_DECLS

extern unsigned char                  mvee_libc_initialized;
extern unsigned char                  mvee_master_variant;
extern unsigned char                  mvee_sync_enabled;
extern unsigned long                  mvee_shm_tag;
extern unsigned short                 mvee_num_variants;

extern void mvee_infinite_loop(void);

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
#define gcc_barrier()  asm volatile("" ::: "memory")
#ifndef arch_cpu_relax
#define arch_cpu_relax()
#endif

#endif /* Not _MVEE_AGENTS_H_DECLS */
