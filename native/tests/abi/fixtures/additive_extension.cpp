#define xnn_transfer_engine_config xnn_transfer_engine_config_v1_original
#define xnn_transfer_engine_create xnn_transfer_engine_create_v1_original
#include "../v1/c_api.h"
#undef xnn_transfer_engine_create
#undef xnn_transfer_engine_config

typedef struct xnn_transfer_engine_config {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t reserved;
  uint64_t additive_tail;
} xnn_transfer_engine_config;

extern "C" XNN_TRANSFER_API xnn_transfer_status xnn_transfer_engine_create(
    const xnn_transfer_engine_config* config, xnn_transfer_engine** out_engine);
extern "C" XNN_TRANSFER_API uint32_t xnn_transfer_additive_symbol(void);

#include "../v1_compat_assertions.hpp"

int main() { return 0; }
