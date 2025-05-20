#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include"core/filesender_manager.h"
#include"server.h"
QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    FilesenderManager* fm = nullptr;
    BroadcastManager* manager = nullptr;
    void refreshDeviceList();

private slots:
    void on_btnLogin_clicked();

    void on_btnRegister_clicked();

    void on_btnRefreshDevices_clicked();

    void on_tableDevices_cellClicked(int row, int column);


private:
    Server* server = nullptr;
    Ui::MainWindow *ui;
};
#endif // MAINWINDOW_H
