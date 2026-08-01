#include "HttpServer.h"

#include "WebSocket.h"

#include <algorithm>
#include <cstdio>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

namespace acm::net {
namespace {

// One process-wide Winsock initialisation, released when the last server
// stops. Two servers is not a case anyone needs, but a start/stop cycle very
// much is, and calling WSACleanup while another is running would break it.
std::mutex g_winsockMutex;
int g_winsockUsers = 0;

bool acquireWinsock(std::string* error) {
    const std::lock_guard<std::mutex> lock(g_winsockMutex);
    if (g_winsockUsers++ > 0) return true;

    WSADATA data{};
    const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        --g_winsockUsers;
        if (error) *error = "could not start Winsock (" + std::to_string(result) + ")";
        return false;
    }
    return true;
}

void releaseWinsock() {
    const std::lock_guard<std::mutex> lock(g_winsockMutex);
    if (--g_winsockUsers <= 0) {
        g_winsockUsers = 0;
        ::WSACleanup();
    }
}

std::string headerValue(const std::string& request, const std::string& name) {
    // Case-insensitive, because a header name is and browsers differ.
    const auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };

    const std::string haystack = lower(request);
    const std::string needle = lower(name) + ":";

    std::size_t position = haystack.find("\r\n" + needle);
    if (position == std::string::npos) return {};

    position += needle.size() + 2;
    const std::size_t end = request.find("\r\n", position);
    if (end == std::string::npos) return {};

    std::string value = request.substr(position, end - position);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.pop_back();
    return value;
}

struct Client {
    SOCKET socket = INVALID_SOCKET;
    bool upgraded = false;
    std::string inbound;
    std::string outbound;
};

bool sendAll(SOCKET socket, std::string& pending) {
    while (!pending.empty()) {
        const int sent = ::send(socket, pending.data(),
                                static_cast<int>(std::min<std::size_t>(pending.size(), 32768)), 0);
        if (sent > 0) {
            pending.erase(0, static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && ::WSAGetLastError() == WSAEWOULDBLOCK) return true;   // try later
        return false;
    }
    return true;
}

} // namespace

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(std::uint16_t port, std::string* error) {
    stop();

    if (!acquireWinsock(error)) return false;

    port_ = port;
    stopRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    // The listening socket is created on this thread so a bind failure is
    // reported to the caller rather than appearing later as "it just does not
    // work".
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        if (error) *error = "could not create a socket";
        running_.store(false, std::memory_order_release);
        releaseWinsock();
        return false;
    }

    // Without this a restart inside the TIME_WAIT window fails to bind, which
    // is exactly what happens when the switch is turned off and on again.
    BOOL reuse = TRUE;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = ::htons(port);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        if (error) *error = "port " + std::to_string(port) + " is already in use";
        ::closesocket(listener);
        running_.store(false, std::memory_order_release);
        releaseWinsock();
        return false;
    }

    if (::listen(listener, 8) == SOCKET_ERROR) {
        if (error) *error = "could not listen on port " + std::to_string(port);
        ::closesocket(listener);
        running_.store(false, std::memory_order_release);
        releaseWinsock();
        return false;
    }

    u_long nonBlocking = 1;
    ::ioctlsocket(listener, FIONBIO, &nonBlocking);

    thread_ = std::thread([this, listener] {
        std::vector<Client> clients;

        while (!stopRequested_.load(std::memory_order_acquire)) {
            fd_set readable;
            fd_set writable;
            FD_ZERO(&readable);
            FD_ZERO(&writable);
            FD_SET(listener, &readable);

            for (const Client& client : clients) {
                FD_SET(client.socket, &readable);
                if (!client.outbound.empty()) FD_SET(client.socket, &writable);
            }

            // A short timeout rather than a blocking wait, so stopping does not
            // have to wait for a connection to arrive first.
            timeval timeout{ 0, 50000 };
            const int ready = ::select(0, &readable, &writable, nullptr, &timeout);

            // -- outgoing ---------------------------------------------------
            std::vector<std::string> pending;
            {
                const std::lock_guard<std::mutex> lock(outgoingMutex_);
                pending.swap(outgoing_);
            }
            for (const std::string& message : pending) {
                const std::string frame = websocket::encode(websocket::Opcode::Text, message);
                for (Client& client : clients)
                    if (client.upgraded) client.outbound += frame;
            }

            if (ready < 0) continue;

            // -- accept -----------------------------------------------------
            if (FD_ISSET(listener, &readable)) {
                const SOCKET accepted = ::accept(listener, nullptr, nullptr);
                if (accepted != INVALID_SOCKET) {
                    u_long clientNonBlocking = 1;
                    ::ioctlsocket(accepted, FIONBIO, &clientNonBlocking);

                    // Small writes go out immediately. A control surface's
                    // whole value is that a knob moves when it is touched, and
                    // Nagle would hold a 20-byte message for 40 ms.
                    BOOL noDelay = TRUE;
                    ::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

                    Client client;
                    client.socket = accepted;
                    clients.push_back(std::move(client));
                }
            }

            // -- service ----------------------------------------------------
            for (std::size_t i = 0; i < clients.size();) {
                Client& client = clients[i];
                bool drop = false;

                if (FD_ISSET(client.socket, &readable)) {
                    char buffer[8192];
                    const int received = ::recv(client.socket, buffer, sizeof(buffer), 0);

                    if (received > 0) {
                        client.inbound.append(buffer, static_cast<std::size_t>(received));
                    } else if (received == 0) {
                        drop = true;
                    } else if (::WSAGetLastError() != WSAEWOULDBLOCK) {
                        drop = true;
                    }
                }

                // -- HTTP, or the upgrade to a WebSocket --------------------
                if (!drop && !client.upgraded) {
                    const std::size_t headerEnd = client.inbound.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        const std::string request = client.inbound.substr(0, headerEnd + 4);
                        client.inbound.erase(0, headerEnd + 4);

                        std::string path = "/";
                        const std::size_t pathStart = request.find(' ');
                        if (pathStart != std::string::npos) {
                            const std::size_t pathEnd = request.find(' ', pathStart + 1);
                            if (pathEnd != std::string::npos)
                                path = request.substr(pathStart + 1, pathEnd - pathStart - 1);
                        }

                        const std::string key = headerValue(request, "Sec-WebSocket-Key");
                        if (!key.empty()) {
                            client.outbound +=
                                "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: " + websocket::acceptKey(key) + "\r\n\r\n";
                            client.upgraded = true;
                            clientCount_.fetch_add(1, std::memory_order_relaxed);

                            // Straight after the handshake, so a client that
                            // arrives between changes still knows what it is
                            // looking at.
                            const std::lock_guard<std::mutex> lock(greetingMutex_);
                            if (!greeting_.empty())
                                client.outbound += websocket::encode(websocket::Opcode::Text,
                                                                     greeting_);
                        } else {
                            HttpResponse response;
                            if (onRequest) response = onRequest(path);
                            else response.status = 404;

                            char header[512];
                            std::snprintf(header, sizeof(header),
                                          "HTTP/1.1 %d %s\r\n"
                                          "Content-Type: %s\r\n"
                                          "Content-Length: %d\r\n"
                                          "Cache-Control: no-store\r\n"
                                          "Connection: close\r\n\r\n",
                                          response.status,
                                          response.status == 200 ? "OK" : "Not Found",
                                          response.contentType.c_str(),
                                          static_cast<int>(response.body.size()));

                            client.outbound += header;
                            client.outbound += response.body;

                            // Answered and finished with. Keep-alive would save
                            // a connection per page load, which is one.
                            if (!sendAll(client.socket, client.outbound)) drop = true;
                            else drop = true;
                        }
                    }
                }

                // -- WebSocket frames --------------------------------------
                if (!drop && client.upgraded) {
                    websocket::Frame frame;
                    while (websocket::decode(client.inbound, frame)) {
                        switch (frame.opcode) {
                            case websocket::Opcode::Text:
                            case websocket::Opcode::Binary: {
                                const std::lock_guard<std::mutex> lock(incomingMutex_);
                                incoming_.push_back(frame.payload);
                                break;
                            }
                            case websocket::Opcode::Ping:
                                client.outbound += websocket::encode(websocket::Opcode::Pong,
                                                                     frame.payload);
                                break;
                            case websocket::Opcode::Close:
                                client.outbound += websocket::encode(websocket::Opcode::Close, {});
                                sendAll(client.socket, client.outbound);
                                drop = true;
                                break;
                            default:
                                break;
                        }
                        if (drop) break;
                    }
                }

                if (!drop && !client.outbound.empty()) {
                    if (!sendAll(client.socket, client.outbound)) drop = true;
                }

                if (drop) {
                    if (client.upgraded) clientCount_.fetch_sub(1, std::memory_order_relaxed);
                    ::closesocket(client.socket);
                    clients.erase(clients.begin() + static_cast<long>(i));
                    continue;
                }
                ++i;
            }
        }

        for (Client& client : clients) ::closesocket(client.socket);
        ::closesocket(listener);
    });

    return true;
}

void HttpServer::stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    stopRequested_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();

    running_.store(false, std::memory_order_release);
    clientCount_.store(0, std::memory_order_relaxed);

    {
        const std::lock_guard<std::mutex> lock(outgoingMutex_);
        outgoing_.clear();
    }
    releaseWinsock();
}

void HttpServer::broadcast(std::string text) {
    if (!running_.load(std::memory_order_acquire)) return;

    const std::lock_guard<std::mutex> lock(outgoingMutex_);
    // Bounded, so a surface updating faster than a tablet can read it cannot
    // grow the queue without limit. The newest state is the one that matters,
    // so the oldest goes.
    if (outgoing_.size() > 64) outgoing_.erase(outgoing_.begin());
    outgoing_.push_back(std::move(text));
}

void HttpServer::setGreeting(std::string text) {
    const std::lock_guard<std::mutex> lock(greetingMutex_);
    greeting_ = std::move(text);
}

std::vector<std::string> HttpServer::takeReceived() {
    std::vector<std::string> out;
    const std::lock_guard<std::mutex> lock(incomingMutex_);
    out.swap(incoming_);
    return out;
}

std::vector<std::string> HttpServer::localAddresses() {
    std::vector<std::string> out;

    std::string error;
    if (!acquireWinsock(&error)) return out;

    char hostName[256] = {};
    if (::gethostname(hostName, sizeof(hostName)) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* results = nullptr;
        if (::getaddrinfo(hostName, nullptr, &hints, &results) == 0) {
            for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
                const auto* address = reinterpret_cast<sockaddr_in*>(entry->ai_addr);
                char text[INET_ADDRSTRLEN] = {};
                if (!::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) continue;

                // Loopback is excluded: nobody types 127.0.0.1 into a tablet.
                const std::string value = text;
                if (value.rfind("127.", 0) == 0) continue;
                if (std::find(out.begin(), out.end(), value) == out.end()) out.push_back(value);
            }
            ::freeaddrinfo(results);
        }
    }

    releaseWinsock();
    return out;
}

} // namespace acm::net
