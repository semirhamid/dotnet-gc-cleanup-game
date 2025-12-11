// The two exports the runtime looks up: GC_VersionInfo and GC_Initialize (gcinterface.h:1169-1178).
#include "gcheap.h"
#include "handles.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GC_EXPORT extern "C" __attribute__((visibility("default")))

IGCToCLR* g_ee;

// Declared by the vendored gcenv.object.h. The MethodTable flags here are the .NET 9+ ones.
bool g_oldMethodTableFlags = false;

static uint32_t s_eeMajorVersion;

bool LogEnabled()
{
    static const bool enabled = [] {
        const char* value = getenv("MANUALGC_LOG");
        return value != nullptr && strcmp(value, "0") != 0;
    }();
    return enabled;
}

void LogLine(const char* format, ...)
{
    if (!LogEnabled())
        return;
    char buffer[512];
    int prefix = snprintf(buffer, sizeof(buffer), "[manualgc %d] ", getpid());
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + prefix, sizeof(buffer) - (size_t)prefix, format, args);
    va_end(args);
    fprintf(stderr, "%s\n", buffer);
}

GC_EXPORT void LOCALGC_CALLCONV GC_VersionInfo(VersionInfo* info)
{
    // On entry the runtime has stored its own EE interface version (vm/gcheaputilities.cpp:270).
    s_eeMajorVersion = info->MajorVersion;
    info->MajorVersion = GC_INTERFACE_MAJOR_VERSION;
    info->MinorVersion = GC_INTERFACE_MINOR_VERSION;
    info->BuildVersion = 0;
    info->Name = "manualgc";
}

GC_EXPORT HRESULT LOCALGC_CALLCONV GC_Initialize(IGCToCLR* clrToGC, IGCHeap** gcHeap,
                                                 IGCHandleManager** gcHandleManager, GcDacVars*)
{
    g_ee = clrToGC;

    // The loader only rejects older GCs, so a newer runtime has to be refused here.
    if (s_eeMajorVersion != EE_INTERFACE_MAJOR_VERSION)
    {
        // LogErrorToHost needs EE interface 1 or later (gcinterface.ee.h:462-464).
        if (s_eeMajorVersion >= 1)
            g_ee->LogErrorToHost("manualgc: built for .NET 10 (EE interface 4) and refuses to run on another version");
        return (HRESULT)E_FAIL;
    }

    bool serverGC = false;
    if (g_ee->GetBooleanConfigValue("gcServer", "System.GC.Server", &serverGC) && serverGC)
    {
        g_ee->LogErrorToHost("manualgc: server GC is not supported; set DOTNET_gcServer=0");
        return (HRESULT)E_FAIL;
    }

    // Static storage and no destructors: both must outlive every runtime thread.
    alignas(ManualGCHeap) static uint8_t heapMemory[sizeof(ManualGCHeap)];
    *gcHeap = new (heapMemory) ManualGCHeap();
    *gcHandleManager = Handles::CreateManager();
    // GcDacVars is left as the runtime zeroed it, so SOS and dotnet-dump cannot read this heap.
    return S_OK;
}
