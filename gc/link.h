// The connection to the viewer. See docs/protocol.md.
#pragma once

#include "platform.h"

class ViewerLink
{
public:
    // False when MANUALGC_PORT turns the viewer off.
    static bool Enabled();

    // Returns false at once when no viewer is listening.
    bool Connect();
    void Close();
    bool Connected() const { return m_socket >= 0; }

    // Each message is built between Begin and End. After an I/O error the link is closed and
    // everything else does nothing.
    void Begin(uint8_t type);
    void U8(uint8_t value) { Append(&value, sizeof(value)); }
    void U16(uint16_t value) { Append(&value, sizeof(value)); }
    void U32(uint32_t value) { Append(&value, sizeof(value)); }
    void U64(uint64_t value) { Append(&value, sizeof(value)); }
    void Append(const void* data, size_t size);
    void End();
    void Flush();

    // Blocks until a whole message arrives, then returns its type and payload. The payload stays
    // valid until the next call. Returns 0 when the viewer is gone.
    uint8_t Receive(const uint8_t** payload, size_t* size);

private:
    bool Read(void* data, size_t size);

    int m_socket = -1;
    size_t m_used = 0;
    size_t m_messageStart = 0;
    uint8_t m_buffer[256 * 1024];
    uint8_t m_received[64 * 1024];
};
