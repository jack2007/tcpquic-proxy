#pragma once
#include <cstddef>

void* TqMalloc(size_t bytes);
void* TqCalloc(size_t count, size_t bytes);
void* TqRealloc(void* ptr, size_t bytes);
void TqFree(void* ptr);

void* TqAllocBytes(size_t bytes);
void TqFreeBytes(void* ptr, size_t bytes);
