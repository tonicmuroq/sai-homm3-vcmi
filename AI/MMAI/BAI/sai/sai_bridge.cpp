/*
 * sai_bridge.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include <boost/asio.hpp>

// boost/asio.hpp pulls in Windows headers that define these; engine code uses both names.
#ifdef _WIN32
#	ifdef AUTO
#		undef AUTO
#	endif
#	ifdef IGNORE
#		undef IGNORE
#	endif
#endif

#include "sai_bridge.h"

namespace MMAI::BAI
{
namespace
{
	const char * DEFAULT_BIND = "127.0.0.1:18080";
	constexpr int DEFAULT_TIMEOUT_SECONDS = 300;

	std::string EnvOr(const char * name, const std::string & fallback)
	{
		const char * value = std::getenv(name);
		return (value != nullptr && *value != '\0') ? std::string(value) : fallback;
	}

	std::string Respond(int status, const std::string & body)
	{
		const char * reason = status == 200 ? "OK" : (status == 400 ? "Bad Request" : "Not Found");
		return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
			+ "Content-Type: application/json\r\n"
			+ "Content-Length: " + std::to_string(body.size()) + "\r\n"
			+ "Connection: close\r\n\r\n" + body;
	}
}

SaiBridge & SaiBridge::instance()
{
	static SaiBridge bridge;
	return bridge;
}

SaiBridge::SaiBridge()
	: bind(EnvOr("VCMI_SAI_ADDR", DEFAULT_BIND))
{
	try
	{
		timeoutSeconds = std::stoi(EnvOr("VCMI_SAI_TIMEOUT", std::to_string(DEFAULT_TIMEOUT_SECONDS)));
	}
	catch(const std::exception &)
	{
		timeoutSeconds = DEFAULT_TIMEOUT_SECONDS;
	}

	worker = std::thread([this] { serve(); });
}

SaiBridge::~SaiBridge()
{
	// The listener blocks in accept() and the process is going down anyway; joining
	// would hang, so the thread is deliberately abandoned.
	worker.detach();
}

std::string SaiBridge::address() const
{
	return bind;
}

SaiDecision SaiBridge::decide(std::string payload, const std::vector<bool> & mask, int castCount)
{
	auto queued = std::lock_guard<std::mutex>(turnstile);
	auto lock = std::unique_lock<std::mutex>(mutex);

	pending = std::move(payload);
	pendingMask = &mask;
	pendingCasts = castCount;
	hasChosen = false;

	auto answered = cv.wait_for(lock, std::chrono::seconds(timeoutSeconds), [this] { return hasChosen; });

	pending.clear();
	pendingMask = nullptr;
	pendingCasts = 0;

	if(!answered)
		throw std::runtime_error("SaiBridge: no action posted within " + std::to_string(timeoutSeconds) + "s");

	return chosen;
}

int SaiBridge::decideOption(std::string payload, int optionCount)
{
	auto queued = std::lock_guard<std::mutex>(turnstile);
	auto lock = std::unique_lock<std::mutex>(mutex);

	pending = std::move(payload);
	pendingOptions = optionCount;
	hasChosen = false;

	auto answered = cv.wait_for(lock, std::chrono::seconds(timeoutSeconds), [this] { return hasChosen; });

	pending.clear();
	pendingOptions = 0;

	if(!answered)
		throw std::runtime_error("SaiBridge: no option posted within " + std::to_string(timeoutSeconds) + "s");

	return chosen.value;
}

std::string SaiBridge::dispatch(const std::string & method, const std::string & path, const std::string & body)
{
	if(path == "/health")
		return Respond(200, R"({"ok":true})");

	if(path == "/state")
	{
		auto lock = std::lock_guard<std::mutex>(mutex);
		if(pending.empty())
			return Respond(200, R"({"pending":false})");
		return Respond(200, pending);
	}

	if(path == "/option" && method == "POST")
	{
		int index = 0;
		try
		{
			index = std::stoi(body);
		}
		catch(const std::exception &)
		{
			return Respond(400, R"({"ok":false,"reason":"body is not an integer"})");
		}

		auto lock = std::lock_guard<std::mutex>(mutex);
		if(pending.empty())
			return Respond(400, R"({"ok":false,"reason":"no decision pending"})");

		if(pendingOptions == 0)
			return Respond(400, R"({"ok":false,"reason":"this decision expects /action or /cast"})");

		if(index < 0 || index >= pendingOptions)
			return Respond(400, R"({"ok":false,"reason":"option index out of range"})");

		chosen = SaiDecision{false, index};
		hasChosen = true;
		cv.notify_all();
		return Respond(200, R"({"ok":true})");
	}

	if(path == "/cast" && method == "POST")
	{
		int index = 0;
		try
		{
			index = std::stoi(body);
		}
		catch(const std::exception &)
		{
			return Respond(400, R"({"ok":false,"reason":"body is not an integer"})");
		}

		auto lock = std::lock_guard<std::mutex>(mutex);
		if(pending.empty())
			return Respond(400, R"({"ok":false,"reason":"no decision pending"})");

		if(pendingOptions > 0)
			return Respond(400, R"({"ok":false,"reason":"this decision expects /option"})");

		if(pendingCasts == 0)
			return Respond(400, R"({"ok":false,"reason":"no casts offered for this decision"})");

		if(index < 0 || index >= pendingCasts)
			return Respond(400, R"({"ok":false,"reason":"cast index out of range"})");

		chosen = SaiDecision{true, index};
		hasChosen = true;
		cv.notify_all();
		return Respond(200, R"({"ok":true})");
	}

	if(path == "/action" && method == "POST")
	{
		int action = 0;
		try
		{
			action = std::stoi(body);
		}
		catch(const std::exception &)
		{
			return Respond(400, R"({"ok":false,"reason":"body is not an integer"})");
		}

		auto lock = std::lock_guard<std::mutex>(mutex);
		if(pending.empty())
			return Respond(400, R"({"ok":false,"reason":"no decision pending"})");

		if(pendingOptions > 0)
			return Respond(400, R"({"ok":false,"reason":"this decision expects /option"})");

		// Refusing here beats letting the engine reject it: the caller still holds
		// the turn and can post a different action.
		if(action < 0 || pendingMask == nullptr || action >= static_cast<int>(pendingMask->size()) || !pendingMask->at(action))
			return Respond(400, R"({"ok":false,"reason":"action is not in the legal mask"})");

		chosen = SaiDecision{false, action};
		hasChosen = true;
		cv.notify_all();
		return Respond(200, R"({"ok":true})");
	}

	return Respond(404, R"({"ok":false,"reason":"unknown path"})");
}

void SaiBridge::serve()
{
	using boost::asio::ip::tcp;

	auto colon = bind.rfind(':');
	auto host = bind.substr(0, colon);
	auto port = static_cast<unsigned short>(std::stoi(bind.substr(colon + 1)));

	boost::asio::io_context ctx;
	boost::system::error_code ec;
	tcp::acceptor acceptor(ctx);
	auto endpoint = tcp::endpoint(boost::asio::ip::make_address(host, ec), port);

	if(ec || (acceptor.open(endpoint.protocol(), ec), ec) || (acceptor.bind(endpoint, ec), ec)
		|| (acceptor.listen(tcp::socket::max_listen_connections, ec), ec))
	{
		logAi->error("SaiBridge: cannot listen on %s: %s", bind, ec.message());
		return;
	}

	logAi->info("SaiBridge: listening on %s", bind);

	while(true)
	{
		tcp::socket socket(ctx);
		acceptor.accept(socket, ec);
		if(ec)
			continue;

		auto request = std::string();
		auto buffer = std::array<char, 4096>();

		// Read until the headers end, then however much Content-Length asks for.
		while(request.find("\r\n\r\n") == std::string::npos)
		{
			auto n = socket.read_some(boost::asio::buffer(buffer), ec);
			if(ec || n == 0)
				break;
			request.append(buffer.data(), n);
		}

		if(ec && request.find("\r\n\r\n") == std::string::npos)
			continue;

		auto headerEnd = request.find("\r\n\r\n") + 4;
		auto body = request.substr(headerEnd);
		auto lengthAt = request.find("Content-Length:");
		if(lengthAt != std::string::npos && lengthAt < headerEnd)
		{
			auto declared = static_cast<size_t>(std::stoul(request.substr(lengthAt + 15)));
			while(body.size() < declared)
			{
				auto n = socket.read_some(boost::asio::buffer(buffer), ec);
				if(ec || n == 0)
					break;
				body.append(buffer.data(), n);
			}
		}

		auto firstSpace = request.find(' ');
		auto secondSpace = request.find(' ', firstSpace + 1);
		auto method = request.substr(0, firstSpace);
		auto path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);

		auto response = dispatch(method, path, body);
		boost::asio::write(socket, boost::asio::buffer(response), ec);
		socket.shutdown(tcp::socket::shutdown_both, ec);
	}
}
}
