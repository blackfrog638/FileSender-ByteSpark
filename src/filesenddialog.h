#ifndef FILESENDDIALOG_H
#define FILESENDDIALOG_H


#pragma once

#include <QDialog>
#include <QProgressBar>
#include <QPushButton>
#include <QLabel>

class FileSendDialog : public QDialog {
    Q_OBJECT
public:
    explicit FileSendDialog(QWidget* parent = nullptr);

    void setFileName(const QString& name);
    void updateProgress(int percent);

signals:
    void cancelRequested();

private:
    QLabel* lblName;
    QProgressBar* progressBar;
    QPushButton* btnCancel;
};


#endif // FILESENDDIALOG_H
