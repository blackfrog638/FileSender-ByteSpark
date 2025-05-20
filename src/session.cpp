#include "authentication/authenticator.h"
#include "session.h"
#include <QString>
#include  <QDebug>
#include <iostream>
#include <boost/asio/write.hpp>
#include <filesystem>
#include <QCryptographicHash>

extern std::string g_current_password;
using boost::asio::ip::tcp;
namespace fs = std::filesystem;



Session::Session(tcp::socket socket, QObject* parent)
    : QObject(parent),
      socket_(std::move(socket))
{}



Session::~Session()
{

    qDebug() << " Session 已被析构";
}

void Session::closeSession() {
    if (socket_.is_open()) {
        boost::system::error_code ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
    if (outputFile_.is_open()) {
        outputFile_.close();
    }
    emit sessionEnded();  // 通知 Server 可以删除该 Session
}


void Session::start()
{    qDebug() << "sessio创建成功" ;
    QString clientInfo = QString::fromStdString(socket_.remote_endpoint().address().to_string() + ":" + std::to_string(socket_.remote_endpoint().port()));
    emit sessionStarted(clientInfo); // 发出信号 设计ui更新，暂时未写
   doWrite();

}


//简单版
void Session::doWrite()
{
    auto self(shared_from_this());
    std::string message = "正在连接服务端，进行密码校验\n";  // 确保消息结尾有换行符

    boost::asio::async_write(socket_, boost::asio::buffer(message),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                //qDebug() << "服务端将信息写入缓存区" ;
                doRead();

            }
            else {
                   qDebug() << "发送消息失败：" << QString::fromStdString(ec.message());
                       }
        });
}



//5.11新增状态机+ 读取客户端传来的文件数据
void Session::doRead()
{
    if (state_ == SessionState::ReceivingFile) {
        // 此状态不应使用行读取逻辑！
        return;
    }

    auto buf = std::make_shared<boost::asio::streambuf>();

    boost::asio::async_read_until(socket_, *buf, '\n',
        [this, buf](boost::system::error_code ec, std::size_t /*bytes_transferred*/) {
            if (!ec) {
                std::istream is(buf.get());
                std::string line;
                std::getline(is, line);

                // 状态驱动

                switch (state_) {
                    case SessionState::WaitingForPassword:
                        handlePassword(line);
                        doRead();
                        break;
                    case SessionState::Authenticated:
                        qDebug() << "接收到客户端输入的文件路径：" << QString::fromStdString(line);
                            handleSendCommand(line); // 正常 SEND
                        // 这里不能调用doread
                        break;
                    default:
                        qDebug() << "当前状态不支持接收输入";
                        break;
                }
            } else {
                qDebug() << "读取失败：" << QString::fromStdString(ec.message());
                closeSession();
            }
        });
}

//5.10 新增处理客户端发送的密码
void Session::handlePassword(const std::string& line)
{
    if (line == g_current_password) {
        qDebug() << "密码正确，进入 Authenticated 状态";
        state_ = SessionState::Authenticated;

        auto self = shared_from_this();
        std::string response = "密码正确，连接成功，请输入要发送文件的路径\n";
        boost::asio::async_write(socket_, boost::asio::buffer(response),
            [this, self,response](boost::system::error_code ec, std::size_t /*length*/) {
            if (ec) {
                qDebug() << "[错误] async_write 失败：" << QString::fromStdString(ec.message());
            } else {
                qDebug() << "[发送成功] 密码验证成功消息已发出";
            }
            });
    }
    else {
        auto self = shared_from_this();
        std::string response = "AUTH_FAIL\n";
        boost::asio::async_write(socket_, boost::asio::buffer(response),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (ec) {
                    qDebug() << "发送确认信息失败2：" << QString::fromStdString(ec.message());
                    closeSession();
                }
            });
        qDebug() << "密码错误，关闭连接，请后续尝试重新输入";
        closeSession();
    }
}
//5.11处理客户端发送的路径命令
//5.13新增交互

//void Session::handleSendCommand(const std::string& line)
//{
//    std::istringstream iss(line);
//    std::string command;
//    iss >> command;

//    if (command == "SEND") {
//        iss >> fileName_ >> fileSize_;

//        if (fileName_.empty() || fileSize_ == 0) {
//            qDebug() << "SEND 命令参数错误";
//            closeSession();
//            return;
//        }

//        qDebug() << "客户端请求发送文件：" << QString::fromStdString(fileName_)
//                 << " 大小：" << fileSize_;
//             //5.13重命名同名的文件
//        fileName_ = generateUniqueFileName(fileName_);

//        outputFile_.open(fileName_, std::ios::binary);
//        if (!outputFile_) {
//            qDebug() << "无法创建文件：" << QString::fromStdString(fileName_);
//            closeSession();
//            return;
//        }
//        qDebug() << "若接收文件，保存为：" << QString::fromStdString(fileName_) << " 大小：" << fileSize_;

//        qDebug() << "是否接受该文件？[yes/no]";
//       // 调试专用
////        auto self = shared_from_this();
////        std::string response = "OK\n";
////        boost::asio::async_write(socket_, boost::asio::buffer(response),
////            [this, self](boost::system::error_code ec, std::size_t) {
////                if (!ec) {
////                    state_ = SessionState::ReceivingFile;
////                    readBuffer_ = std::make_shared<boost::asio::streambuf>();
////                    startReceivingFile();
////                } else {
////                    qDebug() << "1111111"<< QString::fromStdString(ec.message());
////                    closeSession();
////                }
////            });

//        // 启动线程读取手动输入，不阻塞异步主线程
//        std::thread([self = shared_from_this()]() {
//            std::string input;
//            std::getline(std::cin, input);

//            if (input == "yes") {

//                std::string response = "YES\n";
//                boost::asio::async_write(self->socket_, boost::asio::buffer(response),
//                    [self](boost::system::error_code ec, std::size_t) {
//                        if (!ec) {
//                            self->state_ = SessionState::ReceivingFile;
//                            self->readBuffer_ = std::make_shared<boost::asio::streambuf>();
//                            self->startReceivingFile();
//                        } else {
//                            qDebug() << "发送 YES 失败：" << QString::fromStdString(ec.message());
//                            self->closeSession();
//                        }
//                    });
//            } else {
//                std::string response = "NO\n";
//                boost::asio::async_write(self->socket_, boost::asio::buffer(response),
//                    [self](boost::system::error_code, std::size_t) {
//                        qDebug() << "拒绝接收，关闭连接";
//                        self->closeSession();
//                    });
//            }
//        }).detach(); // 线程分离
//    }
//}


void Session::handleSendCommand(const std::string& line)
{
    std::istringstream iss(line);
    std::string command;
    iss >> command;

    if (command == "SEND") {
        iss >> fileName_ >> fileSize_;

        if (fileName_.empty() || fileSize_ == 0) {
            qDebug() << "SEND 命令参数错误";
            closeSession();
            return;
        }

        fileName_ = generateUniqueFileName(fileName_);
        outputFile_.open(fileName_, std::ios::binary);
        if (!outputFile_) {
            qDebug() << "无法创建文件：" << QString::fromStdString(fileName_);
            closeSession();
            return;
        }

        emit fileTransferRequest(this,
                                 QString::fromStdString(fileName_),
                                 fileSize_);
    }
}

void Session::acceptFileTransfer()
{
    auto self = shared_from_this();
    std::string response = "YES\n";
    boost::asio::async_write(socket_, boost::asio::buffer(response),
        [this, self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                // 继续读取文件数据
                state_ = SessionState::ReceivingFile;
                readBuffer_ = std::make_shared<boost::asio::streambuf>();
                startReceivingFile();
            } else {
                qDebug() << "发送 YES 失败：" << QString::fromStdString(ec.message());
                closeSession();
            }
        });
}

void Session::rejectFileTransfer()
{
    auto self = shared_from_this();
    std::string response = "NO\n";
    boost::asio::async_write(socket_, boost::asio::buffer(response),
        [this, self](boost::system::error_code, std::size_t) {
            qDebug() << "已拒绝接收文件，关闭连接";
            closeSession();
        });
}


//5.11 实现文件传输
void Session::startReceivingFile()
{
    auto self = shared_from_this();
    // 剩余数据大小
    std::size_t remaining = fileSize_ - bytesReceived_;
    std::size_t chunkSize = std::min<std::size_t>(4096, remaining);
    std::size_t blockSize = 32 + chunkSize;  // hash + data

     auto buffer = std::make_shared<std::vector<char>>(blockSize);

    //5.14更新 修复跨机传输hash码错误原因
    boost::asio::async_read(socket_, boost::asio::buffer(*buffer, blockSize),
         [this, self, buffer,chunkSize](boost::system::error_code ec, std::size_t bytes_transferred){
            if (!ec) {
                if (bytes_transferred != 32 + chunkSize) {
                    qDebug() << "接收长度错误，预期" << (32 + chunkSize) << "，实际" << bytes_transferred;
                    closeSession();
                    return;
                }

                //qDebug() << "接收文件的回调：" ;
                QByteArray receivedHash(buffer->data(), 32);
                qDebug() << "收到 chunk hash：" << receivedHash.toHex().left(8);
                QByteArray chunkData(buffer->data() + 32, chunkSize);
                QByteArray computedHash = QCryptographicHash::hash(chunkData, QCryptographicHash::Sha256);
                qDebug() << "计算 chunk hash：" << computedHash.toHex().left(8);

                if (receivedHash != computedHash) {
                    qDebug() << "校验失败，块数据不一致，传输中断";
                    closeSession();
                    return;
                }
                // 写入数据
                outputFile_.write(chunkData.data(), chunkData.size());
                bytesReceived_ += chunkData.size();
               //进度显示
                int percent = static_cast<int>((bytesReceived_ * 100.0) / fileSize_);
                static int lastShown = -1;
                if (percent != lastShown) {
                    qDebug() << "已接收:" << percent << "%";
                    lastShown = percent;
                    emit receiveProgress(percent);  // ✅ 发出信号
                }
                if (percent != lastProgress_) {
                    emit receiveProgress(percent);
                    lastProgress_ = percent;
                }


                if (bytesReceived_ < fileSize_) {
                    startReceivingFile(); // 继续接收
                } else {
                    qDebug() << "文件接收完成,所有hash块校验通过：" << QString::fromStdString(fileName_);
                    outputFile_.close();
                    state_ = SessionState::Authenticated;  // 可继续其他命令
                }
            } else {
                qDebug() << "接收文件失败：" << QString::fromStdString(ec.message());
                outputFile_.close();
                closeSession();
            }
        });
}



//5.13新增重命名函数
std::string Session::generateUniqueFileName(const std::string& originalName) {
    fs::path path(originalName);
    std::string stem = path.stem().string();     // 不含扩展名
    std::string ext = path.extension().string();
    std::string parent = path.parent_path().string();

    fs::path tryPath = path;
    int index = 1;
    while (fs::exists(tryPath)) {
        std::ostringstream oss;
        oss << stem << "(" << index << ")" << ext;
        tryPath = fs::path(parent) / oss.str();
        index++;
    }
    return tryPath.string();
}



//5.15断点重传

void Session::handleResumeCommand(const std::string& line)
{
    std::istringstream iss(line);
    std::string cmd, filename;
    std::size_t offset, totalSize;

    iss >> cmd >> filename >> offset >> totalSize;

    if (cmd != "RESUME" || filename.empty() || totalSize == 0) {
        qDebug() << "RESUME 命令格式不正确";
        closeSession();
        return;
    }

    fileName_ = filename;
    fileSize_ = totalSize;       // 接收客户端传来的文件总大小
    bytesReceived_ = offset;

    qDebug() << "收到 RESUME 请求：文件名 =" << QString::fromStdString(fileName_)
             << "，偏移 =" << offset << "，总大小 =" << totalSize;

    // 检查文件是否存在
    std::ifstream in(fileName_, std::ios::binary);
    if (!in.good()) {
        qDebug() << "文件不存在，无法续传：" << QString::fromStdString(fileName_);
        std::string response = "RESUME_REJECT\n";
        boost::asio::async_write(socket_, boost::asio::buffer(response),
            [this, self = shared_from_this()](boost::system::error_code, std::size_t) {
                closeSession();
            });
        return;
    }

    // 打开文件用于续写（写入模式 + 不截断）
    outputFile_.open(fileName_, std::ios::binary | std::ios::in | std::ios::out);
    if (!outputFile_) {
        qDebug() << "无法打开文件进行续写：" << QString::fromStdString(fileName_);
        closeSession();
        return;
    }

    // 跳转偏移
    outputFile_.seekp(offset);
    if (!outputFile_) {
        qDebug() << " seekp 失败，偏移非法：" << offset;
        closeSession();
        return;
    }
    emit resumeTransferRequest(this,
                               QString::fromStdString(fileName_),
                               bytesReceived_,
                               fileSize_);
    // 发送 OK，准备进入文件接收逻辑
//    std::string response = "RESUME_OK\n";
//    boost::asio::async_write(socket_, boost::asio::buffer(response),
//        [this, self = shared_from_this()](boost::system::error_code ec, std::size_t) {
//            if (!ec) {
//                qDebug() << "准备续传：" << QString::fromStdString(fileName_)
//                         << " 从 offset=" << bytesReceived_;
//                state_ = SessionState::ReceivingFile;
//                startReceivingFile();
//            } else {
//                qDebug() << "回复 RESUME_OK 失败：" << QString::fromStdString(ec.message());
//                closeSession();
//            }
//        });
}
//配合GUI的槽函数
void Session::acceptResume()
{
    std::string response = "RESUME_OK\n";
    auto self = shared_from_this();

    boost::asio::async_write(socket_, boost::asio::buffer(response),
        [this, self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                qDebug() << "准备续传：" << QString::fromStdString(fileName_)
                         << " 从 offset=" << bytesReceived_;
                state_ = SessionState::ReceivingFile;
                startReceivingFile();
            } else {
                qDebug() << "发送 RESUME_OK 失败：" << QString::fromStdString(ec.message());
                closeSession();
            }
        });
}

void Session::rejectResume()
{
    std::string response = "RESUME_REJECT\n";
    auto self = shared_from_this();

    boost::asio::async_write(socket_, boost::asio::buffer(response),
        [this, self](boost::system::error_code, std::size_t) {
            qDebug() << "用户拒绝续传，关闭连接";
            closeSession();
        });
}
