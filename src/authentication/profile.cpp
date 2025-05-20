#include "profile.h"


Account::Account(std::string name, std::string password) :
    name(std::move(name)), password(std::move(password)){}

bool Account::operator==(const Account& other) const {
    return name == other.name && password == other.password && ip == other.ip && port == other.port;
}
bool Account::operator!=(const Account& other) const {
    return !(*this == other);
}

