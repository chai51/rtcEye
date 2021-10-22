/*
 * @Author: chaiyukuan
 * @LastEditors: chaiyukuan
 */
#pragma once
#include <QtWebSockets/QWebSocket>

QT_FORWARD_DECLARE_CLASS(QSslPreSharedKeyAuthenticator)

class QmlWebSocket : public QObject {
  Q_OBJECT
 public:
  explicit QmlWebSocket(QObject* parent = Q_NULLPTR);
  virtual ~QmlWebSocket();
 public Q_SLOTS:
  void connect(const QString& url);
  void send(const QString& msg);

  void connected();
  void disconnected();
  void stateChanged(QAbstractSocket::SocketState state);
  void readChannelFinished();
  void textFrameReceived(const QString& frame, bool isLastFrame);
  void binaryFrameReceived(const QByteArray& frame, bool isLastFrame);
  void textMessageReceived(const QString& message);
  void binaryMessageReceived(const QByteArray& message);
  void error2(QAbstractSocket::SocketError error);
  void pong(quint64 elapsedTime, const QByteArray& payload);
  void bytesWritten(qint64 bytes);
  void sslErrors(const QList<QSslError>& errors);
  void preSharedKeyAuthenticationRequired(
      QSslPreSharedKeyAuthenticator* authenticator);
 Q_SIGNALS:
  void close();
  void error(const QString& errorMessage);
  void message(const QString& msg);
  void open();

 private:
  QWebSocket _websocket;
  bool connect_{false};
};
