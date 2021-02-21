#include "mvee-agent-shared.h"

#include <atomic.h>
#include <stddef.h>
#include <unistd.h>

unsigned char                  mvee_libc_initialized         = 0;
unsigned char                  mvee_master_variant           = 0;
unsigned char                  mvee_sync_enabled             = 0;
unsigned short                 mvee_num_variants             = 0;

#ifdef MVEE_USE_TOTALPARTIAL_AGENT
#include "mvee-totalpartial-agent.c"
#else
#include "mvee-woc-agent.c"
#endif
