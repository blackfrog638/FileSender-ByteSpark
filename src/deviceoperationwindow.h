#ifndef DEVICEOPERATIONWINDOW_H
#define DEVICEOPERATIONWINDOW_H

#pragma once
#include <QWidget>
#include <QStringList>
#include "core/filesender_manager.h"
#include"client.h"
#include "filesenddialog.h"
namespace Ui {
class deviceoperationwindow;
}

class deviceoperationwindow : public QWidget
{
    Q_OBJECT

public:
    explicit deviceoperationwindow(const Account& acc, QWidget *parent = nullptr);

    ~deviceoperationwindow();
 Client client;
 Client& getClient() { return client; }
private slots:
    void on_btnSelectFile_clicked();  // 由 UI 自动连接

    void on_btnSendFile_clicked();

private:
    Ui::deviceoperationwindow *ui;
    Account target_account;
    QString selectedFilePath;

   FileSendDialog* fileSendDialog = nullptr;
};


#endif // DEVICEOPERATIONWINDOW_H
