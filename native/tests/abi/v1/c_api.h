#ifndef XNN_TRANSFER_TEST_ABI_V1_C_API_H_
#define XNN_TRANSFER_TEST_ABI_V1_C_API_H_

/*
 * Frozen ABI v1 caller view. This file intentionally does not include the
 * production header. Existing callers compile against these declarations while
 * linking to the current library.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define XNN_TRANSFER_API __declspec(dllimport)
#else
#define XNN_TRANSFER_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define XNN_TRANSFER_ABI_VERSION 1u
#define XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE 256u
#define XNN_TRANSFER_EVENT_QUEUE_CAPACITY 64u
#define XNN_TRANSFER_ENGINE_STATE_EVENT_PAYLOAD_VERSION 1u

typedef struct xnn_transfer_engine xnn_transfer_engine;

typedef enum xnn_transfer_status {
  XNN_TRANSFER_STATUS_OK = 0,
  XNN_TRANSFER_STATUS_INVALID_ARGUMENT = 1,
  XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI = 2,
  XNN_TRANSFER_STATUS_INVALID_STATE = 3,
  XNN_TRANSFER_STATUS_INTERNAL_ERROR = 4,
  XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY = 5
} xnn_transfer_status;

typedef enum xnn_transfer_engine_state {
  XNN_TRANSFER_ENGINE_STATE_CREATED = 0,
  XNN_TRANSFER_ENGINE_STATE_RUNNING = 1,
  XNN_TRANSFER_ENGINE_STATE_STOPPED = 2,
  XNN_TRANSFER_ENGINE_STATE_STOPPING = 3
} xnn_transfer_engine_state;

typedef enum xnn_transfer_event_type {
  XNN_TRANSFER_EVENT_TYPE_ENGINE_STATE_CHANGED = 1
} xnn_transfer_event_type;

typedef enum xnn_transfer_event_flags {
  XNN_TRANSFER_EVENT_FLAG_NONE = 0,
  XNN_TRANSFER_EVENT_FLAG_EVENTS_DROPPED_BEFORE = 1
} xnn_transfer_event_flags;

typedef struct xnn_transfer_engine_config {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
} xnn_transfer_engine_config;

typedef void (*xnn_transfer_event_wakeup_callback)(void* user_data);

typedef struct xnn_transfer_event_callback_config {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  xnn_transfer_event_wakeup_callback callback;
  void* user_data;
} xnn_transfer_event_callback_config;

typedef struct xnn_transfer_event {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t type;
  uint64_t sequence;
  uint32_t payload_version;
  uint32_t payload_size;
  uint32_t flags;
  uint32_t reserved;
  uint8_t payload[XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE];
} xnn_transfer_event;

typedef struct xnn_transfer_engine_state_event_payload {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t state;
  uint32_t reserved;
} xnn_transfer_engine_state_event_payload;

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

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_engine_set_event_callback(
    xnn_transfer_engine* engine, const xnn_transfer_event_callback_config* config);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_engine_poll_event(
    xnn_transfer_engine* engine, xnn_transfer_event* out_event);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XNN_TRANSFER_TEST_ABI_V1_C_API_H_
