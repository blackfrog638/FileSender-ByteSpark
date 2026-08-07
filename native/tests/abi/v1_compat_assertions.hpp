#ifndef XNN_TRANSFER_TEST_ABI_V1_COMPAT_ASSERTIONS_HPP_
#define XNN_TRANSFER_TEST_ABI_V1_COMPAT_ASSERTIONS_HPP_

#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(sizeof(void*) == 8);
static_assert(sizeof(std::size_t) == 8);

static_assert(XNN_TRANSFER_ABI_VERSION == 1u);
static_assert(XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE == 256u);
static_assert(XNN_TRANSFER_EVENT_QUEUE_CAPACITY == 64u);
static_assert(XNN_TRANSFER_ENGINE_STATE_EVENT_PAYLOAD_VERSION == 1u);

static_assert(XNN_TRANSFER_STATUS_OK == 0);
static_assert(XNN_TRANSFER_STATUS_INVALID_ARGUMENT == 1);
static_assert(XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI == 2);
static_assert(XNN_TRANSFER_STATUS_INVALID_STATE == 3);
static_assert(XNN_TRANSFER_STATUS_INTERNAL_ERROR == 4);
static_assert(XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY == 5);
static_assert(sizeof(xnn_transfer_status) == 4);

static_assert(XNN_TRANSFER_ENGINE_STATE_CREATED == 0);
static_assert(XNN_TRANSFER_ENGINE_STATE_RUNNING == 1);
static_assert(XNN_TRANSFER_ENGINE_STATE_STOPPED == 2);
static_assert(XNN_TRANSFER_ENGINE_STATE_STOPPING == 3);
static_assert(sizeof(xnn_transfer_engine_state) == 4);

static_assert(XNN_TRANSFER_EVENT_TYPE_ENGINE_STATE_CHANGED == 1);
static_assert(sizeof(xnn_transfer_event_type) == 4);
static_assert(XNN_TRANSFER_EVENT_FLAG_NONE == 0);
static_assert(XNN_TRANSFER_EVENT_FLAG_EVENTS_DROPPED_BEFORE == 1);
static_assert(sizeof(xnn_transfer_event_flags) == 4);

static_assert(std::is_standard_layout_v<xnn_transfer_engine_config>);
static_assert(alignof(xnn_transfer_engine_config) == 8);
static_assert(sizeof(xnn_transfer_engine_config) >= 16);
static_assert(offsetof(xnn_transfer_engine_config, struct_size) == 0);
static_assert(offsetof(xnn_transfer_engine_config, abi_version) == 8);
static_assert(offsetof(xnn_transfer_engine_config, reserved) == 12);
static_assert(
    std::is_same_v<decltype(xnn_transfer_engine_config::struct_size), size_t>);
static_assert(
    std::is_same_v<decltype(xnn_transfer_engine_config::abi_version), uint32_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_engine_config::reserved), uint32_t>);

static_assert(std::is_same_v<xnn_transfer_event_wakeup_callback, void (*)(void*)>);
static_assert(std::is_standard_layout_v<xnn_transfer_event_callback_config>);
static_assert(alignof(xnn_transfer_event_callback_config) == 8);
static_assert(sizeof(xnn_transfer_event_callback_config) >= 32);
static_assert(offsetof(xnn_transfer_event_callback_config, struct_size) == 0);
static_assert(offsetof(xnn_transfer_event_callback_config, abi_version) == 8);
static_assert(offsetof(xnn_transfer_event_callback_config, reserved) == 12);
static_assert(offsetof(xnn_transfer_event_callback_config, callback) == 16);
static_assert(offsetof(xnn_transfer_event_callback_config, user_data) == 24);
static_assert(
    std::is_same_v<decltype(xnn_transfer_event_callback_config::struct_size), size_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_event_callback_config::abi_version),
                             uint32_t>);
static_assert(
    std::is_same_v<decltype(xnn_transfer_event_callback_config::reserved), uint32_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_event_callback_config::callback),
                             xnn_transfer_event_wakeup_callback>);
static_assert(
    std::is_same_v<decltype(xnn_transfer_event_callback_config::user_data), void*>);

static_assert(std::is_standard_layout_v<xnn_transfer_event>);
static_assert(alignof(xnn_transfer_event) == 8);
static_assert(sizeof(xnn_transfer_event) >= 296);
static_assert(offsetof(xnn_transfer_event, struct_size) == 0);
static_assert(offsetof(xnn_transfer_event, abi_version) == 8);
static_assert(offsetof(xnn_transfer_event, type) == 12);
static_assert(offsetof(xnn_transfer_event, sequence) == 16);
static_assert(offsetof(xnn_transfer_event, payload_version) == 24);
static_assert(offsetof(xnn_transfer_event, payload_size) == 28);
static_assert(offsetof(xnn_transfer_event, flags) == 32);
static_assert(offsetof(xnn_transfer_event, reserved) == 36);
static_assert(offsetof(xnn_transfer_event, payload) == 40);
static_assert(std::is_same_v<decltype(xnn_transfer_event::struct_size), size_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_event::abi_version), uint32_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_event::type), uint32_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_event::sequence), uint64_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_event::payload_version), uint32_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_event::payload_size), uint32_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_event::flags), uint32_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_event::reserved), uint32_t>);
static_assert(std::is_same_v<decltype(xnn_transfer_event::payload),
                             uint8_t[XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE]>);

static_assert(std::is_standard_layout_v<xnn_transfer_engine_state_event_payload>);
static_assert(alignof(xnn_transfer_engine_state_event_payload) == 8);
static_assert(sizeof(xnn_transfer_engine_state_event_payload) >= 24);
static_assert(offsetof(xnn_transfer_engine_state_event_payload, struct_size) == 0);
static_assert(offsetof(xnn_transfer_engine_state_event_payload, abi_version) == 8);
static_assert(offsetof(xnn_transfer_engine_state_event_payload, state) == 12);
static_assert(offsetof(xnn_transfer_engine_state_event_payload, reserved) == 16);
static_assert(std::is_same_v<
              decltype(xnn_transfer_engine_state_event_payload::struct_size), size_t>);
static_assert(
    std::is_same_v<decltype(xnn_transfer_engine_state_event_payload::abi_version),
                   uint32_t>);
static_assert(
    std::is_same_v<decltype(xnn_transfer_engine_state_event_payload::state), uint32_t>);
static_assert(std::is_same_v<
              decltype(xnn_transfer_engine_state_event_payload::reserved), uint32_t>);

using xnn_transfer_abi_version_v1_fn = uint32_t (*)(void);
using xnn_transfer_engine_create_v1_fn =
    xnn_transfer_status (*)(const xnn_transfer_engine_config*, xnn_transfer_engine**);
using xnn_transfer_engine_destroy_v1_fn = void (*)(xnn_transfer_engine*);
using xnn_transfer_engine_start_v1_fn = xnn_transfer_status (*)(xnn_transfer_engine*);
using xnn_transfer_engine_stop_v1_fn = xnn_transfer_status (*)(xnn_transfer_engine*);
using xnn_transfer_engine_get_state_v1_fn =
    xnn_transfer_status (*)(const xnn_transfer_engine*, xnn_transfer_engine_state*);
using xnn_transfer_engine_set_event_callback_v1_fn = xnn_transfer_status (*)(
    xnn_transfer_engine*, const xnn_transfer_event_callback_config*);
using xnn_transfer_engine_poll_event_v1_fn =
    xnn_transfer_status (*)(xnn_transfer_engine*, xnn_transfer_event*);

static_assert(std::is_same_v<decltype(&xnn_transfer_abi_version),
                             xnn_transfer_abi_version_v1_fn>);
static_assert(std::is_same_v<decltype(&xnn_transfer_engine_create),
                             xnn_transfer_engine_create_v1_fn>);
static_assert(std::is_same_v<decltype(&xnn_transfer_engine_destroy),
                             xnn_transfer_engine_destroy_v1_fn>);
static_assert(std::is_same_v<decltype(&xnn_transfer_engine_start),
                             xnn_transfer_engine_start_v1_fn>);
static_assert(std::is_same_v<decltype(&xnn_transfer_engine_stop),
                             xnn_transfer_engine_stop_v1_fn>);
static_assert(std::is_same_v<decltype(&xnn_transfer_engine_get_state),
                             xnn_transfer_engine_get_state_v1_fn>);
static_assert(std::is_same_v<decltype(&xnn_transfer_engine_set_event_callback),
                             xnn_transfer_engine_set_event_callback_v1_fn>);
static_assert(std::is_same_v<decltype(&xnn_transfer_engine_poll_event),
                             xnn_transfer_engine_poll_event_v1_fn>);

#endif  // XNN_TRANSFER_TEST_ABI_V1_COMPAT_ASSERTIONS_HPP_
