#include <cstdint>
#include <cstring>
#include <iostream>

#include "v1/c_api.h"

namespace {

void Wakeup(void* user_data) {
  auto* const wakeups = static_cast<std::uint32_t*>(user_data);
  ++(*wakeups);
}

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "ABI v1 legacy client failure: " << message << '\n';
  }
  return condition;
}

bool DrainStates(xnn_transfer_engine* engine, std::uint32_t* state_mask) {
  for (;;) {
    xnn_transfer_event event{
        .struct_size = sizeof(xnn_transfer_event),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
    };
    const xnn_transfer_status status = xnn_transfer_engine_poll_event(engine, &event);
    if (status == XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY) {
      return true;
    }
    if (!Expect(status == XNN_TRANSFER_STATUS_OK, "poll_event failed")) {
      return false;
    }
    if (!Expect(event.type == XNN_TRANSFER_EVENT_TYPE_ENGINE_STATE_CHANGED,
                "unexpected event type") ||
        !Expect(
            event.payload_version == XNN_TRANSFER_ENGINE_STATE_EVENT_PAYLOAD_VERSION,
            "unexpected payload version") ||
        !Expect(event.payload_size == sizeof(xnn_transfer_engine_state_event_payload),
                "unexpected state payload size")) {
      return false;
    }

    xnn_transfer_engine_state_event_payload payload{};
    std::memcpy(&payload, event.payload, sizeof(payload));
    if (!Expect(payload.struct_size == sizeof(payload),
                "unexpected payload struct size") ||
        !Expect(payload.abi_version == XNN_TRANSFER_ABI_VERSION,
                "unexpected payload ABI version") ||
        !Expect(payload.state <= XNN_TRANSFER_ENGINE_STATE_STOPPING,
                "unexpected engine state")) {
      return false;
    }
    *state_mask |= 1u << payload.state;
  }
}

}  // namespace

int main() {
  if (!Expect(xnn_transfer_abi_version() == XNN_TRANSFER_ABI_VERSION,
              "runtime ABI version changed")) {
    return 1;
  }

  xnn_transfer_engine_config config{
      .struct_size = sizeof(xnn_transfer_engine_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
  xnn_transfer_engine* engine = nullptr;
  if (!Expect(xnn_transfer_engine_create(&config, &engine) == XNN_TRANSFER_STATUS_OK,
              "engine_create rejected the frozen v1 config") ||
      !Expect(engine != nullptr, "engine_create returned null")) {
    return 1;
  }

  std::uint32_t wakeups = 0;
  xnn_transfer_event_callback_config callback_config{
      .struct_size = sizeof(xnn_transfer_event_callback_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .callback = Wakeup,
      .user_data = &wakeups,
  };
  bool passed =
      Expect(xnn_transfer_engine_set_event_callback(engine, &callback_config) ==
                 XNN_TRANSFER_STATUS_OK,
             "callback registration failed") &&
      Expect(wakeups > 0, "queued CREATED event did not wake the v1 callback");

  std::uint32_t state_mask = 0;
  passed = DrainStates(engine, &state_mask) && passed;

  passed = Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK,
                  "engine_start failed") &&
           passed;
  xnn_transfer_engine_state state = XNN_TRANSFER_ENGINE_STATE_CREATED;
  passed =
      Expect(xnn_transfer_engine_get_state(engine, &state) == XNN_TRANSFER_STATUS_OK,
             "engine_get_state failed") &&
      Expect(state == XNN_TRANSFER_ENGINE_STATE_RUNNING,
             "engine did not enter RUNNING") &&
      passed;

  passed = Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK,
                  "engine_stop failed") &&
           passed;
  passed = DrainStates(engine, &state_mask) && passed;
  passed = Expect((state_mask & (1u << XNN_TRANSFER_ENGINE_STATE_CREATED)) != 0,
                  "CREATED event missing") &&
           Expect((state_mask & (1u << XNN_TRANSFER_ENGINE_STATE_RUNNING)) != 0,
                  "RUNNING event missing") &&
           Expect((state_mask & (1u << XNN_TRANSFER_ENGINE_STATE_STOPPING)) != 0,
                  "STOPPING event missing") &&
           Expect((state_mask & (1u << XNN_TRANSFER_ENGINE_STATE_STOPPED)) != 0,
                  "STOPPED event missing") &&
           passed;

  xnn_transfer_engine_destroy(engine);
  return passed ? 0 : 1;
}
