// A small HTTP and WebSocket server, for serving the control surface to a
// tablet on the same network.
//
// One thread, one select() loop, no dependencies. That is a deliberate ceiling
// rather than a shortcut: this serves one page and a handful of long-lived
// sockets to devices in the same room, and anything built for more concurrency
// than that would be more code to be wrong in exchange for nothing.
//
// It is worth being explicit about what this is not. There is no TLS, no
// authentication and no attempt at hardening, and it binds to the local
// network. It is for a laptop and an iPad on a private network at a venue, and
// it should not be exposed to the internet. The application says as much next
// to the switch that turns it on.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acm::net {

struct HttpResponse {
    int status = 200;
    std::string contentType = "text/html; charset=utf-8";
    std::string body;
};

class HttpServer {
public:
    ~HttpServer();

    // Binds and starts serving. Returns false and fills `error` when the port
    // is taken or the socket layer refuses to start.
    bool start(std::uint16_t port, std::string* error = nullptr);
    void stop();

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    std::uint16_t port() const noexcept { return port_; }
    int clientCount() const noexcept { return clientCount_.load(std::memory_order_relaxed); }

    // Answers an ordinary GET. Called on the server thread, so it must not
    // touch the graph - the control server hands back a string prepared
    // earlier.
    std::function<HttpResponse(const std::string& path)> onRequest;

    // -- WebSocket ---------------------------------------------------------
    // Queues a text message for every connected socket. Safe from any thread.
    void broadcast(std::string text);

    // The message every newly connected socket is sent before anything else.
    //
    // Without one, a client that connects between changes sits looking at
    // nothing: the state is only broadcast when it differs from the last
    // broadcast, and a socket that was not there for that never hears it.
    void setGreeting(std::string text);

    // Takes everything received since the last call. Drained once per frame on
    // the message thread, which is the only place incoming messages are allowed
    // to change anything.
    std::vector<std::string> takeReceived();

    // The addresses this machine can be reached on, for showing next to the
    // port. Loopback is excluded: nobody types 127.0.0.1 into a tablet.
    static std::vector<std::string> localAddresses();

private:
    void threadMain();

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
    std::atomic<int> clientCount_{ 0 };
    std::uint16_t port_ = 0;

    std::mutex outgoingMutex_;
    std::vector<std::string> outgoing_;

    std::mutex greetingMutex_;
    std::string greeting_;

    std::mutex incomingMutex_;
    std::vector<std::string> incoming_;
};

} // namespace acm::net
