#include "attach.h"
#include "collector.h"
#include "link.h"

#include <crt_externs.h>
#include <string.h>
#include <unistd.h>

namespace
{
    ViewerLink s_control;

    // The app's dll if the command line names one, else the program.
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
            // Also the wait between attempts, and before the first, so the runtime gets to start.
            sleep(1);
            if (!s_control.Connect())
                continue;
            const char* name = AppName();
            size_t length = strlen(name);
            s_control.Begin(9);
            s_control.U32((uint32_t)getpid());
            s_control.Append(name, length < 4096 ? length : 4096);
            s_control.End();
            s_control.Flush();

            const uint8_t* payload;
            size_t size;
            while (uint8_t type = s_control.Receive(&payload, &size))
                if (type == 66)
                    Collector::Collect(false);
            s_control.Close();
        }
    }
}

void Attach::Start()
{
    if (!ViewerLink::Enabled())
        return;
    // Not suspendable, like the stock server GC's threads, which also suspend the EE: it runs no
    // managed code, so a collection need not wait for it.
    if (!g_ee->CreateThread(Run, nullptr, false, "manualgc viewer"))
        LogLine("could not start the viewer thread; only GC.Collect() will start collections");
}
