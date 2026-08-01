// Serving the control surface to a tablet.
//
// The page is the same surface the Control tab shows, laid out on the same
// grid, and touching a control there does exactly what touching it on the
// laptop does. That is the whole feature: a performer standing away from the
// machine with the eight controls that matter under their hands.
//
// The protocol is deliberately small. The server sends the layout once on
// connection and again whenever it changes, and a value as it moves; the
// client sends a value as it moves. Nothing else. There is no attempt to be a
// general remote-control protocol, because a general protocol here would be a
// second surface model to keep in step with the first.
//
// State flows one way per control at a time. A value arriving from a tablet is
// applied on the message thread with the same call the local knob makes, and
// the resulting layout goes back out to every *other* client - never to the one
// that sent it, which would fight its own finger.
#pragma once

#include "../control/Surface.h"
#include "../core/Engine.h"
#include "HttpServer.h"

#include <string>

namespace acm::net {

class ControlServer {
public:
    void initialise(Engine* engine, control::Surface* surface);

    bool start(std::uint16_t port, std::string* error = nullptr);
    void stop();
    bool running() const noexcept { return server_.running(); }
    std::uint16_t port() const noexcept { return server_.port(); }
    int clientCount() const noexcept { return server_.clientCount(); }

    // Applies anything received and pushes the layout when it has changed.
    // Called once per frame from the message thread.
    void serviceFromMessageThread();

    // The URLs a tablet can be pointed at.
    std::vector<std::string> addresses() const;

private:
    std::string layoutJson() const;

    Engine* engine_ = nullptr;
    control::Surface* surface_ = nullptr;
    HttpServer server_;

    // What was last sent, so a layout only goes out when it is different. A
    // surface published every frame would be a few kilobytes sixty times a
    // second to a device on a phone network.
    std::string lastLayout_;
    float sinceBroadcast_ = 0.0f;
};

} // namespace acm::net
