#include "file_receivedialog.h"
#include <QVBoxLayout>

FileReceiveDialog::FileReceiveDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("正在接收文件");
    resize(400, 150);

    lblFileName = new QLabel("文件名：", this);
    progressBar = new QProgressBar(this);
    progressBar->setRange(0, 100);
    btnClose = new QPushButton("关闭", this);
    btnClose->setEnabled(false);

    auto layout = new QVBoxLayout(this);
    layout->addWidget(lblFileName);
    layout->addWidget(progressBar);
    layout->addWidget(btnClose);

    connect(btnClose, &QPushButton::clicked, this, [this]() {
        emit closedByUser();  // 通知外部
        this->close();        // 自己关闭
    });
}

void FileReceiveDialog::setFileName(const QString& name)
{
    lblFileName->setText("文件名：" + name);
}

void FileReceiveDialog::updateProgress(int percent)
{
    progressBar->setValue(percent);
    if (percent >= 100) {
        btnClose->setEnabled(true);
        setWindowTitle("接收完成");
    }
}
