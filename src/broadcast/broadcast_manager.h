#ifndef BROADCAST_MANAGER_H
#define BROADCAST_MANAGER_H
#pragma once

#include <atomic>
#include <vector>
#include"authentication/profile.h"
#include <boost/asio.hpp>
namespace net = boost::asio;

class BroadcastManager {
    public:
        Account account;
        std::atomic<bool> broad_stop_flag{false};//使用原子变量控制线程停止
         std::atomic<bool> receive_stop_flag{false};

        void broadcast_sender(short port);
        void broadcast_receiver(short port);

        mutable std::mutex received_mutex;

        std::vector<Account> received_accounts;

     std::optional<Account> find_account_by_name(const std::string& name)const;

     std::string get_local_ipv4_address();

        std::vector<Account> receiver_list(short port);

};

#endif // BROADCAST_MANAGER_H
