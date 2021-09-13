#pragma once

#include "chai/PeerConnection.h"

#include <QObject>
#include <QtMultimedia/QAbstractVideoSurface>
#include <QtMultimedia/QVideoSurfaceFormat>

class QmlVideoFrame : public QObject,
                      rtc::VideoSinkInterface<webrtc::VideoFrame>,
                      chai::PeerConnectionObserver {
  Q_OBJECT
  Q_PROPERTY(QAbstractVideoSurface* videoSurface READ videoSurface WRITE
                 setVideoSurface)
  Q_PROPERTY(QString direction READ direction WRITE setDirection)
  Q_PROPERTY(int width READ width WRITE setWidth)
  Q_PROPERTY(int height READ height WRITE setHeight)

 public:
  explicit QmlVideoFrame(QObject* parent = Q_NULLPTR);
  virtual ~QmlVideoFrame();

  QAbstractVideoSurface* videoSurface() const { return _surface; }
  void setVideoSurface(QAbstractVideoSurface* surface);
  QString direction() const { return _direction; }
  void setDirection(const QString& d) { _direction = d; }
  int width() const { return _width; }
  void setWidth(int w) { _width = w; }
  int height() const { return _height; }
  void setHeight(int h) { _height = h; }

 public Q_SLOTS:
  void newVideoContent(const QVideoFrame& frame);
  QString createOffer();
  QString createAnswer();
  void setRemoteDescription(const QString& sdp);
  QString getLocalDescription();
 Q_SIGNALS:
  void newFrameAvailable(const QVideoFrame& frame);
  void message(const QString& type, const QString& msg);

 protected:
  // VideoSinkInterface implementation
  void OnFrame(const webrtc::VideoFrame& frame) override;

  // PeerConnectionObserver
  void onRtpPakcet(nlohmann::json& json) override;
  void onRtcpPakcet(nlohmann::json& json) override;

  void setFormat(QVideoFrame::PixelFormat pixelFormat);

 private:
  QAbstractVideoSurface* _surface{nullptr};
  QVideoSurfaceFormat _format;
  QString _direction;
  int _width{0};
  int _height{0};
  int _audioPayload{0};
  int _videoPayload{0};
  int _rtxPayload{0};
  int _fecPayload{0};

  std::unique_ptr<chai::PeerConnection> _pc{nullptr};
  rtc::scoped_refptr<webrtc::VideoTrackInterface> rendered_track_{nullptr};
};
