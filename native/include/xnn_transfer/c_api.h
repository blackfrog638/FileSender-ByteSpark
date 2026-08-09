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
#define XNN_TRANSFER_DISCOVERY_PEER_EVENT_PAYLOAD_VERSION 1u
#define XNN_TRANSFER_DISCOVERY_DISPLAY_LABEL_MAX_SIZE 96u
#define XNN_TRANSFER_DISCOVERY_ADDRESS_MAX_SIZE 16u
#define XNN_TRANSFER_DISCOVERY_SNAPSHOT_PAGE_CAPACITY 8u
#define XNN_TRANSFER_PAIRING_ATTEMPT_EVENT_PAYLOAD_VERSION 1u
#define XNN_TRANSFER_TRUST_EVENT_PAYLOAD_VERSION 1u
#define XNN_TRANSFER_PAIRING_ATTEMPT_ID_SIZE 16u
#define XNN_TRANSFER_PAIRING_SAS_WORD_COUNT 5u
#define XNN_TRANSFER_TRUST_SNAPSHOT_PAGE_CAPACITY 8u

typedef struct xnn_transfer_engine xnn_transfer_engine;

typedef enum xnn_transfer_status {
  XNN_TRANSFER_STATUS_OK = 0,
  XNN_TRANSFER_STATUS_INVALID_ARGUMENT = 1,
  XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI = 2,
  XNN_TRANSFER_STATUS_INVALID_STATE = 3,
  XNN_TRANSFER_STATUS_INTERNAL_ERROR = 4,
  XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY = 5,
  XNN_TRANSFER_STATUS_STALE_SNAPSHOT = 6,
  XNN_TRANSFER_STATUS_UNAVAILABLE = 7,
  XNN_TRANSFER_STATUS_STALE_HANDLE = 8
} xnn_transfer_status;

typedef enum xnn_transfer_engine_state {
  XNN_TRANSFER_ENGINE_STATE_CREATED = 0,
  XNN_TRANSFER_ENGINE_STATE_RUNNING = 1,
  XNN_TRANSFER_ENGINE_STATE_STOPPED = 2,
  XNN_TRANSFER_ENGINE_STATE_STOPPING = 3
} xnn_transfer_engine_state;

typedef enum xnn_transfer_event_type {
  XNN_TRANSFER_EVENT_TYPE_ENGINE_STATE_CHANGED = 1,
  XNN_TRANSFER_EVENT_TYPE_DISCOVERY_PEER_CHANGED = 2,
  XNN_TRANSFER_EVENT_TYPE_PAIRING_ATTEMPT_CHANGED = 3,
  XNN_TRANSFER_EVENT_TYPE_TRUST_CHANGED = 4
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

typedef enum xnn_transfer_discovery_address_family {
  XNN_TRANSFER_DISCOVERY_ADDRESS_FAMILY_IPV4 = 4,
  XNN_TRANSFER_DISCOVERY_ADDRESS_FAMILY_IPV6 = 6
} xnn_transfer_discovery_address_family;

typedef enum xnn_transfer_discovery_peer_change {
  XNN_TRANSFER_DISCOVERY_PEER_APPEARED = 1,
  XNN_TRANSFER_DISCOVERY_PEER_UPDATED = 2,
  XNN_TRANSFER_DISCOVERY_PEER_EXPIRED = 3
} xnn_transfer_discovery_peer_change;

typedef enum xnn_transfer_discovery_expiry_reason {
  XNN_TRANSFER_DISCOVERY_EXPIRY_NONE = 0,
  XNN_TRANSFER_DISCOVERY_EXPIRY_TTL = 1,
  XNN_TRANSFER_DISCOVERY_EXPIRY_WITHDRAWN = 2,
  XNN_TRANSFER_DISCOVERY_EXPIRY_INTERFACE_REMOVED = 3,
  XNN_TRANSFER_DISCOVERY_EXPIRY_WAKE = 4,
  XNN_TRANSFER_DISCOVERY_EXPIRY_DISCOVERY_STOPPED = 5
} xnn_transfer_discovery_expiry_reason;

typedef struct xnn_transfer_discovery_config {
  size_t struct_size;
  uint32_t abi_version;
  uint16_t service_port;
  uint16_t reserved;
  uint32_t display_label_size;
  uint8_t display_label[XNN_TRANSFER_DISCOVERY_DISPLAY_LABEL_MAX_SIZE];
} xnn_transfer_discovery_config;

/*
 * peer_id is an opaque, process-local identifier for one observed candidate.
 * It is not a device identity or trust decision. Address and label bytes are
 * copied from untrusted discovery state and are bounded by this struct.
 */
typedef struct xnn_transfer_discovery_peer {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  uint64_t peer_id;
  uint16_t service_port;
  uint8_t address_family;
  uint8_t address_size;
  uint32_t display_label_size;
  uint8_t address[XNN_TRANSFER_DISCOVERY_ADDRESS_MAX_SIZE];
  uint8_t display_label[XNN_TRANSFER_DISCOVERY_DISPLAY_LABEL_MAX_SIZE];
} xnn_transfer_discovery_peer;

typedef struct xnn_transfer_discovery_peer_event_payload {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t change;
  uint64_t snapshot_revision;
  uint32_t expiry_reason;
  uint32_t reserved;
  xnn_transfer_discovery_peer peer;
} xnn_transfer_discovery_peer_event_payload;

/*
 * Pass expected_revision=0 to begin a snapshot. Continue with the returned
 * revision and offset+count. A concurrent peer change returns STALE_SNAPSHOT,
 * requiring the caller to restart at revision 0 and offset 0.
 */
typedef struct xnn_transfer_discovery_snapshot_page {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  uint64_t snapshot_revision;
  uint32_t offset;
  uint32_t count;
  uint32_t total_count;
  uint32_t reserved2;
  xnn_transfer_discovery_peer peers[XNN_TRANSFER_DISCOVERY_SNAPSHOT_PAGE_CAPACITY];
} xnn_transfer_discovery_snapshot_page;

typedef enum xnn_transfer_pairing_attempt_state {
  XNN_TRANSFER_PAIRING_ATTEMPT_STARTING = 1,
  XNN_TRANSFER_PAIRING_ATTEMPT_AWAITING_CONFIRMATION = 2,
  XNN_TRANSFER_PAIRING_ATTEMPT_PAIRED = 3,
  XNN_TRANSFER_PAIRING_ATTEMPT_CLOSED = 4
} xnn_transfer_pairing_attempt_state;

/*
 * Detailed parser, certificate, pin, storage, and transcript failures are
 * intentionally collapsed before crossing the ABI. These values are local UI
 * outcomes and are never serialized to a peer.
 */
typedef enum xnn_transfer_pairing_error {
  XNN_TRANSFER_PAIRING_ERROR_NONE = 0,
  XNN_TRANSFER_PAIRING_ERROR_REJECTED = 1,
  XNN_TRANSFER_PAIRING_ERROR_CANCELLED = 2,
  XNN_TRANSFER_PAIRING_ERROR_TIMED_OUT = 3,
  XNN_TRANSFER_PAIRING_ERROR_BUSY = 4,
  XNN_TRANSFER_PAIRING_ERROR_UNAVAILABLE = 5,
  XNN_TRANSFER_PAIRING_ERROR_FAILED = 6
} xnn_transfer_pairing_error;

typedef enum xnn_transfer_trust_state {
  XNN_TRANSFER_TRUST_STATE_ACTIVE = 1,
  XNN_TRANSFER_TRUST_STATE_REVOKED = 2
} xnn_transfer_trust_state;

typedef struct xnn_transfer_pairing_window_config {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  uint64_t duration_ms;
} xnn_transfer_pairing_window_config;

/*
 * peer_id must be a live native discovery observation. The caller cannot
 * provide an address, identity key, certificate, role, or transcript value.
 */
typedef struct xnn_transfer_pairing_start_request {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  uint64_t peer_id;
} xnn_transfer_pairing_start_request;

typedef struct xnn_transfer_pairing_attempt_ref {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  uint8_t attempt_id[XNN_TRANSFER_PAIRING_ATTEMPT_ID_SIZE];
} xnn_transfer_pairing_attempt_ref;

typedef struct xnn_transfer_trust_ref {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  uint64_t trust_id;
} xnn_transfer_trust_ref;

/*
 * SAS indices are display-only and present only while state is
 * AWAITING_CONFIRMATION. attempt_id is a random native-selected capability;
 * it is invalid for commands after a terminal event.
 */
typedef struct xnn_transfer_pairing_attempt_event_payload {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t state;
  uint64_t peer_id;
  uint64_t deadline_ms;
  uint8_t attempt_id[XNN_TRANSFER_PAIRING_ATTEMPT_ID_SIZE];
  uint16_t sas_word_indices[XNN_TRANSFER_PAIRING_SAS_WORD_COUNT];
  uint16_t sas_word_count;
  uint16_t error;
  uint16_t reserved;
} xnn_transfer_pairing_attempt_event_payload;

/*
 * trust_id is process-local and opaque. No device identifier, public key,
 * fingerprint, profile floor, or persisted-record metadata crosses this ABI.
 */
typedef struct xnn_transfer_trust_event_payload {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t state;
  uint64_t trust_id;
  uint64_t peer_id;
  uint32_t reserved;
  uint32_t reserved2;
} xnn_transfer_trust_event_payload;

typedef struct xnn_transfer_pairing_snapshot {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  uint64_t snapshot_revision;
  uint32_t has_attempt;
  uint32_t reserved2;
  xnn_transfer_pairing_attempt_event_payload attempt;
} xnn_transfer_pairing_snapshot;

typedef struct xnn_transfer_trust_snapshot_page {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  uint64_t snapshot_revision;
  uint32_t offset;
  uint32_t count;
  uint32_t total_count;
  uint32_t reserved2;
  xnn_transfer_trust_event_payload records[XNN_TRANSFER_TRUST_SNAPSHOT_PAGE_CAPACITY];
} xnn_transfer_trust_snapshot_page;

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

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_discovery_start(
    xnn_transfer_engine* engine, const xnn_transfer_discovery_config* config);

XNN_TRANSFER_API xnn_transfer_status
xnn_transfer_discovery_stop(xnn_transfer_engine* engine);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_discovery_get_snapshot(
    xnn_transfer_engine* engine, uint64_t expected_revision, uint32_t offset,
    xnn_transfer_discovery_snapshot_page* out_page);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_pairing_open_window(
    xnn_transfer_engine* engine, const xnn_transfer_pairing_window_config* config);

XNN_TRANSFER_API xnn_transfer_status
xnn_transfer_pairing_close_window(xnn_transfer_engine* engine);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_pairing_start(
    xnn_transfer_engine* engine, const xnn_transfer_pairing_start_request* request);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_pairing_confirm(
    xnn_transfer_engine* engine, const xnn_transfer_pairing_attempt_ref* attempt);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_pairing_reject(
    xnn_transfer_engine* engine, const xnn_transfer_pairing_attempt_ref* attempt);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_pairing_revoke(
    xnn_transfer_engine* engine, const xnn_transfer_trust_ref* trust);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_pairing_get_snapshot(
    xnn_transfer_engine* engine, xnn_transfer_pairing_snapshot* out_snapshot);

XNN_TRANSFER_API xnn_transfer_status xnn_transfer_trust_get_snapshot(
    xnn_transfer_engine* engine, uint64_t expected_revision, uint32_t offset,
    xnn_transfer_trust_snapshot_page* out_page);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XNN_TRANSFER_C_API_H_
