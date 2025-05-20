#ifndef AUTHENTICATOR_H
#define AUTHENTICATOR_H


#include "../authentication/profile.h"

class Authenticator {

    public:
      static std::string current_password;
        Account get_profile();
        bool login();
        void regist();
        void main_handler();
        constexpr static const char* PROFILE_PATH = "profile.txt";

//GUI
        bool login(const std::string& inputPassword);
        void regist(const std::string& name, const std::string& password);

};
#endif // AUTHENTICATOR_H
