// client.cpp
#include  <QDebug>
#include "client.h"
#include <iostream>
#include <boost/asio/connect.hpp>
#include <QFileInfo>
#include <QCryptographicHash>
using boost::asio::ip::tcp;

Client::Client(QObject* parent)
    : QObject(parent)
{


}

Client::~Client()
{
    io_context_.stop();
    if (networkThread_.joinable()) {
        networkThread_.join();
    }
    qDebug() << "客户端析构，连接已断开";
}

void Client::cancelSend()
{
    if (socket_ && socket_->is_open()) {
        boost::system::error_code ec;
        socket_->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_->close(ec);
    }

    if (fileStream_.is_open()) {
        fileStream_.close();
    }

    qDebug() << "[发送] 用户取消发送";
}

void Client::cancelSend_resume()
{
    if (socket_ && socket_->is_open()) {
        boost::system::error_code ec;
        socket_->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_->close(ec);
    }

    if (fileStream_.is_open()) {
        fileStream_.close();
    }
     //清理 resume 文件
    if (!resumeMetaFile_.isEmpty()) {
        QFile::remove(resumeMetaFile_);
    }
    qDebug() << "[发送] 用户取消发送";
}

void Client::sendMessage(const QString& message)
{
    if (!socket_ || !socket_->is_open())
        return;

    std::string msg = message.toStdString() + "\n";  //
    boost::asio::async_write(*socket_, boost::asio::buffer(msg),
        [this](boost::system::error_code ec, std::size_t /*length*/) {
            if (ec) {
                emit connectionFailed(QString::fromStdString(ec.message()));
            }

        });
}


//传入需要连接的ip地址和端口
//void Client::connectToServer(const QString& ip, unsigned short port)
//{

//        std::cout << "请输入密码以连接服务器: ";
//   //std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
//        std::string password;
//        std::getline(std::cin, password);
//       // std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
//        setMessageToSend(QString::fromStdString(password));

//       std::string ipStr = ip.toStdString();


//    networkThread_ = std::thread([this, ipStr, port]() {
//        doConnect(ipStr, port);
//        io_context_.run();
//    });

//}

//gui
void Client::connectToServer(const QString& ip, unsigned short port, const QString& password)
{
    setMessageToSend(password);  // 设置为首条待发送消息

    std::string ipStr = ip.toStdString();

    networkThread_ = std::thread([this, ipStr, port]() {
        doConnect(ipStr, port);
        io_context_.run();  // 启动 boost::asio IO 服务
    });
}



void Client::setMessageToSend(const QString& msg)
{
    messageToSend_ = msg;
}

void Client::doConnect(const std::string& ip, unsigned short port)
{
    socket_ = std::make_unique<tcp::socket>(io_context_);
    resolver_ = std::make_unique<tcp::resolver>(io_context_);

    auto endpoints = resolver_->resolve(ip, std::to_string(port));
    boost::asio::async_connect(*socket_, endpoints,

        [this](boost::system::error_code ec, const tcp::endpoint& endpoint) {

            if (!ec) {
                 doRead();
                emit connectedToServer();//建立连接时，这里发一个信号。
                if (!messageToSend_.isEmpty()) {
                        sendMessage(messageToSend_);

                    }

            } else {
                std::cout << "连接失败，错误信息：" << ec.message() << std::endl;
                QString err = QString::fromStdString(ec.message());
                io_context_.stop();
                emit connectionFailed(err);
            }
        });
}

//5.11修改 增加文件头发送与文件传输功能


void Client::doRead()
{
    auto buf = std::make_shared<boost::asio::streambuf>();
    boost::asio::async_read_until(*socket_, *buf, '\n',
        [this, buf](boost::system::error_code ec, std::size_t /*bytes_transferred*/) {
            if (!ec) {
                std::istream is(buf.get());
                std::string line;
                std::getline(is, line);
                QString qline = QString::fromStdString(line);

                qDebug() << "[服务端]：" << qline;
                emit receivedMessage(qline);  // 后续UI 可绑定

             //5.13更新 防止阻塞回调
                if (qline.contains("请输入要发送文件的路径")) {
                    // 用独立线程读取输入，不阻塞io_ctx
                    std::thread([this]() {
                        std::cout << "请输入文件路径：";
                        std::string path;
                        std::getline(std::cin, path);
                        if (!path.empty()) {
                            sendFile(QString::fromStdString(path));
                        }
                    }).detach();
                }
                else if (qline.trimmed() == "OK") {
                      qDebug() << "服务端同意接受";
                    // 服务端确认接收，发送文件内容
                    sendFileDataChunk();

                }
                else if (qline.trimmed() == "NO") {
                    qDebug() << "服务端拒绝接收文件，连接即将关闭";
                    socket_->close();
                }
                // 持续读取后续消息
                doRead();
            } else {
                emit connectionFailed(QString::fromStdString(ec.message()));
            }
        });
}

//5.11新增文件头发送
void Client::sendFile(const QString& path)
{
    filePath_ = path;
    fileStream_.open(filePath_.toStdString(), std::ios::binary | std::ios::in);

    if (!fileStream_.is_open()) {
        qDebug() << "无法打开文件：" << filePath_;
        return;
    }

    // 获取文件大小
    fileStream_.seekg(0, std::ios::end);
    fileSize_ = fileStream_.tellg();
    fileStream_.seekg(0, std::ios::beg);
    bytesSent_ = 0;

    QFileInfo info(filePath_);
    QString fileName = info.fileName();

    // 构造发送的文件头
    QString header = QString("SEND %1 %2\n").arg(fileName).arg(fileSize_);
    sendMessage(header); // 先发 文件头信息
}


//5.12新增 文件分块发送

void Client::sendFileDataChunk()
{

    if (!fileStream_.is_open() || !socket_ || !socket_->is_open()) {
        qDebug() << "文件流或 socket 无效，无法发送";
        return;
    }
    constexpr std::size_t CHUNK_SIZE = 4096;
    std::vector<char> buffer(CHUNK_SIZE);

    fileStream_.read(buffer.data(), CHUNK_SIZE);
    std::streamsize readSize = fileStream_.gcount();
    if (readSize > 0) {
        //5.14新增分块哈希
        QByteArray dataChunk(buffer.data(), readSize);
        QByteArray hash = QCryptographicHash::hash(dataChunk, QCryptographicHash::Sha256);
        qDebug() << "发送前计算 hash：" << hash.toHex().left(8);
        QByteArray fullBlock;
        fullBlock.append(hash);         // 32字节hash
        fullBlock.append(dataChunk); //数据流
       //发送的fullblock为hash+data
        auto sendBuf = std::make_shared<QByteArray>(fullBlock);
        boost::asio::async_write(*socket_, boost::asio::buffer(sendBuf->data(), sendBuf->size()),
            [this, sendBuf](boost::system::error_code ec, std::size_t bytes_transferred) {
                if (!ec) {
                    bytesSent_ += bytes_transferred-32;
                    //更新offset
                   currentOffset_ = bytesSent_;
                   //更新涉及重传的文件
                   QFile resumeFile(resumeMetaFile_);
                   if (resumeFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                       QTextStream out(&resumeFile);
                       out << "offset=" << currentOffset_;
                       resumeFile.close();
                   }
                   int percent = static_cast<int>((bytesSent_ * 100.0) / fileSize_);
                   emit sendProgress(percent);

//                   int percent = static_cast<int>((bytesSent_ * 100.0) /(fileSize_-currentOffset_1));
//                   static int lastShown = -1;
//                   if (percent != lastShown) {
//                       qDebug() << "客户端已发送:" << percent << "%";
//                       lastShown = percent;
//                   }
                    if (bytesSent_ < fileSize_) {
                        sendFileDataChunk();  // 继续发送
                    } else {
                        qDebug() << "文件发送完成：" << filePath_;
                        fileStream_.close();
                        //删掉涉及重传的文件
                         QFile::remove(resumeMetaFile_);
                          emit sendFinished();
                    }
                } else {
                    qDebug() << "文件发送失败：" << QString::fromStdString(ec.message());
                    fileStream_.close();
                }
            });
    }
}

//5.15新增断点续传

void Client::checkResumeAndStart(const QString& path)
{
    filePath_ = path;
    QFileInfo info(filePath_);
    QString fileName = info.fileName();
    resumeMetaFile_ = filePath_ + ".resume";

    // 获取文件总大小并设置
    fileStream_.open(filePath_.toStdString(), std::ios::binary);
    if (!fileStream_.is_open()) {
        qDebug() << " 无法打开文件：" << filePath_;
        return;
    }
    fileStream_.seekg(0, std::ios::end);
    fileSize_ = fileStream_.tellg();

    fileStream_.close();  // 稍后再打开

    currentOffset_ = 0;

    // 检查是否已有 resume 文件，若有则赋值offset
    if (QFile::exists(resumeMetaFile_)) {
        QFile file(resumeMetaFile_);
        if (file.open(QIODevice::ReadOnly)) {
            QTextStream in(&file);
            QString line = in.readLine();  // e.g., offset=8192
            if (line.startsWith("offset=")) {
                currentOffset_ = line.mid(7).toLongLong();
                qDebug() << "检测到 resume 文件，offset =" << currentOffset_;
            }
        }

        // 发起 RESUME 命令：附带文件大小
        QString resumeCmd = QString("RESUME %1 %2 %3\n")
                                .arg(fileName)
                                .arg(currentOffset_)
                                .arg(fileSize_);
        sendMessage(resumeCmd);
        return;
    }

    // 否则走正常流程：发送 SEND 指令
    sendFile(filePath_);
}

void Client::sendCommandThen(const QString& msg, std::function<void()> onSent)
{
    auto buffer = std::make_shared<QByteArray>(msg.toUtf8());

    boost::asio::async_write(*socket_,
        boost::asio::buffer(buffer->data(), buffer->size()),
        [this, buffer, onSent](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                qDebug() << "命令发送完成：" << buffer->trimmed();
                if (onSent) onSent();
            } else {
                qDebug() << "命令发送失败：" << QString::fromStdString(ec.message());
            }
        });
}

