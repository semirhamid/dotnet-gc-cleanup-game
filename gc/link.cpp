#include "link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int ViewerPort()
{
    const char* value = getenv("MANUALGC_PORT");
    return value == nullptr ? 5710 : atoi(value);
}

bool ViewerLink::Connect()
{
    int port = ViewerPort();
    if (port <= 0 || port > 65535)
        return false;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return false;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    // A viewer that stops reading must not freeze the app for good.
    timeval timeout = {10, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&address, sizeof(address)) != 0)
    {
        close(s);
        return false;
    }
    m_socket = s;
    m_used = 0;
    return true;
}

void ViewerLink::Close()
{
    if (m_socket >= 0)
        close(m_socket);
    m_socket = -1;
}

void ViewerLink::Begin(uint8_t type)
{
    // Leave room for the length; End fills it in. Messages are kept well below the buffer size.
    if (m_used > sizeof(m_buffer) / 2)
        Flush();
    m_messageStart = m_used;
    m_used += sizeof(uint32_t);
    U8(type);
}

void ViewerLink::Append(const void* data, size_t size)
{
    if (m_used + size > sizeof(m_buffer))
        Fatal("viewer message too large");
    memcpy(m_buffer + m_used, data, size);
    m_used += size;
}

void ViewerLink::End()
{
    uint32_t length = (uint32_t)(m_used - m_messageStart - sizeof(uint32_t));
    memcpy(m_buffer + m_messageStart, &length, sizeof(length));
}

void ViewerLink::Flush()
{
    for (size_t sent = 0; m_socket >= 0 && sent < m_used;)
    {
        ssize_t n = send(m_socket, m_buffer + sent, m_used - sent, 0);
        if (n > 0)
            sent += (size_t)n;
        else if (errno != EINTR)
        {
            LogLine("lost the viewer while sending: %s", strerror(errno));
            Close();
        }
    }
    m_used = 0;
}
