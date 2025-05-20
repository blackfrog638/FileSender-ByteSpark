#ifndef CLIENT_H
#define CLIENT_H


// client.h

#pragma once

#include <QObject>
#include <QString>
#include <boost/asio.hpp>
#include <fstream>
class Client : public QObject
{
    Q_OBJECT

public:
    explicit Client(QObject* parent = nullptr);
    ~Client();

    //void connectToServer(const QString& ip, unsigned short port);
    void connectToServer(const QString& ip, unsigned short port, const QString& password);
    void sendMessage(const QString& message);
   void setMessageToSend(const QString& msg);
      void checkResumeAndStart(const QString& path);
   void sendFile(const QString& path);
   void cancelSend();
void cancelSend_resume();
signals:
    void connectedToServer();                 // 连接成功

    void connectionFailed(const QString&);   // 连接失败
    void receivedMessage(const QString&);    // 收到服务端消息
    //密码校验
    void authPassed();
    void authFailed();
  void sendProgress(int percent);//发送进度条
    void sendRejected();  // 服务端拒绝接收
   void resumeRejected();//服务端拒绝续传
    void sendFinished();//发送结束
private:
    void doConnect(const std::string& ip, unsigned short port);
    void doRead();
  boost::asio::streambuf readBuf_;
    //5.11新增 客户端文件发送相关
    QString filePath_;
    std::ifstream fileStream_;
    std::size_t fileSize_ = 0;
    std::size_t fileSize_1= 0;
    std::size_t bytesSent_ = 0;

    //5.11新增文件发送相关

   //5.12新增 分块发送
   void sendFileDataChunk();

  //5.15断点续传
   QString resumeMetaFile_;      // .resume 文件路径
   qint64 currentOffset_ = 0;    // 已发送字节数
   qint64 currentOffset_1 = 0;
//5.18
   void sendCommandThen(const QString& msg, std::function<void()> onSent);


    QString messageToSend_;
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::ip::tcp::socket> socket_;
    std::unique_ptr<boost::asio::ip::tcp::resolver> resolver_;
    std::thread networkThread_;

};


#endif // CLIENT_H
