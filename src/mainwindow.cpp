#include "mainwindow.h"
#include "ui_mainwindow.h"
#include"authentication/authenticator.h"
#include <QMessageBox>
#include <QString>
#include"core/filesender_manager.h"
#include"broadcast/broadcast_manager.h"
#include <fstream>
#include"deviceoperationwindow.h"
#include <QInputDialog>
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 判断用户数据文件是否存在，控制默认界面显示
    std::ifstream infile(Authenticator::PROFILE_PATH);

    if (infile.good()) {
        // 存在用户配置，显示登录界面
        ui->stackedWidget->setCurrentWidget(ui->pageLogin);
    } else {
        // 不存在用户配置，首次使用，进入注册界面
        ui->stackedWidget->setCurrentWidget(ui->pageRegister);
    }


}

MainWindow::~MainWindow()
{
    delete ui;
    if (server) {
        delete server;
        server = nullptr;
    }
     delete fm;  // 终止广播线程

}


void MainWindow::on_btnRegister_clicked()
{
    QString qname = ui->lineEditRegisterUsername->text();
    QString qpwd  = ui->lineEditRegisterPassword->text();

    if (qname.isEmpty() || qpwd.isEmpty()) {
        QMessageBox::warning(this, "错误", "用户名和密码不能为空！");
        return;
    }

    std::string name = qname.toStdString();
    std::string password = qpwd.toStdString();

    Authenticator auth;
    auth.regist(name, password);

    QMessageBox::information(this, "注册成功", "请返回登录界面登录！");
    ui->stackedWidget->setCurrentWidget(ui->pageLogin);  // 回到登录界面
}


void MainWindow::on_btnLogin_clicked()
{
    QString password_q = ui->lineEditLoginPassword->text();
    std::string password = password_q.toStdString();

    Authenticator auth;
    if (auth.login(password)) {
        Account acc = auth.get_profile();
        short port = 8888;
        fm = new FilesenderManager(acc, port);
        manager = &fm->broadcast_manager;

        if (!server) {
             server = new Server();
            // server->startServer(port);
         }
        // 切换页面
        ui->stackedWidget->setCurrentWidget(ui->pageDevices);

        // 触发一次刷新
        refreshDeviceList();

    } else {
        QMessageBox::warning(this, "登录失败", "密码错误或用户不存在！");
    }
}

void MainWindow::refreshDeviceList()
{
    ui->tableDevices->setRowCount(0); // 清空
    ui->labelStatus->setText("正在刷新...");

    std::thread([=]() {
        auto devices = manager->receiver_list(8888);

        QMetaObject::invokeMethod(this, [=]() {
            ui->tableDevices->setRowCount(devices.size());
            for (int i = 0; i < devices.size(); ++i) {
                const Account& acc = devices[i];
                ui->tableDevices->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(acc.name)));
                ui->tableDevices->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(acc.ip)));
                ui->tableDevices->setItem(i, 2, new QTableWidgetItem(QString::number(acc.port)));
            }
            ui->labelStatus->setText(QString("已发现 %1 个设备").arg(devices.size()));
        });

    }).detach();
}


void MainWindow::on_btnRefreshDevices_clicked()
{
     refreshDeviceList();
}

void MainWindow::on_tableDevices_cellClicked(int row, int column)
{
    QString name = ui->tableDevices->item(row, 0)->text();
    QString ip   = ui->tableDevices->item(row, 1)->text();
    QString portStr = ui->tableDevices->item(row, 2)->text();

    auto result = manager->find_account_by_name(name.toStdString());
    if (!result) {
        QMessageBox::warning(this, "错误", "找不到设备信息！");
        return;
    }

    Account target = result.value();
    unsigned short port = static_cast<unsigned short>(target.port);

    //  弹出密码输入框
    bool ok = false;
    QString password = QInputDialog::getText(
        this,
        "连接密码",
        QString("请输入连接 %1 的密码：").arg(name),
        QLineEdit::Password,
        "", &ok
    );

    if (!ok || password.isEmpty()) {
        QMessageBox::information(this, "已取消", "连接操作已取消");
        return;
    }

    // 创建操作窗口（每个设备一个窗口）
    auto* window = new deviceoperationwindow(target);
    connect(&window->getClient(), &Client::authPassed, window, [window]() {
           window->show();  // 只有通过认证才真正弹出窗口

       });

       // 如果失败，提示用户
    connect(&window->getClient(), &Client::authFailed, this, [this, window]() {
        QMessageBox::warning(this, "错误", "密码错误");
        delete window;
    });

    // 启动连接
    window->getClient().connectToServer(ip, port, password);
}


