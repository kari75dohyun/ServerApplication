#pragma once
#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include "SSLSession.h"

class SessionPool {
public:
    SessionPool(size_t pool_size, boost::asio::io_context& io, boost::asio::ssl::context& context, std::weak_ptr<DataHandler> handler);

    // ��� ������ ���� ȹ�� (������ nullptr ��ȯ)
    std::shared_ptr<SSLSession> acquire(boost::asio::ip::tcp::socket&& socket, int session_id);

    // ���� ��ȯ(����)
    void release(std::shared_ptr<SSLSession> session);

private:
    std::vector<std::shared_ptr<SSLSession>> pool_;
    std::queue<size_t> available_indices_;
    std::mutex mutex_;
    boost::asio::ssl::context& context_;
    std::weak_ptr<DataHandler> handler_;

    boost::asio::io_context& io_; // �߰�
};

