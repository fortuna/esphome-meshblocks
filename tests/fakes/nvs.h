#pragma once
#include <cstddef>
#include <cstdint>
using nvs_handle_t = uint32_t;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_NVS_NOT_FOUND = 1;
constexpr int NVS_READWRITE = 0;
int nvs_open(const char *name, int mode, nvs_handle_t *handle);
void nvs_close(nvs_handle_t handle);
int nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size);
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size);
int nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value);
int nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
int nvs_commit(nvs_handle_t handle);
