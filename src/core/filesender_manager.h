#ifndef FILESENDER_MANAGER_H
#define FILESENDER_MANAGER_H
#include <atomic>
#include"broadcast/broadcast_manager.h"
#include"authentication/authenticator.h"


class FilesenderManager {
    public:
        std::atomic<bool> global_stop_flag{false};

        Account account;

        Authenticator authenticator;
        BroadcastManager broadcast_manager;

      explicit FilesenderManager(short port);
     //GUI
      explicit FilesenderManager(const Account& acc, short port);
             ~FilesenderManager();

         std::optional<Account>run_broadcast();
private:
          std::thread broadcast_thread;

};

#endif // FILESENDER_MANAGER_H
