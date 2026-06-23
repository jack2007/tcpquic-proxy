#include "relay_alloc.h"
#include <cstdlib>

#if TCPQUIC_USE_MIMALLOC
#include <mimalloc.h>
#endif

void* TqMalloc(size_t bytes) {
#if TCPQUIC_USE_MIMALLOC
  return mi_malloc(bytes);
#else
  return std::malloc(bytes);
#endif
}

void* TqCalloc(size_t count, size_t bytes) {
#if TCPQUIC_USE_MIMALLOC
  return mi_calloc(count, bytes);
#else
  return std::calloc(count, bytes);
#endif
}

void* TqRealloc(void* ptr, size_t bytes) {
#if TCPQUIC_USE_MIMALLOC
  return mi_realloc(ptr, bytes);
#else
  return std::realloc(ptr, bytes);
#endif
}

void TqFree(void* ptr) {
  if (ptr == nullptr) return;
#if TCPQUIC_USE_MIMALLOC
  mi_free(ptr);
#else
  std::free(ptr);
#endif
}

void* TqAllocBytes(size_t bytes) {
  return TqMalloc(bytes);
}

void TqFreeBytes(void* ptr, size_t bytes) {
  (void)bytes;
  TqFree(ptr);
}
