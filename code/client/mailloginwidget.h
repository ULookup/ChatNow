#ifndef MAILLOGINWIDGET_H
#define MAILLOGINWIDGET_H

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QTimer>

class MailLoginWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MailLoginWidget(QWidget *parent = nullptr);

    void sendVerifyCode();
    void sendVerifyCodeDone();

    void clickSubmitBtn();
    void mailLoginDone(bool ok, const QString& reason);
    void mailRegisterDone(bool ok, const QString& reason);

    void countDown();
    void switchMode();
private:
    QLineEdit* mailEdit;
    QPushButton* sendVerifyCodeBtn;
    QLineEdit* verifyCodeEdit;
    QLabel* titleLabel;
    QPushButton* submitBtn;
    QPushButton* switchModeBtn;

    bool isLoginMode = true;
    QString currentMail = "";    // 记录是使用哪个手机号发送的验证码
    QTimer* timer;
    int leftTime = 30;

signals:
};

#endif // MAILLOGINWIDGET_H
