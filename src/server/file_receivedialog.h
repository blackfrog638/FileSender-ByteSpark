#ifndef FILE_RECEIVEDIALOG_H
#define FILE_RECEIVEDIALOG_H


#pragma once

#include <QDialog>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>

class FileReceiveDialog : public QDialog {
    Q_OBJECT
public:
    explicit FileReceiveDialog(QWidget* parent = nullptr);

    void setFileName(const QString& name);
    void updateProgress(int percent);
signals:
    void closedByUser();  // 通知外部：用户主动点击了“关闭”

private:
    QLabel* lblFileName;
    QProgressBar* progressBar;
    QPushButton* btnClose;
};

#endif // FILE_RECEIVEDIALOG_H
