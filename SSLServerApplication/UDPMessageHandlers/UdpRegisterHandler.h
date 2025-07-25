#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

class SSLSession;

void Udpregister_handler(std::shared_ptr<SSLSession> session, const nlohmann::json& msg,
	const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket);
