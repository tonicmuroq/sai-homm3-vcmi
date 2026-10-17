/*
 * SaiServer.cpp, part of VCMI engine
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

#include "SaiServer.h"

#include "../../lib/CThreadHelper.h"

namespace sai
{

namespace
{

constexpr size_t MAX_BODY = 1 << 20;
constexpr auto READ_TIMEOUT = std::chrono::seconds(10);

// One connection is served at a time, so a client that connects and sends nothing would block every other one.
size_t ReadSome(boost::asio::ip::tcp::socket & socket, std::array<char, 8192> & buffer, boost::system::error_code & ec)
{
	auto & ctx = static_cast<boost::asio::io_context &>(socket.get_executor().context());
	size_t received = 0;
	auto done = false;
	socket.async_read_some(boost::asio::buffer(buffer), [&](const boost::system::error_code & error, size_t bytes) {
		ec = error;
		received = bytes;
		done = true;
	});
	ctx.restart();
	ctx.run_for(READ_TIMEOUT);
	if(!done)
	{
		socket.close();
		ctx.restart();
		ctx.run();
		ec = boost::asio::error::timed_out;
	}
	return received;
}

std::string UrlDecode(const std::string & text)
{
	auto out = std::string();
	for(size_t i = 0; i < text.size(); ++i)
	{
		if(text[i] == '+')
			out += ' ';
		else if(text[i] == '%' && i + 2 < text.size())
		{
			out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
			i += 2;
		}
		else
			out += text[i];
	}
	return out;
}

std::map<std::string, std::string> ParseQuery(const std::string & query)
{
	auto out = std::map<std::string, std::string>();
	auto parts = std::vector<std::string>();
	boost::split(parts, query, boost::is_any_of("&"));
	for(const auto & part : parts)
	{
		if(part.empty())
			continue;
		auto eq = part.find('=');
		if(eq == std::string::npos)
			out[UrlDecode(part)] = "";
		else
			out[UrlDecode(part.substr(0, eq))] = UrlDecode(part.substr(eq + 1));
	}
	return out;
}

std::string Reason(int status)
{
	switch(status)
	{
	case 200: return "OK";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 409: return "Conflict";
	default: return "Internal Server Error";
	}
}

void Respond(boost::asio::ip::tcp::socket & socket, const HttpHandler & handler)
{
	boost::system::error_code ec;
	auto raw = std::string();
	auto buffer = std::array<char, 8192>();
	size_t headerEnd = std::string::npos;

	while(headerEnd == std::string::npos && raw.size() < MAX_BODY)
	{
		auto n = ReadSome(socket, buffer, ec);
		if(ec)
			break;
		raw.append(buffer.data(), n);
		headerEnd = raw.find("\r\n\r\n");
	}
	if(headerEnd == std::string::npos)
		return;

	auto head = raw.substr(0, headerEnd);
	auto lines = std::vector<std::string>();
	boost::split(lines, head, boost::is_any_of("\n"));
	for(auto & line : lines)
		boost::trim(line);

	auto request = HttpRequest();
	auto startLine = std::vector<std::string>();
	boost::split(startLine, lines.at(0), boost::is_any_of(" "));
	if(startLine.size() < 2)
		return;
	request.method = startLine[0];
	auto target = startLine[1];
	auto question = target.find('?');
	request.path = target.substr(0, question);
	if(question != std::string::npos)
		request.query = ParseQuery(target.substr(question + 1));

	size_t contentLength = 0;
	for(size_t i = 1; i < lines.size(); ++i)
	{
		auto colonPos = lines[i].find(':');
		if(colonPos == std::string::npos)
			continue;
		auto name = boost::algorithm::to_lower_copy(lines[i].substr(0, colonPos));
		if(name == "content-length")
			contentLength = std::min<size_t>(std::stoul(boost::trim_copy(lines[i].substr(colonPos + 1))), MAX_BODY);
	}

	request.body = raw.substr(headerEnd + 4);
	while(request.body.size() < contentLength)
	{
		auto n = ReadSome(socket, buffer, ec);
		if(ec)
			break;
		request.body.append(buffer.data(), n);
	}

	auto response = HttpResponse();
	try
	{
		response = handler(request);
	}
	catch(const std::exception & e)
	{
		logGlobal->error("sai_request_failed, path=%s, error=%s", request.path, e.what());
		response.status = 500;
		response.body["error"].String() = e.what();
	}

	auto body = response.body.toCompactString();
	auto reply = "HTTP/1.1 " + std::to_string(response.status) + " " + Reason(response.status) + "\r\n"
		+ "Content-Type: application/json; charset=utf-8\r\n"
		+ "Content-Length: " + std::to_string(body.size()) + "\r\n"
		+ "Connection: close\r\n\r\n" + body;
	boost::asio::write(socket, boost::asio::buffer(reply), ec);
}

}

Server::Server(std::string bind, HttpHandler handler)
	: bind(std::move(bind))
	, handler(std::move(handler))
{
	worker = std::thread([this] { serve(); });
}

Server::~Server()
{
	// The listener blocks in accept() and the process is going down anyway; joining would hang.
	worker.detach();
}

void Server::serve()
{
	setThreadName("SaiServer");
	using boost::asio::ip::tcp;

	auto colon = bind.rfind(':');
	auto host = bind.substr(0, colon);
	auto port = static_cast<unsigned short>(std::stoi(bind.substr(colon + 1)));

	boost::asio::io_context ctx;
	boost::system::error_code ec;
	auto endpoint = tcp::endpoint(boost::asio::ip::make_address(host, ec), port);
	if(ec)
	{
		logGlobal->error("sai_server_bad_address, bind=%s, error=%s", bind, ec.message());
		return;
	}

	auto acceptor = tcp::acceptor(ctx);
	acceptor.open(endpoint.protocol(), ec);
	acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
	acceptor.bind(endpoint, ec);
	if(!ec)
		acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
	if(ec)
	{
		logGlobal->error("sai_server_listen_failed, bind=%s, error=%s", bind, ec.message());
		return;
	}

	logGlobal->info("sai_server_listening, bind=%s", bind);

	while(true)
	{
		auto socket = tcp::socket(ctx);
		acceptor.accept(socket, ec);
		if(ec)
			continue;

		try
		{
			Respond(socket, handler);
		}
		catch(const std::exception & e)
		{
			logGlobal->error("sai_connection_failed, error=%s", e.what());
		}
	}
}

}
