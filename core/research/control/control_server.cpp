#include "research/control/control_server.h"

#include "log/Log.h"
#include "oslib/oslib.h"

#include <asio.hpp>

#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace research::control
{

struct ControlServer::Impl
{
	Impl(std::uint16_t requestedPort, std::string token, ControlActions actions)
		: token(std::move(token)), actions(std::move(actions)),
			acceptor(io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), requestedPort))
	{
		acceptor.set_option(asio::socket_base::reuse_address(true));
		boundPort = acceptor.local_endpoint().port();
		accept();
		thread = std::thread([this] {
			ThreadName _("ResearchControl");
			try
			{
				io.run();
			}
			catch (const std::exception& exception)
			{
				ERROR_LOG(COMMON, "Research control server stopped: %s", exception.what());
			}
		});
	}

	~Impl()
	{
		io.stop();
		if (thread.joinable())
			thread.join();
	}

	class Session : public std::enable_shared_from_this<Session>
	{
	public:
		Session(Impl& server, asio::ip::tcp::socket socket)
			: server(server), socket(std::move(socket))
		{
		}

		void start()
		{
			readLine();
		}

	private:
		void readLine()
		{
			auto self = shared_from_this();
			asio::async_read_until(socket,
					asio::dynamic_buffer(buffer, server.limits.maxRequestBytes + 1), '\n',
					[this, self](const std::error_code& error, std::size_t length) {
						if (error)
						{
							if (error != asio::error::eof && error != asio::error::operation_aborted
									&& error != asio::error::not_found)
								DEBUG_LOG(COMMON, "Research control read: %s", error.message().c_str());
							if (error == asio::error::not_found)
								writeLine(handleControlRequest(std::string(server.limits.maxRequestBytes + 1, ' '),
										server.token, server.actions, server.limits), false);
							return;
						}
						std::string line = buffer.substr(0, length);
						buffer.erase(0, length);
						while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
							line.pop_back();
						if (line.empty())
						{
							readLine();
							return;
						}
						writeLine(handleControlRequest(line, server.token, server.actions, server.limits), true);
					});
		}

		void writeLine(std::string response, bool continueReading)
		{
			auto self = shared_from_this();
			response.push_back('\n');
			outgoing = std::move(response);
			asio::async_write(socket, asio::buffer(outgoing),
					[this, self, continueReading](const std::error_code& error, std::size_t) {
						if (error || !continueReading)
							return;
						readLine();
					});
		}

		Impl& server;
		asio::ip::tcp::socket socket;
		std::string buffer;
		std::string outgoing;
	};

	void accept()
	{
		acceptor.async_accept([this](const std::error_code& error, asio::ip::tcp::socket socket) {
			if (!error)
			{
				try
				{
					std::make_shared<Session>(*this, std::move(socket))->start();
				}
				catch (const std::exception& exception)
				{
					WARN_LOG(COMMON, "Research control session failed: %s", exception.what());
				}
			}
			else if (error == asio::error::operation_aborted)
				return;
			accept();
		});
	}

	std::string token;
	ControlActions actions;
	ControlLimits limits;
	asio::io_context io;
	asio::ip::tcp::acceptor acceptor;
	std::uint16_t boundPort = 0;
	std::thread thread;
};

ControlServer::ControlServer(std::uint16_t port, std::string token, ControlActions actions)
	: impl(std::make_unique<Impl>(port, std::move(token), std::move(actions)))
{
}

ControlServer::~ControlServer() = default;

std::uint16_t ControlServer::port() const
{
	return impl->boundPort;
}

} // namespace research::control
