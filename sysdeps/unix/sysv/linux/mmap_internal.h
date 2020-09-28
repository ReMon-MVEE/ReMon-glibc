/* Common mmap definition for Linux implementation.
   Copyright (C) 2017-2020 Free Software Foundation, Inc.
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

#ifndef MMAP_INTERNAL_LINUX_H
#define MMAP_INTERNAL_LINUX_H 1

/* This is the minimum mmap2 unit size accept by the kernel.  An architecture
   with multiple minimum page sizes (such as m68k) might define it as -1 and
   thus it will queried at runtime.  */
#ifndef MMAP2_PAGE_UNIT
# define MMAP2_PAGE_UNIT 4096ULL
#endif

#if MMAP2_PAGE_UNIT == -1
static uint64_t page_unit;
# define MMAP_CHECK_PAGE_UNIT()			\
  if (page_unit == 0)				\
    page_unit = __getpagesize ();
# undef MMAP2_PAGE_UNIT
# define MMAP2_PAGE_UNIT page_unit
#else
# define MMAP_CHECK_PAGE_UNIT()
#endif

/* Do not accept offset not multiple of page size.  */
#define MMAP_OFF_LOW_MASK  (MMAP2_PAGE_UNIT - 1)

/* An architecture may override this.  */
#ifndef MMAP_CALL
extern void* mvee_shm_mmap (void *addr, size_t len, int prot, int flags, int fd, off_t offset);

#ifdef __NR_mmap2
# define orig_MMAP_CALL(__addr, __len, __prot, __flags, __fd, __offset) \
  INLINE_SYSCALL_CALL (mmap2, __addr, __len, __prot, __flags, __fd, __offset)
#else
# define orig_MMAP_CALL(__addr, __len, __prot, __flags, __fd, __offset) \
  INLINE_SYSCALL_CALL (mmap, __addr, __len, __prot, __flags, __fd, __offset)
#endif

# if IS_IN (libc)
#  define MMAP_CALL(__nr, __addr, __len, __prot, __flags, __fd, __offset) \
  mvee_shm_mmap (__addr, __len, __prot, __flags, __fd, __offset)
# else
#  define MMAP_CALL(__nr, __addr, __len, __prot, __flags, __fd, __offset) \
  orig_MMAP_CALL (__addr, __len, __prot, __flags, __fd, __offset)
# endif
#endif

#endif /* MMAP_INTERNAL_LINUX_H  */
