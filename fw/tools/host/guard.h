#pragma once
/* forced into ALACDecoder.cpp only (run.py): its buffers come with guard areas test_alac.cpp looks at after each frame */
#include <stdlib.h>
#include <cstdlib>
#include <cstring>
extern "C" void *guard_calloc(size_t n, size_t size);
extern "C" void guard_free(void *p);
#define calloc guard_calloc
#define free guard_free
