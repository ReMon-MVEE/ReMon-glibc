/* Multiple versions of memset.
   All versions must be listed in ifunc-impl-list.c.
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

/* Define multiple versions only for the definition in libc.  */
#if IS_IN (libc)
# define memset __redirect_memset
# include <string.h>
# undef memset

# define SYMBOL_NAME memset
# include "ifunc-memset.h"

libc_ifunc_redirected (__redirect_memset, orig_memset, IFUNC_SELECTOR ());

extern __typeof (orig_memset) mvee_shm_memset;

void *
mvee_memset (void *dest, int ch, size_t len)
{
  if ((unsigned long)dest & 0x8000000000000000ull)
    return mvee_shm_memset(dest, ch, len);

  return orig_memset(dest, ch, len);
}

# ifdef SHARED
__hidden_ver1 (mvee_memset, __GI_memset, __redirect_memset)
  __attribute__ ((visibility ("hidden"))) __attribute_copy__ (orig_memset);
# endif
strong_alias(mvee_memset, memset)
#endif
