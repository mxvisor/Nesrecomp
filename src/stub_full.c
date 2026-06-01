/* stub_full.c — interpreter-only mode, no generated code */
#include "runner.h"

void call_by_address(uint16_t addr) {
    cpu_interp_run(addr);
}
