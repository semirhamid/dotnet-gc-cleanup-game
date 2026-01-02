// Lets the viewer start a collection, for apps that never call GC.Collect() themselves.
#pragma once

namespace Attach
{
    // Starts a native thread that stays connected to the viewer and collects when told to.
    void Start();
}
