#include "authenticator.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <QMessageBox>
#include <QString>
std::string g_current_password;

Account Authenticator::get_profile(){
    std::ifstream ifs(PROFILE_PATH);
    boost::archive::text_iarchive ia(ifs);

    Account account;
    ia >> account;
    return account;
}

void Authenticator::regist(){
    std::cout<<"Please input your name: ";
    std::string name;
    std::cin>>name;
std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cout<<"Please input your password: ";
    std::string password;
    std::cin>>password;
std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    Account account(name, password);

    std::ofstream ofs(Authenticator::PROFILE_PATH);
    boost::archive::text_oarchive oa(ofs);

    oa << account;
}
//gui
bool Authenticator::login() {
    std::ifstream infile(Authenticator::PROFILE_PATH);  // 尝试打开文件

    if (infile.good()) {
        Account account = get_profile();
        std::cout << account.name << "的密码: ";

        std::string password;
        std::cin >> password;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        if (password != account.password) {
            std::cout << "Password is incorrect!" << std::endl;
            return false;
        }

        g_current_password = password;
        return true;
    }
    else {
        std::cout << "No profile found, please register first!" << std::endl;
        regist();
        return false;
    }
}



bool Authenticator::login(const std::string& password) {
    std::ifstream infile(Authenticator::PROFILE_PATH);  // 尝试打开文件

    if (infile.good()) {
        Account account = get_profile();

        if (password != account.password) {
            std::cerr << "Password is incorrect!" << std::endl;
            return false;
        }

        g_current_password = password;
        return true;
    } else {
        std::cerr << "No profile found, please register first!" << std::endl;
        return false;
    }
}
//GUI
void Authenticator::regist(const std::string& name, const std::string& password)
{
    Account account(name, password);

    std::ofstream ofs(Authenticator::PROFILE_PATH);
    if (!ofs) {
        std::cerr << "无法打开文件进行写入！" << std::endl;
        return;
    }

    boost::archive::text_oarchive oa(ofs);
    oa << account;
}




void Authenticator::main_handler(){
    while(!login()){
        login();
    }
}
