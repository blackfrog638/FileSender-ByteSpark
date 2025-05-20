#include <QCoreApplication>
#include <QApplication>
#include <QTextStream>
#include "mainwindow.h"
#include <QThread>
#include <QTimer>
#include <iostream>
#include <QDebug>
#include "core/filesender_manager.h"
#include "server.h"
#include "client.h"

const short PORT = 8888;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    qRegisterMetaType<std::size_t>("std::size_t");

    MainWindow w;
    w.show();

    return app.exec();  // 启动事件循环，GUI 应用必须有
}

//int main(int argc, char *argv[])
//{  QCoreApplication a(argc, argv);

//    FilesenderManager fm(PORT);
//    std::cout<<"Login successful!"<<std::endl;


//    Server server;
//    server.startServer(PORT);

//    Client client;


//  //各种信号
//   QObject::connect(&server, &Server::newSessionConnected, [](const QString& clientInfo){
//          std::cout << "新连接来自: " << clientInfo.toStdString() << std::endl;});


//   //启动广播

//        auto target_opt = fm.run_broadcast();

//     if (target_opt) {
//        const Account& target = target_opt.value();
//       // std::cout << "自动连接到: " << target.name << " - " << target.ip << ":" << target.port << std::endl;
//       client.connectToServer(QString::fromStdString(target.ip), target.port);
//       //client.connectToServer("58.199.161.11", 8888);

////            QObject::connect(&client, &Client::connectedToServer, []() {
////                std::cout << "已经创建传输通道！" << std::endl;
////            });

////            QObject::connect(&client, &Client::receivedMessage, [](const QString& msg) {
////                std::cout << "【服务器消息】" << msg.toStdString() << std::endl;
////            });

//            QObject::connect(&client, &Client::connectionFailed, [](const QString& error) {
//                std::cout << "【连接失败】" << error.toStdString() << std::endl;
//            });

//    }

//else {
//        std::cout << "未找到目标用户，无法连接。" << std::endl;
//    }





//   return a.exec();
//}
