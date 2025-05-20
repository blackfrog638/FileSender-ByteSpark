#ifndef DEVICECLIENTWINDOW_H
#define DEVICECLIENTWINDOW_H

#pragma once
#include <QWidget>
#include <QStringList>
#include "core/filesender_manager.h"

namespace Ui {
class DeviceOperationWindow;
}

class DeviceOperationWindow : public QWidget
{
    Q_OBJECT

public:
    explicit DeviceOperationWindow(const Account& acc, QWidget *parent = nullptr);
    ~DeviceOperationWindow();

private slots:
    void on_btnSelectFile_clicked();  // 由 UI 自动连接

private:
    Ui::DeviceOperationWindow *ui;
    Account target_account;
    QStringList selected_files;
};

#endif // DEVICECLIENTWINDOW_H
