#pragma once
#include <cstddef>

void* TqAllocBytes(size_t bytes);
void TqFreeBytes(void* ptr, size_t bytes);
