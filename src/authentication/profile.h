#ifndef PROFILE_H
#define PROFILE_H


#pragma once

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <string>

class Account {
    public:
        std::string name;
        std::string password;
        std::string ip;
        short port = 0;

        Account() = default;
        Account(std::string name, std::string password);

        bool operator==(const Account& other) const;
        bool operator!=(const Account& other) const;

        template<class Archive>
        void serialize(Archive& ar, unsigned int version) {
            ar & name;
            ar & password;
            ar & ip;
            //!TODO: 密码加密
            ar & port;
        }

    private:


};

#endif // PROFILE_H
