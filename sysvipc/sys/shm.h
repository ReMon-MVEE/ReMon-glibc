/* Copyright (C) 1995-2020 Free Software Foundation, Inc.
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
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

#ifndef _SYS_SHM_H
#define _SYS_SHM_H	1

#include <features.h>

#define __need_size_t
#include <stddef.h>

/* Get common definition of System V style IPC.  */
#include <sys/ipc.h>

/* Get system dependent definition of `struct shmid_ds' and more.  */
#include <bits/shm.h>

/* Define types required by the standard.  */
#include <bits/types/time_t.h>

#ifdef __USE_XOPEN
# ifndef __pid_t_defined
typedef __pid_t pid_t;
#  define __pid_t_defined
# endif
#endif	/* X/Open */


__BEGIN_DECLS

/* The following System V style IPC functions implement a shared memory
   facility.  The definition is found in XPG4.2.  */

/* Shared memory control operation.  */
extern int shmctl (int __shmid, int __cmd, struct shmid_ds *__buf) __THROW;


/* Get shared memory segment.  */
extern int shmget (key_t __key, size_t __size, int __shmflg) __THROW;


/* Attach shared memory segment.  */
extern void *shmat (int __shmid, const void *__shmaddr, int __shmflg)
     __THROW;
extern void *mvee_shm_shmat (int __shmid, const void *__shmaddr, int __shmflg)
     __THROW;

#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
#define orig_SHMAT_CALL(__shmid, __shmaddr, __shmflg)                                                                  \
(void*) INLINE_SYSCALL_CALL (shmat, __shmid, __shmaddr, __shmflg);
#else
#define orig_SHMAT_CALL(__shmid, __shmaddr, __shmflg)                                                                  \
({                                                                                                                     \
    INTERNAL_SYSCALL_DECL(err);                                                                                        \
    unsigned long resultvar;                                                                                           \
    void *raddr;                                                                                                       \
                                                                                                                       \
    resultvar = INTERNAL_SYSCALL_CALL (ipc, err, IPCOP_shmat, shmid, shmflg, &raddr, shmaddr);                         \
    if (INTERNAL_SYSCALL_ERROR_P (resultvar, err))                                                                     \
        raddr = (void *) INLINE_SYSCALL_ERROR_RETURN_VALUE (INTERNAL_SYSCALL_ERRNO (resultvar, err));                  \
    raddr                                                                                                              \
})
#endif


/* Detach shared memory segment.  */
extern int shmdt (const void *__shmaddr) __THROW;
extern int mvee_shm_shmdt (const void *__shmaddr) __THROW;

#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
#define orig_SHMDT_CALL(__shmaddr) INLINE_SYSCALL_CALL (shmdt, __shmaddr);
#else
#define orig_SHMDT_CALL(__shmaddr) INLINE_SYSCALL_CALL (ipc, IPCOP_shmdt, 0, 0, 0, __shmaddr);
#endif


__END_DECLS

#endif /* sys/shm.h */
