#include "broadcast_manager.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <QDebug>


std::string BroadcastManager::get_local_ipv4_address() {
    try {
        boost::asio::io_context io_context;

        // 创建UDP socket，连接一个“外部”地址（不会真的发包）
        boost::asio::ip::udp::endpoint target_endpoint(
            boost::asio::ip::address::from_string("8.8.8.8"), 53);
        boost::asio::ip::udp::socket socket(io_context);
        socket.connect(target_endpoint);

        // 获取本地出口IP
        auto local_ip = socket.local_endpoint().address().to_string();
        std::cout << "本地IP地址: " << local_ip << std::endl;
        return local_ip;

    } catch (const std::exception& e) {
        std::cerr << "获取本地ip错误: " << e.what() << std::endl;
    }

    return "0.0.0.0";
}





void BroadcastManager::broadcast_sender(short port){
    try {
        net::io_context io;
        net::ip::udp::socket socket(io, net::ip::udp::v4());
        socket.set_option(net::socket_base::broadcast(true));

        const auto broadcast_ep = net::ip::udp::endpoint(
            net::ip::address_v4::broadcast(), port);

        std::string new_ip = get_local_ipv4_address();
       // account.ip = get_local_ipv4_address();  // 设置为本机真实IP
        //account.port = your_service_port;       // 设置你真正监听的端口
        account.ip = new_ip;
        std::cout << "[广播] 发送账号1信息: IP = " << account.ip
                  << ", 端口 = " << account.port << std::endl;

        while(!broad_stop_flag){

            std::ostringstream oss;
            boost::archive::text_oarchive oa(oss);
            oa << account;
            std::string serialized_account = oss.str();

            socket.send_to(net::buffer(serialized_account), broadcast_ep);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch(std::exception &e){
        std::cerr<<"广播失败: "<<e.what()<<std::endl;
    }
}

void BroadcastManager::broadcast_receiver(short port){
   //while(!receive_stop_flag){
        auto received_messages = receiver_list(port);
        if(!received_messages.empty()){
            std::cout << "在线设备: " << std::endl;
            for(const auto& message : received_messages){
                std::cout << "name:" << message.name << " ip:" << message.ip
                          << " port:" << message.port << std::endl;
            }
        }
  // }
}

std::vector<Account> BroadcastManager::receiver_list(short port){
    std::vector<Account> received_messages;
    try {
        net::io_context io;
        net::ip::udp::socket socket(io,
            net::ip::udp::endpoint(net::ip::udp::v4(), port));

        const size_t buffer_size = 1024;
        std::string serialized_str;
        std::vector<char> buffer(buffer_size);
        net::ip::udp::endpoint remote_ep;
        //获取当时时钟，限时3秒刷新一次
        auto clock_start = std::chrono::high_resolution_clock::now();

        while(!receive_stop_flag){
            size_t len = socket.receive_from(net::buffer(buffer), remote_ep);
            std::string serialized_str(buffer.begin(), buffer.begin() + (int)len);

            std::istringstream iss(serialized_str);
            boost::archive::text_iarchive ia(iss);
            Account received_data;
            ia >> received_data;

            received_data.ip = remote_ep.address().to_string();
            if(!std::count(received_messages.begin(), received_messages.end(), received_data)){
                received_messages.push_back(received_data);
                {
                        std::lock_guard<std::mutex> lock(received_mutex);
                        if (!std::count(received_accounts.begin(), received_accounts.end(), received_data)) {
                            received_accounts.push_back(received_data);
                        }
                    }

            }



            auto clock_end = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(clock_end - clock_start).count();
            if(elapsed > 3){
                break;
            }
        }
    }
    catch (const std::exception &e){
        std::cerr<<"Receiver error: " << e.what() << std::endl;
    }
    return received_messages;
}
std::optional<Account> BroadcastManager::find_account_by_name(const std::string& name)const  {
    std::lock_guard<std::mutex> lock(received_mutex);
    for (const auto& acc : received_accounts) {
        if (acc.name == name) {
            return acc;
        }
    }
    return std::nullopt;
}
