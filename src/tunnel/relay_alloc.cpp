#include "relay_alloc.h"
#include <cstdlib>

#if TCPQUIC_USE_MIMALLOC
#include <mimalloc.h>
#endif

void* TqAllocBytes(size_t bytes) {
#if TCPQUIC_USE_MIMALLOC
  return mi_malloc(bytes);
#else
  return std::malloc(bytes);
#endif
}

void TqFreeBytes(void* ptr, size_t bytes) {
  (void)bytes;
  if (ptr == nullptr) return;
#if TCPQUIC_USE_MIMALLOC
  mi_free(ptr);
#else
  std::free(ptr);
#endif
}
