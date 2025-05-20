#include "filesender_manager.h"
#include <iostream>
#include <thread>


FilesenderManager::FilesenderManager(short port){
    authenticator.main_handler();
    account = authenticator.get_profile();
    account.port = port;
    broadcast_manager.account = account;

}
//GUI
FilesenderManager::FilesenderManager(const Account& acc, short port) {
    account = acc;
    account.port = port;
    broadcast_manager.account = account;

    broadcast_thread = std::thread([this]() {
         broadcast_manager.broadcast_sender(account.port);
     });
     broadcast_thread.detach();  // 后台常驻


}
FilesenderManager::~FilesenderManager()
{
    broadcast_manager.broad_stop_flag = true;

    if (broadcast_thread.joinable()) {
        broadcast_thread.join();  // 等待广播线程退出
    }

    std::cout << "[广播] 广播线程已安全退出。" << std::endl;
}


std::optional<Account> FilesenderManager::run_broadcast(){
    std::thread sender_thread([&]() { broadcast_manager.broadcast_sender(account.port); });
    std::thread receiver_thread([&]() { broadcast_manager.broadcast_receiver(account.port); });

    std::cout<<"正在获取在线设备端号..."<<std::endl;
     std::this_thread::sleep_for(std::chrono::seconds(3));
    std::string target_user;
    std::cin>>target_user;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cout<<"连接设备: "<<target_user<<std::endl;
    broadcast_manager.receive_stop_flag = true;
    //broadcast_manager.broad_stop_flag = true;
    sender_thread.detach();
    receiver_thread.join();

    auto target_account_opt = broadcast_manager.find_account_by_name(target_user);
       if (target_account_opt) {
           const auto& target_account = target_account_opt.value();
           std::cout << "找到用户：" << target_account.name << std::endl;
           std::cout << "IP: " << target_account.ip << " 端口: " << target_account.port << std::endl;
           return target_account;
       } else {

           std::cout << "未找到该用户名！" << std::endl;
               return std::nullopt;
       }



}
