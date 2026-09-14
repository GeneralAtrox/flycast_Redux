#pragma once

// Loopback TCP server speaking the JSON-lines control protocol. One thread,
// any number of clients; each request is answered before the next line of
// that connection is read.

#include "research/control/control_protocol.h"

#include <cstdint>
#include <memory>
#include <string>

namespace research::control
{

class ControlServer
{
public:
	// Binds 127.0.0.1:port immediately (throws on failure) and starts serving.
	ControlServer(std::uint16_t port, std::string token, ControlActions actions);
	~ControlServer();
	ControlServer(const ControlServer&) = delete;
	ControlServer& operator=(const ControlServer&) = delete;

	std::uint16_t port() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace research::control
