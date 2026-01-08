#include "attach.h"
#include "collector.h"
#include "link.h"
#include "protocol.h"

#include <crt_externs.h>
#include <string.h>
#include <unistd.h>

namespace
{
    ViewerLink s_control;

    const char* AppName()
    {
        int count = *_NSGetArgc();
        char** args = *_NSGetArgv();
        for (int i = 1; i < count; i++)
        {
            size_t length = strlen(args[i]);
            if (length > 4 && strcmp(args[i] + length - 4, ".dll") == 0)
                return args[i];
        }
        return count > 0 ? args[0] : "";
    }

    void Run(void*)
    {
        for (;;)
        {
            // Retry once a second. The sleep comes first so that the runtime can finish starting.
            sleep(1);
            if (!s_control.Connect())
                continue;
            const char* name = AppName();
            size_t length = strlen(name);
            s_control.Begin(Protocol::Attach);
            s_control.U32((uint32_t)getpid());
            s_control.Append(name, length < 4096 ? length : 4096);
            s_control.End();
            s_control.Flush();

            const uint8_t* payload;
            size_t size;
            while (uint8_t type = s_control.Receive(&payload, &size))
            {
                if (type == Protocol::Collect)
                    Collector::Collect(false);
            }
            s_control.Close();
        }
    }
}

void Attach::Start()
{
    if (!ViewerLink::Enabled())
        return;
    // "false" makes the thread not suspendable, as the stock server GC does for its own threads.
    // It never runs managed code, so a collection has no reason to wait for it.
    if (!g_ee->CreateThread(Run, nullptr, false, "manualgc viewer"))
        LogLine("could not start the viewer thread; only GC.Collect() will start collections");
}
