#pragma once

#include "guest_abi/dispatch.h"

// A stage module's translated functions. All modules are linked to the same guest window
// (see tools/rso_link.py), so each has its own table and only one is active at a time
// (runtime/src/hle/os/os_rso.cpp). Records are sorted by address.
struct ModuleCodeSet {
    const char* name;
    uint32_t windowBase;
    uint32_t windowSize;
    const BulkTranslatedFunctionRecord* records;
    size_t count;
    uint32_t prolog;      // absolute addresses inside the window (RSO header fields after linking)
    uint32_t epilog;
    uint32_t unresolved;
};

extern "C" const ModuleCodeSet* const g_moduleCodeSets[];
extern "C" const unsigned long long g_moduleCodeSetCount;
