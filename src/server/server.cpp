#include "server.h"
#include "session.h"
#include <QDebug>
#include <QMessageBox>
#include "file_receivedialog.h"

using boost::asio::ip::tcp;

Server::Server(QObject* parent)
    : QObject(parent)
{}

Server::~Server()
{
    stopServer();
    qDebug() << "Session 已被析构";
}

void Server::startServer(unsigned short port)
{    qDebug() << "传输端口：" << port;

    acceptor_ = std::make_unique<tcp::acceptor>(io_context_, tcp::endpoint(tcp::v4(), port));

    running_ = true;

    doAccept();

    networkThread_ = std::thread([this]() {

        io_context_.run();

    });
}

void Server::stopServer()
{
    if (running_) {
        io_context_.stop();
        if (networkThread_.joinable()) {
            networkThread_.join();
        }
        running_ = false;
    }

}


void Server::doAccept() {
    acceptor_->async_accept(
        [this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                qDebug() << "收到新连接，创建 Session";

                auto session = std::make_shared<Session>(std::move(socket));
                sessions_.insert(session);

             connect(session.get(), &Session::sessionStarted, this, &Server::newSessionConnected);//当有新连接建立时emit sessionStarted ，newSessionConnected通知主线程
             connect(session.get(), &Session::sessionEnded, this, [this, session]() {
                    sessions_.erase(session);
                    qDebug() << "Session 已结束并已移除";
                });

             connect(session.get(), &Session::fileTransferRequest,
                     this, &Server::onFileTransferRequest ,Qt::QueuedConnection);

             connect(session.get(), &Session::resumeTransferRequest,
                     this, &Server::onResumeTransferRequest);


                session->start();
            }

            if (running_) {
                doAccept();  // 继续接收新连接
            }
        });
}

void Server::onFileTransferRequest(Session* session,
                                   const QString& fileName,
                                   std::size_t fileSize)
{
    QString msg = QString("客户端请求发送文件：\n文件名：%1\n大小：%2 字节\n是否接受？")
                      .arg(fileName)
                      .arg(fileSize);

    auto reply = QMessageBox::question(nullptr, "文件接收请求", msg,
                                       QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::No) {
        session->rejectFileTransfer();
        return;
    }

    // 用户选择“接受”，弹出进度框
    auto dialog = new FileReceiveDialog();
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileName(fileName);
    dialog->show();

    connect(session, &Session::receiveProgress,
            dialog, &FileReceiveDialog::updateProgress);

    session->acceptFileTransfer();  // 开始接收
    connect(dialog, &FileReceiveDialog::closedByUser, this, [this, session]() {
        session->closeSession();  // 清理 session 资源
    });

}


void Server::onResumeTransferRequest(Session* session,
                                     const QString& fileName,
                                     std::size_t offset,
                                     std::size_t totalSize)
{
    QString msg = QString("客户端请求续传文件：\n文件名：%1\n已接收：%2 / %3 字节\n是否接受？")
                      .arg(fileName)
                      .arg(offset)
                      .arg(totalSize);

    auto reply = QMessageBox::question(nullptr, "断点续传请求", msg,
                                       QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        auto dialog = new FileReceiveDialog();
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setFileName(fileName);
        dialog->show();

        connect(session, &Session::receiveProgress,
                dialog, &FileReceiveDialog::updateProgress);

        session->acceptResume();
        connect(dialog, &FileReceiveDialog::closedByUser, this, [this, session]() {
            session->closeSession();  // 清理 session 资源
        });

    } else {
        session->rejectResume();
    }
}
