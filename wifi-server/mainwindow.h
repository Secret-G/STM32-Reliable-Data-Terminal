#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QHash>

#include "deviceprotocolparser.h"

class QTcpServer;
class QTcpSocket;
class QCheckBox;
class QPushButton;
class QProgressBar;
class QTimer;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void toggleServer();
    void handleNewConnection();
    void handleSocketReadyRead(QTcpSocket *socket);
    void handleSocketDisconnected(QTcpSocket *socket);
    void displayFrame(const DeviceFrame &frame, const QString &peer);
    void appendLog(const QString &message);
    void updateConnectionCount();
    void setServerStatus(bool listening, const QString &detail = QString());
    void selectAndStartOta();
    void handleOtaResponse(QTcpSocket *socket, const DeviceFrame &frame);
    void sendOtaStart();
    void sendNextOtaData();
    void sendOtaEnd();
    void sendOtaRequest(const QByteArray &frame, const QString &name);
    void handleOtaResponseTimeout();
    void finishOta(const QString &message, bool success);

    Ui::MainWindow *ui;
    QTcpServer *m_server;
    QHash<QTcpSocket *, DeviceProtocolParser> m_parsers;
    // 跨TCP连接保留最近256个设备ID+序号，用于识别ACK丢失后的重传。
    QList<QByteArray> m_recentFrames;
    QCheckBox *m_autoAck;
    QPushButton *m_otaButton;
    QProgressBar *m_otaProgress;
    QTimer *m_otaResponseTimer;
    QTcpSocket *m_otaSocket = nullptr;
    QByteArray m_otaImage;
    quint32 m_otaVersion = 0;
    quint32 m_otaSequence = 0;
    int m_otaOffset = 0;
    int m_otaChunkSize = 64;
    QByteArray m_otaLastRequest;
    QString m_otaLastRequestName;
    int m_otaRetryCount = 0;
    enum OtaState { OtaIdle, OtaWaitStart, OtaWaitData, OtaWaitEnd } m_otaState = OtaIdle;
};
#endif // MAINWINDOW_H
