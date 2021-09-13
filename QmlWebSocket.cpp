#include "QmlWebSocket.h"

#include <QtNetwork/QSslPreSharedKeyAuthenticator>

QmlWebSocket::QmlWebSocket(QObject* parent /*= Q_NULLPTR*/) {
  void (QWebSocket::*p)(QAbstractSocket::SocketError) = &QWebSocket::error;

  QObject::connect(&this->_websocket, &QWebSocket::connected, this,
                   &QmlWebSocket::connected);
  QObject::connect(&this->_websocket, &QWebSocket::disconnected, this,
                   &QmlWebSocket::disconnected);
  QObject::connect(&this->_websocket, &QWebSocket::stateChanged, this,
                   &QmlWebSocket::stateChanged);
  QObject::connect(&this->_websocket, &QWebSocket::readChannelFinished, this,
                   &QmlWebSocket::readChannelFinished);
  QObject::connect(&this->_websocket, &QWebSocket::textFrameReceived, this,
                   &QmlWebSocket::textFrameReceived);
  QObject::connect(&this->_websocket, &QWebSocket::binaryFrameReceived, this,
                   &QmlWebSocket::binaryFrameReceived);
  QObject::connect(&this->_websocket, &QWebSocket::textMessageReceived, this,
                   &QmlWebSocket::textMessageReceived);
  QObject::connect(&this->_websocket, &QWebSocket::binaryMessageReceived, this,
                   &QmlWebSocket::binaryMessageReceived);
  QObject::connect(&this->_websocket, p, this, &QmlWebSocket::error2);
  QObject::connect(&this->_websocket, &QWebSocket::pong, this,
                   &QmlWebSocket::pong);
  QObject::connect(&this->_websocket, &QWebSocket::bytesWritten, this,
                   &QmlWebSocket::bytesWritten);
  QObject::connect(&this->_websocket, &QWebSocket::sslErrors, this,
                   &QmlWebSocket::sslErrors);
  QObject::connect(&this->_websocket,
                   &QWebSocket::preSharedKeyAuthenticationRequired, this,
                   &QmlWebSocket::preSharedKeyAuthenticationRequired);
}

QmlWebSocket::~QmlWebSocket() {}

void QmlWebSocket::connect(const QString& url) {
  if (!QSslSocket::supportsSsl()) {
    Q_EMIT this->error("not find ssl so");
    return;
  }

  if (connect_) {
    return;
  }
  this->connect_ = true;

  QNetworkRequest request;
  request.setUrl(url);
  QByteArray byteHeader = "Sec-WebSocket-Protocol";
  QByteArray protocol = "protoo";
  request.setRawHeader(byteHeader, protocol);
  this->_websocket.open(request);
}

void QmlWebSocket::send(const QString& msg) {
  this->_websocket.sendTextMessage(msg);
}

void QmlWebSocket::connected() {
  this->connect_ = true;
  Q_EMIT this->open();
}

void QmlWebSocket::disconnected() {
  this->connect_ = false;
  this->_websocket.close();
  Q_EMIT this->close();
}

void QmlWebSocket::stateChanged(QAbstractSocket::SocketState state) {
  switch (state) {
    case QAbstractSocket::UnconnectedState:
      break;
    case QAbstractSocket::HostLookupState:
      break;
    case QAbstractSocket::ConnectingState:
      /*RTC_LOG(LS_VERBOSE) << "ws connecting";*/
      break;
    case QAbstractSocket::ConnectedState:
      break;
    case QAbstractSocket::BoundState:
      break;
    case QAbstractSocket::ListeningState:
      break;
    case QAbstractSocket::ClosingState:
      break;
    default:
      break;
  }
}

void QmlWebSocket::readChannelFinished() {}

void QmlWebSocket::textFrameReceived(const QString& frame, bool isLastFrame) {}

void QmlWebSocket::binaryFrameReceived(const QByteArray& frame,
                                       bool isLastFrame) {}

void QmlWebSocket::textMessageReceived(const QString& msg) {
  Q_EMIT this->message(msg);
}

void QmlWebSocket::binaryMessageReceived(const QByteArray& message) {}

void QmlWebSocket::error2(QAbstractSocket::SocketError error) {
  Q_EMIT this->error(this->_websocket.errorString());
}

void QmlWebSocket::pong(quint64 elapsedTime, const QByteArray& payload) {}

void QmlWebSocket::bytesWritten(qint64 bytes) {}

void QmlWebSocket::sslErrors(const QList<QSslError>& errors) {}

void QmlWebSocket::preSharedKeyAuthenticationRequired(
    QSslPreSharedKeyAuthenticator* authenticator) {}
