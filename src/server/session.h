#ifndef SESSION_H
#define SESSION_H


#pragma once

#include <QObject>
#include <boost/asio.hpp>
#include <fstream>

class Session : public QObject, public std::enable_shared_from_this<Session>
{
    Q_OBJECT
public:

    explicit Session(boost::asio::ip::tcp::socket socket, QObject* parent = nullptr);
     ~Session();
    void start(); // 开始会话
    void acceptFileTransfer();   // 添加的
    void rejectFileTransfer();   // 添加的
    void acceptResume();
    void rejectResume();
    int lastProgress_ = -1;
   void closeSession();

    //5.10新增状态机管理
    enum class SessionState {
        WaitingForPassword,
           Authenticated,
           WaitingForSendCommand,
           ReceivingFile,
         ResumingFile,//重传
           Closed
    };
signals:
    void sessionStarted(const QString& clientInfo); // 通知有新连接
     void sessionEnded(); //会话结束


signals:
    void fileTransferRequest(Session* session,
                             const QString& fileName,
                             std::size_t fileSize);
    void resumeTransferRequest(Session* session,
                               const QString& fileName,
                               std::size_t offset,
                               std::size_t totalSize);

   void receiveProgress(int percent);

private:
    void doWrite(); //可以做提示发送给客户端输入密码
    void doRead();  // 读取客户端信息


    //5.10新增
    void handlePassword(const std::string& line);
    //5.11新增 处理发送的文件路径
    void handleSendCommand(const std::string& line);
   //5.11  处理文件传输
    void startReceivingFile();
    //5.13 新增自动重命名函数
    std::string generateUniqueFileName(const std::string& originalName);
  //5.15断点重传
    void handleResumeCommand(const std::string& line);

    boost::asio::ip::tcp::socket socket_;
    boost::asio::streambuf read_buffer_; // 用于接收数据
    //5.10新增
     SessionState state_ = SessionState::WaitingForPassword;



     // 5.11新增文件接收相关
     std::ofstream outputFile_;
     std::size_t fileSize_ = 0;
     std::size_t bytesReceived_ = 0;//确认文件大小
     std::string fileName_;
     std::shared_ptr<boost::asio::streambuf> readBuffer_;



};

#endif // SESSION_H
