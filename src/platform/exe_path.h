#pragma once

#include <string>

// Directory containing the running executable (no trailing slash).
bool TqGetExecutableDirectory(std::string& out);

// Writes the executable directory into out (NUL-terminated). Returns false on failure.
bool TqGetExecutableDirectory(char* out, size_t outSize);
