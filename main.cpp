#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include "QmlVideoFrame.h"
#include "QmlWebSocket.h"

int main(int argc, char* argv[]) {
#if defined(Q_OS_WIN)
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

  QGuiApplication app(argc, argv);

  qmlRegisterType<QmlWebSocket>("Qml.WebSocket", 1, 0, "QmlWebSocket");
  qmlRegisterType<QmlVideoFrame>("Qml.VideoFrame", 1, 0, "QmlVideoFrame");

  QQmlApplicationEngine engine;
  engine.load(QUrl(QStringLiteral("qrc:/main.qml")));
  if (engine.rootObjects().isEmpty())
    return -1;

  return app.exec();
}
