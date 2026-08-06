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

/*
 * A wakeup has no event or payload pointer. The callback only tells the caller
 * to poll the native-owned bounded queue until EVENT_QUEUE_EMPTY. This is safe
 * for runtimes such as Dart NativeCallable.listener whose Dart handler runs
 * after the native callback has already returned.
 *
 * Calls are serialized per engine and may run synchronously on any native
 * thread that queues an event, including before callback registration returns.
 * The callback may reenter get_state, poll_event, or clear itself. Reentering
 * start or stop returns INVALID_STATE. Destroy from inside the callback is
 * forbidden.
 *
 * user_data is borrowed. The caller owns it and must keep it valid until a
 * callback-clearing call returns. Clearing is a barrier for callbacks already
 * in flight, except that a callback may clear itself without waiting on its own
 * stack frame.
 */
typedef void (*xnn_transfer_event_wakeup_callback)(void* user_data);

typedef struct xnn_transfer_event_callback_config {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  xnn_transfer_event_wakeup_callback callback;
  void* user_data;
} xnn_transfer_event_callback_config;

/*
 * poll_event copies one event and its inline payload into caller-owned storage.
 * No pointer in this struct is borrowed from native code. The caller initializes
 * struct_size and abi_version before every call. The current ABI requires space
 * for the complete payload array; larger structs are accepted and their unknown
 * tail is left untouched.
 */
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

/*
 * destroy must be externally serialized against every API call using engine.
 * It performs the same callback barrier as stop, discards queued events, and
 * never invokes a callback after it returns.
 */
XNN_TRANSFER_API void xnn_transfer_engine_destroy(xnn_transfer_engine* engine);

XNN_TRANSFER_API xnn_transfer_status
xnn_transfer_engine_start(xnn_transfer_engine* engine);

/*
 * stop publishes STOPPING and STOPPED, closes callback registration, and waits
 * for every in-flight callback before returning. Queued events remain available
 * to poll until destroy, but no callback can begin after stop returns.
 */
XNN_TRANSFER_API xnn_transfer_status
xnn_transfer_engine_stop(xnn_transfer_engine* engine);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_engine_get_state(
    const xnn_transfer_engine* engine, xnn_transfer_engine_state* out_state);

/*
 * Pass a non-null config to register the engine's single callback. Pass null to
 * clear it. If events are already queued, registration may invoke the callback
 * synchronously. A callback is an edge wakeup; consumers must drain the queue.
 */
XNN_TRANSFER_API xnn_transfer_status xnn_transfer_engine_set_event_callback(
    xnn_transfer_engine* engine, const xnn_transfer_event_callback_config* config);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_engine_poll_event(
    xnn_transfer_engine* engine, xnn_transfer_event* out_event);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XNN_TRANSFER_C_API_H_
