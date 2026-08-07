#include "../v1/c_api.h"

#undef XNN_TRANSFER_EVENT_QUEUE_CAPACITY
#define XNN_TRANSFER_EVENT_QUEUE_CAPACITY 65u

#include "../v1_compat_assertions.hpp"

int main() { return 0; }
