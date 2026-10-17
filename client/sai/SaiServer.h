/*
 * SaiServer.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/json/JsonNode.h"

namespace sai
{

struct HttpRequest
{
	std::string method;
	std::string path;
	std::map<std::string, std::string> query;
	std::string body;
};

struct HttpResponse
{
	int status = 200;
	JsonNode body;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest &)>;

class Server
{
	std::string bind;
	HttpHandler handler;
	std::thread worker;

	void serve();

public:
	Server(std::string bind, HttpHandler handler);
	~Server();

	const std::string & address() const { return bind; }
};

}
