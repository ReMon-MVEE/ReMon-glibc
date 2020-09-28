#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sysdep.h>

int
__munmap(void* addr, size_t len)
{
    return MUNMAP_CALL(addr, len);
}

weak_alias (__munmap, munmap)
libc_hidden_def (__munmap)