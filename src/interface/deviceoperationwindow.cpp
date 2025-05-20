#include "deviceoperationwindow.h"
#include "ui_deviceoperationwindow.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QDebug>
#include "filesenddialog.h"
deviceoperationwindow::deviceoperationwindow(const Account& acc, QWidget *parent ) :
    QWidget(parent),
    ui(new Ui::deviceoperationwindow)
{

    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    setWindowTitle(QString("连接设备：%1").arg(QString::fromStdString(acc.name)));

    ui->labelStatus->setText(QString("用户名: %1\nIP: %2\n端口: %3")
        .arg(QString::fromStdString(acc.name))
        .arg(QString::fromStdString(acc.ip))
        .arg(target_account.port));


}

deviceoperationwindow::~deviceoperationwindow()
{
    delete ui;
}

void deviceoperationwindow::on_btnSelectFile_clicked()
{
    QString filePath = QFileDialog::getOpenFileName(this, "选择文件");
    if (!filePath.isEmpty()) {
        selectedFilePath = filePath;
        ui->listFiles->addItem(filePath);  // 显示在列表中
    }
}

void deviceoperationwindow::on_btnSendFile_clicked()
{       if (selectedFilePath.isEmpty()) {
        QMessageBox::warning(this, "错误", "请先选择要发送的文件！");
        return;
    }

    // 创建进度窗口
    fileSendDialog = new FileSendDialog(this);
    fileSendDialog->setFileName(selectedFilePath);
    fileSendDialog->setAttribute(Qt::WA_DeleteOnClose);
    fileSendDialog->show();

    // 连接发送进度信号
    connect(&client, &Client::sendProgress,
            fileSendDialog, &FileSendDialog::updateProgress);

    // 连接取消发送逻辑
    connect(fileSendDialog, &FileSendDialog::cancelRequested, this, [this]() {
        client.cancelSend();
        fileSendDialog->close();
    });

        //服务端拒绝接收
        connect(&client, &Client::sendRejected, this, [this]() {
             if (fileSendDialog) {
                 fileSendDialog->close();
                 fileSendDialog = nullptr;
             }

        QMessageBox::warning(this, "发送被拒绝", "服务端拒绝接收该文件！");
        client.cancelSend();
        this->close();        // 关闭当前窗口
         });
         //拒绝重传
      connect(&client, &Client::resumeRejected, this, [this]() {
                if (fileSendDialog) {
                    fileSendDialog->close();
                    fileSendDialog = nullptr;
                }
                client.cancelSend_resume();
                this->close();        // 关闭当前窗口
       QMessageBox::information(this, "续传被拒绝", "服务端拒绝了断点续传请求，将重新开始完整发送。");
            });
//发送完成
         connect(&client, &Client::sendFinished, this, [this]() {
              if (fileSendDialog) {
                  fileSendDialog->close();
                  fileSendDialog = nullptr;
              }

            QMessageBox::information(this, "发送成功", "文件发送完成！");

              client.cancelSend();
              this->close();        // 销毁窗口
          });

    // 最后启动发送流程
    client.checkResumeAndStart(selectedFilePath);
}
