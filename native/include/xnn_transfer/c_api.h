#ifndef XNN_TRANSFER_C_API_H_
#define XNN_TRANSFER_C_API_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(XNN_TRANSFER_BUILDING_LIBRARY)
#define XNN_TRANSFER_API __declspec(dllexport)
#else
#define XNN_TRANSFER_API __declspec(dllimport)
#endif
#else
#define XNN_TRANSFER_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define XNN_TRANSFER_ABI_VERSION 1u

typedef struct xnn_transfer_engine xnn_transfer_engine;

typedef enum xnn_transfer_status {
  XNN_TRANSFER_STATUS_OK = 0,
  XNN_TRANSFER_STATUS_INVALID_ARGUMENT = 1,
  XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI = 2,
  XNN_TRANSFER_STATUS_INVALID_STATE = 3,
  XNN_TRANSFER_STATUS_INTERNAL_ERROR = 4
} xnn_transfer_status;

typedef enum xnn_transfer_engine_state {
  XNN_TRANSFER_ENGINE_STATE_CREATED = 0,
  XNN_TRANSFER_ENGINE_STATE_RUNNING = 1,
  XNN_TRANSFER_ENGINE_STATE_STOPPED = 2
} xnn_transfer_engine_state;

typedef struct xnn_transfer_engine_config {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
} xnn_transfer_engine_config;

XNN_TRANSFER_API uint32_t xnn_transfer_abi_version(void);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_engine_create(
    const xnn_transfer_engine_config* config, xnn_transfer_engine** out_engine);

XNN_TRANSFER_API void xnn_transfer_engine_destroy(xnn_transfer_engine* engine);

XNN_TRANSFER_API xnn_transfer_status
xnn_transfer_engine_start(xnn_transfer_engine* engine);

XNN_TRANSFER_API xnn_transfer_status
xnn_transfer_engine_stop(xnn_transfer_engine* engine);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_engine_get_state(
    const xnn_transfer_engine* engine, xnn_transfer_engine_state* out_state);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XNN_TRANSFER_C_API_H_
