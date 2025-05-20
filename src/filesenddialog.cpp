#include "filesenddialog.h"
#include <QVBoxLayout>

FileSendDialog::FileSendDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("文件发送中");
    resize(400, 150);

    lblName = new QLabel(this);
    progressBar = new QProgressBar(this);
    progressBar->setRange(0, 100);
    btnCancel = new QPushButton("取消发送", this);

    auto layout = new QVBoxLayout(this);
    layout->addWidget(lblName);
    layout->addWidget(progressBar);
    layout->addWidget(btnCancel);

    connect(btnCancel, &QPushButton::clicked, this, &FileSendDialog::cancelRequested);
}

void FileSendDialog::setFileName(const QString& name)
{
    lblName->setText("文件：" + name);
}

void FileSendDialog::updateProgress(int percent)
{
    progressBar->setValue(percent);
    if (percent >= 100) {
        btnCancel->setEnabled(false);
        setWindowTitle("发送完成");
    }
}
