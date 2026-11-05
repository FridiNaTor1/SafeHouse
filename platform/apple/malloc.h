#ifndef CANE_PLATFORM_APPLE_MALLOC_H_
#define CANE_PLATFORM_APPLE_MALLOC_H_

#include <malloc/malloc.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int malloc_trim(size_t pad) {
  (void)pad;
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif  // CANE_PLATFORM_APPLE_MALLOC_H_
