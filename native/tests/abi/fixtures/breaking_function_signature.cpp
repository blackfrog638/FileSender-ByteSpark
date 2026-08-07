#define xnn_transfer_engine_start xnn_transfer_engine_start_v1_original
#include "../v1/c_api.h"
#undef xnn_transfer_engine_start

extern "C" XNN_TRANSFER_API void xnn_transfer_engine_start(xnn_transfer_engine* engine);

#include "../v1_compat_assertions.hpp"

int main() { return 0; }
