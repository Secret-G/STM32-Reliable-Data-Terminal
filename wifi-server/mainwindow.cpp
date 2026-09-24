#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QHeaderView>
#include <QHostAddress>
#include <QMessageBox>
#include <QScrollBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>

namespace {

constexpr int OtaResponseTimeoutMs = 3000;
constexpr int OtaMaxRetries = 3;

quint16 otaImageCrc16Modbus(const QByteArray &data)
{
    quint16 crc = 0xFFFFU;
    for (const char rawByte : data) {
        crc ^= quint8(rawByte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? quint16((crc >> 1U) ^ 0xA001U) : quint16(crc >> 1U);
    }
    return crc;
}

void appendU16BE(QByteArray &data, quint16 value)
{
    data.append(char(value >> 8U));
    data.append(char(value & 0xFFU));
}

void appendU32BE(QByteArray &data, quint32 value)
{
    data.append(char(value >> 24U));
    data.append(char((value >> 16U) & 0xFFU));
    data.append(char((value >> 8U) & 0xFFU));
    data.append(char(value & 0xFFU));
}

quint16 readU16BE(const QByteArray &data, int offset)
{
    return (quint16(quint8(data.at(offset))) << 8U) | quint16(quint8(data.at(offset + 1)));
}

quint32 readU32BE(const QByteArray &data, int offset)
{
    return (quint32(quint8(data.at(offset))) << 24U)
           | (quint32(quint8(data.at(offset + 1))) << 16U)
           | (quint32(quint8(data.at(offset + 2))) << 8U)
           | quint32(quint8(data.at(offset + 3)));
}

quint32 protocolTimestamp()
{
    return quint32(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFFULL);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_server(new QTcpServer(this))
    , m_otaResponseTimer(new QTimer(this))
{
    ui->setupUi(this);
    m_autoAck = new QCheckBox(QStringLiteral("自动回复 ACK（取消可测试重传）"), this);
    m_autoAck->setChecked(true);
    statusBar()->addPermanentWidget(m_autoAck);
    m_otaProgress = new QProgressBar(this);
    m_otaProgress->setRange(0, 100);
    m_otaProgress->setValue(0);
    m_otaProgress->setFixedWidth(150);
    m_otaButton = new QPushButton(QStringLiteral("选择固件并 OTA"), this);
    statusBar()->addPermanentWidget(m_otaProgress);
    statusBar()->addPermanentWidget(m_otaButton);

    ui->contentSplitter->setSizes({430, 190});
    ui->frameTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->frameTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->frameTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->frameTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->frameTable->verticalHeader()->setVisible(false);

    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget#centralwidget {
            background: #0b1120;
            color: #dbe7f5;
            font-family: "Microsoft YaHei UI";
            font-size: 14px;
        }
        QFrame#headerFrame, QFrame#controlFrame, QFrame#tableFrame, QFrame#rawFrame,
        QFrame#deviceCard, QFrame#sequenceCard, QFrame#valueCard, QFrame#crcCard {
            background: #121c2e;
            border: 1px solid #22324a;
            border-radius: 12px;
        }
        QLabel#titleLabel { color: #f5f9ff; font-size: 25px; font-weight: 700; }
        QLabel#subtitleLabel { color: #7890ad; font-size: 13px; }
        QLabel#serverStatusLabel {
            background: #202b3e; color: #91a1b7; border-radius: 16px;
            padding: 7px 15px; font-weight: 600;
        }
        QLabel#connectionCountLabel { color: #8fa5c0; font-weight: 600; padding-right: 8px; }
        QLabel#deviceCardTitle, QLabel#sequenceCardTitle, QLabel#valueCardTitle, QLabel#crcCardTitle {
            color: #7f94ae; font-size: 13px;
        }
        QLabel#deviceIdValue, QLabel#sequenceValue, QLabel#realtimeValue, QLabel#crcValue {
            color: #f4f8ff; font-size: 22px; font-weight: 700;
        }
        QLabel#tableTitle, QLabel#rawTitle { color: #eaf2ff; font-size: 15px; font-weight: 700; }
        QLineEdit, QSpinBox {
            background: #0d1626; border: 1px solid #2a3c57; border-radius: 7px;
            padding: 6px 10px; color: #e6eef9; selection-background-color: #2878e8;
        }
        QLineEdit:focus, QSpinBox:focus { border: 1px solid #3486f4; }
        QPushButton {
            background: #1c2a40; border: 1px solid #30445f; border-radius: 7px;
            color: #dce9f8; padding: 7px 14px; font-weight: 600;
        }
        QPushButton:hover { background: #263a56; border-color: #47709e; }
        QPushButton#startServerButton { background: #1677e8; border-color: #278cff; color: white; }
        QPushButton#startServerButton:hover { background: #2788f2; }
        QTableWidget {
            background: #0d1626; alternate-background-color: #111d30;
            border: 1px solid #21334d; border-radius: 7px; gridline-color: #1e2c42;
            color: #cbd9e9; selection-background-color: #214a78;
        }
        QHeaderView::section {
            background: #17243a; color: #92a8c2; border: none;
            border-right: 1px solid #273952; padding: 9px; font-weight: 600;
        }
        QPlainTextEdit {
            background: #09111d; border: 1px solid #20314a; border-radius: 7px;
            color: #8fddbd; padding: 8px; font-family: "Cascadia Mono", Consolas, monospace;
            font-size: 12px;
        }
        QSplitter::handle { background: transparent; height: 10px; }
        QStatusBar { background: #0b1120; color: #71849c; }
        QScrollBar:vertical { background: #0d1626; width: 10px; margin: 0; }
        QScrollBar::handle:vertical { background: #334963; border-radius: 5px; min-height: 24px; }
    )"));

    connect(ui->startServerButton, &QPushButton::clicked, this, &MainWindow::toggleServer);
    connect(ui->clearButton, &QPushButton::clicked, this, [this] {
        ui->frameTable->setRowCount(0);
        ui->rawHexEdit->clear();
        ui->deviceIdValue->setText(QStringLiteral("--"));
        ui->sequenceValue->setText(QStringLiteral("--"));
        ui->realtimeValue->setText(QStringLiteral("--"));
        ui->crcValue->setText(QStringLiteral("等待数据"));
        ui->crcValue->setStyleSheet(QString());
        // 清空显示记录时同步开启一个新的去重观测周期。
        m_recentFrames.clear();
    });
    connect(m_server, &QTcpServer::newConnection, this, &MainWindow::handleNewConnection);
    connect(m_otaButton, &QPushButton::clicked, this, &MainWindow::selectAndStartOta);
    m_otaResponseTimer->setSingleShot(true);
    connect(m_otaResponseTimer, &QTimer::timeout, this, &MainWindow::handleOtaResponseTimeout);

    statusBar()->showMessage(QStringLiteral("准备就绪 · 协议帧头 AA 55 · CRC-16/CCITT-FALSE"));
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::toggleServer()
{
    if (m_server->isListening()) {
        const auto sockets = m_parsers.keys();
        for (QTcpSocket *socket : sockets)
            socket->disconnectFromHost();
        m_server->close();
        setServerStatus(false);
        appendLog(QStringLiteral("服务器已停止监听"));
        return;
    }

    QHostAddress address;
    const QString addressText = ui->listenAddressEdit->text().trimmed();
    /*
     * 0.0.0.0表示监听本机全部IPv4网卡，电脑的有线/无线IP变化后无需改界面。
     * 这里只影响Qt服务端绑定地址；ESP32的CIPSTART仍需填写电脑实际局域网IP。
     */
    if (addressText.isEmpty() || addressText == QStringLiteral("0.0.0.0"))
        address = QHostAddress::AnyIPv4;
    else if (!address.setAddress(addressText)) {
        QMessageBox::warning(this, QStringLiteral("监听失败"), QStringLiteral("监听地址格式不正确。"));
        return;
    }

    const quint16 port = quint16(ui->portSpinBox->value());
    if (!m_server->listen(address, port)) {
        QMessageBox::critical(this, QStringLiteral("监听失败"), m_server->errorString());
        appendLog(QStringLiteral("[ERROR] %1").arg(m_server->errorString()));
        return;
    }

    setServerStatus(true, QStringLiteral("%1:%2").arg(addressText).arg(port));
    appendLog(QStringLiteral("开始监听 %1:%2").arg(addressText).arg(port));
}

void MainWindow::handleNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        m_parsers.insert(socket, DeviceProtocolParser());

        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            handleSocketReadyRead(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            handleSocketDisconnected(socket);
        });
        connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
            appendLog(QStringLiteral("[SOCKET ERROR] %1 · %2")
                          .arg(socket->peerAddress().toString(), socket->errorString()));
        });

        appendLog(QStringLiteral("[CONNECTED] %1:%2")
                      .arg(socket->peerAddress().toString())
                      .arg(socket->peerPort()));
    }
    updateConnectionCount();
}

void MainWindow::handleSocketReadyRead(QTcpSocket *socket)
{
    const QByteArray incoming = socket->readAll();
    if (incoming.isEmpty())
        return;

    const QString peer = QStringLiteral("%1:%2")
                             .arg(socket->peerAddress().toString())
                             .arg(socket->peerPort());
    appendLog(QStringLiteral("RX %1 · %2 bytes · %3")
                  .arg(peer)
                  .arg(incoming.size())
                  .arg(QString::fromLatin1(incoming.toHex(' ').toUpper())));

    auto parserIt = m_parsers.find(socket);
    if (parserIt == m_parsers.end())
        return;

    const QList<DeviceFrame> frames = parserIt.value().feed(incoming);
    for (const DeviceFrame &frame : frames) {
        if (frame.messageType == 0xA0U) {
            displayFrame(frame, peer);
            handleOtaResponse(socket, frame);
            continue;
        }

        const QByteArray ack = DeviceProtocolParser::makeAck(frame);
        bool duplicate = false;
        if (!ack.isEmpty()) {
            QByteArray key; // 幂等键固定为大端的device_id + sequence，不受实时/历史类型影响。
            key.reserve(8);
            key.append(char((frame.deviceId >> 24) & 0xFFU));
            key.append(char((frame.deviceId >> 16) & 0xFFU));
            key.append(char((frame.deviceId >> 8) & 0xFFU));
            key.append(char(frame.deviceId & 0xFFU));
            key.append(char((frame.sequence >> 24) & 0xFFU));
            key.append(char((frame.sequence >> 16) & 0xFFU));
            key.append(char((frame.sequence >> 8) & 0xFFU));
            key.append(char(frame.sequence & 0xFFU));

            duplicate = m_recentFrames.contains(key);
            if (!duplicate) {
                m_recentFrames.append(key);
                if (m_recentFrames.size() > 256) m_recentFrames.removeFirst();
            }
        }
        if (duplicate)
            appendLog(QStringLiteral("DUPLICATE %1 · SEQ=%2 · 不重复入表").arg(peer).arg(frame.sequence));
        else
            displayFrame(frame, peer);
        if (!ack.isEmpty()) {
            // 重复帧也必须回复：可能上一份ACK在返回途中丢失。
            if (!m_autoAck->isChecked()) {
                appendLog(QStringLiteral("ACK SUPPRESSED · SEQ=%1（测试开关）").arg(frame.sequence));
                continue;
            }
            // write仅表示交给Qt发送缓冲区；STM32的APP ACK日志验证实际到达。
            const qint64 written = socket->write(ack);
            appendLog(written == ack.size()
                ? QStringLiteral("TX ACK QUEUED %1 · SEQ=%2 · %3")
                    .arg(peer).arg(frame.sequence).arg(QString::fromLatin1(ack.toHex(' ').toUpper()))
                : QStringLiteral("[ACK WRITE FAILED] %1 · SEQ=%2 · %3")
                    .arg(peer).arg(frame.sequence).arg(socket->errorString()));
        }
    }
}

void MainWindow::handleSocketDisconnected(QTcpSocket *socket)
{
    appendLog(QStringLiteral("[CLOSED] %1:%2")
                  .arg(socket->peerAddress().toString())
                  .arg(socket->peerPort()));
    m_parsers.remove(socket);
    if (socket == m_otaSocket && m_otaState != OtaIdle)
        finishOta(QStringLiteral("OTA连接已断开"), false);
    // 去重窗口不随socket断开清空，重连后历史补传仍可识别同一业务帧。
    socket->deleteLater();
    updateConnectionCount();
}

void MainWindow::selectAndStartOta()
{
    if (m_otaState != OtaIdle) {
        QMessageBox::information(this, QStringLiteral("OTA进行中"), QStringLiteral("请等待当前OTA会话结束。"));
        return;
    }

    const auto sockets = m_parsers.keys();
    if (sockets.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法开始OTA"), QStringLiteral("当前没有在线设备。"));
        return;
    }

    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择APP固件"), QString(),
                                                       QStringLiteral("Binary firmware (*.bin);;All files (*.*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, QStringLiteral("读取失败"), file.errorString());
        return;
    }
    const QByteArray image = file.readAll();
    if (image.isEmpty() || image.size() > 0x40000) {
        QMessageBox::warning(this, QStringLiteral("固件无效"), QStringLiteral("BIN必须非空且不超过256 KB。"));
        return;
    }

    bool ok = false;
    const QString versionText = QInputDialog::getText(this, QStringLiteral("固件版本"),
                                                       QStringLiteral("输入8位十六进制版本，例如 01020304："),
                                                       QLineEdit::Normal, QStringLiteral("01000000"), &ok);
    const quint32 version = versionText.toUInt(&ok, 16);
    if (!ok || version == 0U) {
        QMessageBox::warning(this, QStringLiteral("版本无效"), QStringLiteral("请输入非零的32位十六进制版本。"));
        return;
    }

    m_otaSocket = sockets.first();
    m_otaImage = image;
    m_otaVersion = version;
    m_otaSequence = 0U;
    m_otaOffset = 0;
    m_otaChunkSize = 64;
    m_otaProgress->setValue(0);
    m_otaButton->setEnabled(false);
    m_otaState = OtaWaitStart;
    sendOtaStart();
}

void MainWindow::sendOtaStart()
{
    QByteArray payload;
    appendU32BE(payload, quint32(m_otaImage.size()));
    appendU16BE(payload, otaImageCrc16Modbus(m_otaImage));
    appendU32BE(payload, m_otaVersion);
    const QByteArray frame = DeviceProtocolParser::makeFrame(0x20U, 1U, 0U,
                                                              protocolTimestamp(), payload);
    sendOtaRequest(frame, QStringLiteral("START"));
    appendLog(QStringLiteral("OTA START TX · SIZE=%1 CRC=0x%2 VERSION=0x%3")
                  .arg(m_otaImage.size())
                  .arg(otaImageCrc16Modbus(m_otaImage), 4, 16, QLatin1Char('0'))
                  .arg(m_otaVersion, 8, 16, QLatin1Char('0')).toUpper());
}

void MainWindow::sendNextOtaData()
{
    if (m_otaOffset >= m_otaImage.size()) {
        sendOtaEnd();
        return;
    }

    const QByteArray payload = m_otaImage.mid(m_otaOffset, m_otaChunkSize);
    const QByteArray frame = DeviceProtocolParser::makeFrame(0x21U, 1U, m_otaSequence,
                                                              protocolTimestamp(), payload);
    m_otaState = OtaWaitData;
    sendOtaRequest(frame, QStringLiteral("DATA SEQ=%1").arg(m_otaSequence));
}

void MainWindow::sendOtaEnd()
{
    QByteArray payload;
    appendU32BE(payload, m_otaSequence);
    const QByteArray frame = DeviceProtocolParser::makeFrame(0x22U, 1U, m_otaSequence,
                                                              protocolTimestamp(), payload);
    m_otaState = OtaWaitEnd;
    sendOtaRequest(frame, QStringLiteral("END"));
    appendLog(QStringLiteral("OTA END TX · PACKETS=%1").arg(m_otaSequence));
}

void MainWindow::handleOtaResponse(QTcpSocket *socket, const DeviceFrame &frame)
{
    if (socket != m_otaSocket || m_otaState == OtaIdle || !frame.crcOk() || frame.payload.size() != 9)
        return;

    const quint8 requestType = quint8(frame.payload.at(0));
    const quint16 result = readU16BE(frame.payload, 1);
    const quint32 value = readU32BE(frame.payload, 3);
    const quint16 maxDataLength = readU16BE(frame.payload, 7);

    const quint8 expectedType = (m_otaState == OtaWaitStart) ? 0x20U
                                : (m_otaState == OtaWaitData) ? 0x21U
                                                             : 0x22U;
    if (requestType != expectedType)
        return;
    const quint32 expectedSequence = (m_otaState == OtaWaitStart) ? 0U : m_otaSequence;
    if (frame.sequence != expectedSequence)
        return;

    m_otaResponseTimer->stop();

    if (result != 0U) {
        finishOta(QStringLiteral("OTA失败：请求=0x%1 结果=0x%2 详情=%3")
                      .arg(requestType, 2, 16, QLatin1Char('0'))
                      .arg(result, 4, 16, QLatin1Char('0')).arg(value).toUpper(), false);
        return;
    }

    if (m_otaState == OtaWaitStart && requestType == 0x20U) {
        if (maxDataLength == 0U || maxDataLength > 64U) {
            finishOta(QStringLiteral("设备返回的OTA包长无效：%1").arg(maxDataLength), false);
            return;
        }
        m_otaChunkSize = maxDataLength;
        appendLog(QStringLiteral("OTA START ACK · SLOT=%1 MAX_DATA=%2").arg(value).arg(maxDataLength));
        sendNextOtaData();
        return;
    }

    if (m_otaState == OtaWaitData && requestType == 0x21U && frame.sequence == m_otaSequence) {
        m_otaOffset += qMin(m_otaChunkSize, m_otaImage.size() - m_otaOffset);
        ++m_otaSequence;
        m_otaProgress->setValue((m_otaOffset * 100) / m_otaImage.size());
        sendNextOtaData();
        return;
    }

    if (m_otaState == OtaWaitEnd && requestType == 0x22U) {
        m_otaProgress->setValue(100);
        finishOta(QStringLiteral("OTA发送完成，设备即将复位安装"), true);
    }
}

void MainWindow::finishOta(const QString &message, bool success)
{
    m_otaResponseTimer->stop();
    appendLog((success ? QStringLiteral("[OTA OK] ") : QStringLiteral("[OTA ERROR] ")) + message);
    m_otaState = OtaIdle;
    m_otaSocket = nullptr;
    m_otaImage.clear();
    m_otaLastRequest.clear();
    m_otaLastRequestName.clear();
    m_otaRetryCount = 0;
    m_otaButton->setEnabled(true);
    if (!success)
        m_otaProgress->setValue(0);
}

void MainWindow::sendOtaRequest(const QByteArray &frame, const QString &name)
{
    if (m_otaSocket == nullptr || frame.isEmpty()) {
        finishOta(QStringLiteral("%1帧无效或设备已断开").arg(name), false);
        return;
    }

    m_otaLastRequest = frame;
    m_otaLastRequestName = name;
    m_otaRetryCount = 0;
    const qint64 written = m_otaSocket->write(m_otaLastRequest);
    if (written != m_otaLastRequest.size())
        appendLog(QStringLiteral("[OTA WRITE WARNING] %1 · %2").arg(name, m_otaSocket->errorString()));
    m_otaResponseTimer->start(OtaResponseTimeoutMs);
}

void MainWindow::handleOtaResponseTimeout()
{
    if (m_otaState == OtaIdle || m_otaSocket == nullptr || m_otaLastRequest.isEmpty())
        return;

    if (m_otaRetryCount >= OtaMaxRetries) {
        finishOta(QStringLiteral("%1响应超时，已重发%2次")
                      .arg(m_otaLastRequestName).arg(OtaMaxRetries), false);
        return;
    }

    ++m_otaRetryCount;
    const qint64 written = m_otaSocket->write(m_otaLastRequest);
    appendLog(QStringLiteral("OTA %1 TIMEOUT · RETRY=%2/%3 · WRITE=%4")
                  .arg(m_otaLastRequestName)
                  .arg(m_otaRetryCount)
                  .arg(OtaMaxRetries)
                  .arg(written));
    m_otaResponseTimer->start(OtaResponseTimeoutMs);
}

void MainWindow::displayFrame(const DeviceFrame &frame, const QString &peer)
{
    Q_UNUSED(peer)

    const QString deviceText = QStringLiteral("0x%1")
                                   .arg(frame.deviceId, 8, 16, QLatin1Char('0')).toUpper();
    const QString payloadText = QString::fromLatin1(frame.payload.toHex(' ').toUpper());
    const QString crcText = frame.crcOk() ? QStringLiteral("OK") : QStringLiteral("FAILED");

    ui->deviceIdValue->setText(deviceText);
    ui->sequenceValue->setText(QString::number(frame.sequence));
    if (frame.messageType == 0x01 && frame.payload.size() == 4) {
        const quint32 value = (quint32(quint8(frame.payload.at(0))) << 24U)
                              | (quint32(quint8(frame.payload.at(1))) << 16U)
                              | (quint32(quint8(frame.payload.at(2))) << 8U)
                              | quint32(quint8(frame.payload.at(3)));
        ui->realtimeValue->setText(QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper());
    } else {
        ui->realtimeValue->setText(QStringLiteral("--"));
    }

    ui->crcValue->setText(crcText);
    ui->crcValue->setStyleSheet(frame.crcOk()
                                    ? QStringLiteral("color:#46d39a;font-size:22px;font-weight:700;")
                                    : QStringLiteral("color:#ff667a;font-size:22px;font-weight:700;"));

    ui->frameTable->insertRow(0);
    const QStringList values = {
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
        deviceText,
        QString::number(frame.sequence),
        frame.messageTypeName(),
        QString::number(frame.timestamp),
        payloadText,
        QStringLiteral("%1 · %2/%3")
            .arg(crcText)
            .arg(frame.receivedCrc, 4, 16, QLatin1Char('0'))
            .arg(frame.calculatedCrc, 4, 16, QLatin1Char('0')).toUpper()
    };

    for (int column = 0; column < values.size(); ++column) {
        auto *item = new QTableWidgetItem(values.at(column));
        if (column == 6)
            item->setForeground(frame.crcOk() ? QColor(QStringLiteral("#46d39a"))
                                              : QColor(QStringLiteral("#ff667a")));
        ui->frameTable->setItem(0, column, item);
    }

    constexpr int maximumRows = 500;
    if (ui->frameTable->rowCount() > maximumRows)
        ui->frameTable->removeRow(ui->frameTable->rowCount() - 1);
}

void MainWindow::appendLog(const QString &message)
{
    ui->rawHexEdit->appendPlainText(
        QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), message));
    ui->rawHexEdit->verticalScrollBar()->setValue(ui->rawHexEdit->verticalScrollBar()->maximum());
}

void MainWindow::updateConnectionCount()
{
    ui->connectionCountLabel->setText(QStringLiteral("在线设备 %1").arg(m_parsers.size()));
}

void MainWindow::setServerStatus(bool listening, const QString &detail)
{
    ui->listenAddressEdit->setEnabled(!listening);
    ui->portSpinBox->setEnabled(!listening);
    ui->startServerButton->setText(listening ? QStringLiteral("停止监听") : QStringLiteral("启动监听"));
    ui->serverStatusLabel->setText(listening
                                       ? QStringLiteral("● 正在监听 %1").arg(detail)
                                       : QStringLiteral("● 服务器未启动"));
    ui->serverStatusLabel->setStyleSheet(listening
        ? QStringLiteral("background:#123b36;color:#4ee0a7;border-radius:16px;padding:7px 15px;font-weight:600;")
        : QStringLiteral("background:#202b3e;color:#91a1b7;border-radius:16px;padding:7px 15px;font-weight:600;"));
}
