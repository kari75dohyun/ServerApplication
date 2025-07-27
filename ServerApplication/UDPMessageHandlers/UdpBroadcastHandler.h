#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

class Session;
class DataHandler;

void Udpbroadcast_handler(std::shared_ptr<Session> session, const nlohmann::json& msg,
	const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket, DataHandler* handler);

