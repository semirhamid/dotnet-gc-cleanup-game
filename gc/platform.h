// Common includes for the GC. Line references such as "gcinterface.h:923" point into
// third_party/dotnet-runtime/src/coreclr/gc (dotnet/runtime tag v10.0.14).
#pragma once

#include "common.h"
#include "gcenv.h"
#include "gcinterface.h"

static_assert(GC_INTERFACE_MAJOR_VERSION == 5 && GC_INTERFACE_MINOR_VERSION == 5,
              "written against GC interface 5.5 (gcinterface.h:9,14)");
static_assert(EE_INTERFACE_MAJOR_VERSION == 4, "written against EE interface 4 (gcinterface.h:18)");
static_assert(sizeof(gc_alloc_context) == 56, "gc_alloc_context layout (gcinterface.h:201-225)");
static_assert(min_obj_size == 24, "minimum object size (gcinterface.h:250)");

// Set from GC_Initialize.
extern IGCToCLR* g_ee;

// MANUALGC_LOG: 0 off, 1 summaries, 2 also counts per type.
int LogLevel();
void LogLine(const char* format, ...) __attribute__((format(printf, 1, 2)));
// Reports through the host and ends the process. For broken invariants only.
[[noreturn]] void Fatal(const char* format, ...) __attribute__((format(printf, 1, 2)));
