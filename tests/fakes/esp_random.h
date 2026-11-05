#pragma once
#include <cstddef>
#include <cstring>
inline void esp_fill_random(void *data, size_t size) { std::memset(data, 42, size); }
