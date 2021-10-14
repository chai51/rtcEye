#include "QmlVideoFrame.h"

#include <iostream>

#include <api/video/i420_buffer.h>
#include <sdptransform.hpp>

QmlVideoFrame::QmlVideoFrame(QObject* parent /*= Q_NULLPTR*/) {
  chai::PeerConnection::Initialize();

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.enable_dtls_srtp = true;
  config.type = webrtc::PeerConnectionInterface::IceTransportsType::kAll;
  config.bundle_policy = webrtc::PeerConnectionInterface::BundlePolicy::kBundlePolicyMaxBundle;
  config.rtcp_mux_policy = webrtc::PeerConnectionInterface::RtcpMuxPolicy::kRtcpMuxPolicyRequire;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  _pc.reset(new chai::PeerConnection(config, this));

  QObject::connect(this, &QmlVideoFrame::newFrameAvailable, this, &QmlVideoFrame::newVideoContent);
}

QmlVideoFrame::~QmlVideoFrame() {
  rendered_track_->RemoveSink(this);
}

void QmlVideoFrame::setVideoSurface(QAbstractVideoSurface* surface) {
  if (_surface && _surface != surface && _surface->isActive()) {
    _surface->stop();
  }

  _surface = surface;

  if (_surface && _format.isValid()) {
    _format = _surface->nearestFormat(_format);
    _surface->start(_format);
  }
}

void QmlVideoFrame::newVideoContent(const QVideoFrame& frame) {
  if (_surface)
    _surface->present(frame);
}

QString QmlVideoFrame::createOffer() {
  auto audioOptions = cricket::AudioOptions();
  //audioOptions.echo_cancellation = false;
  this->_pc->CreateTrack(audioOptions, cricket::VideoOptions());
  
  std::string offer = this->_pc->CreateOffer();
  this->_pc->SetLocalDescription(webrtc::SdpType::kOffer, offer);
  
  this->setFormat(QVideoFrame::PixelFormat::Format_YUV420P);
  
  auto senders = this->_pc->GetSenders();
  for (auto sender : senders) {
    auto* track = sender->track().release();
    if (!track)
      continue;
    if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
      auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
      rendered_track_ = video_track;
  
      rendered_track_->AddOrUpdateSink(this, rtc::VideoSinkWants());
    }
    track->Release();
  }
  return QString::fromStdString(offer);
}

QString QmlVideoFrame::createAnswer() {
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  options.offer_to_receive_audio = true;
  options.offer_to_receive_video = true;
  std::string answer = this->_pc->CreateAnswer(options);
  this->_pc->SetLocalDescription(webrtc::SdpType::kAnswer, answer);
  this->setFormat(QVideoFrame::PixelFormat::Format_YUV420P);

  auto receivers = this->_pc->GetReceivers();
  for (auto receiver : receivers) {
    auto* track = receiver->track().release();
    if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
      auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
      rendered_track_ = video_track;

      rendered_track_->AddOrUpdateSink(this, rtc::VideoSinkWants());
    }
    track->Release();
  }
  return QString::fromStdString(answer);
}

void QmlVideoFrame::setRemoteDescription(const QString& sdp) {
  if (this->_direction == "send") {
    this->_pc->SetRemoteDescription(webrtc::SdpType::kAnswer, sdp.toStdString());
  } else {
    this->_pc->SetRemoteDescription(webrtc::SdpType::kOffer, sdp.toStdString());
  }
}

QString QmlVideoFrame::getLocalDescription() {
  return QString::fromStdString(this->_pc->GetLocalDescription());
}

void QmlVideoFrame::OnFrame(const webrtc::VideoFrame& video_frame) {
  rtc::scoped_refptr<webrtc::I420BufferInterface> buffer(
      video_frame.video_frame_buffer()->ToI420());
  if (video_frame.rotation() != webrtc::kVideoRotation_0) {
    buffer = webrtc::I420Buffer::Rotate(*buffer, video_frame.rotation());
  }

  buffer = buffer->Scale(this->_width, this->_height)->ToI420();

  int size = _width * _height * 3 / 2;
  {
    QVideoFrame frame(size, QSize(_width, _height), _width, this->_format.pixelFormat());
    if (frame.map(QAbstractVideoBuffer::WriteOnly)) {
      memcpy(frame.bits(), buffer->DataY(), size);
      frame.setStartTime(0);
      frame.unmap();
      Q_EMIT this->newFrameAvailable(frame);
    }
  }
}

void QmlVideoFrame::onRtpPakcet(nlohmann::json& json) {
  auto j = json.dump();
  uint8_t payloadType = json["header"]["payload_type"].get<uint8_t>();

  nlohmann::json customize;

  switch (payloadType) {
    case chai::Subtype::OPUS:
      customize["subtype"] = "audio";
      break;
    case chai::Subtype::H264:
    case chai::Subtype::AV1:
      customize["subtype"] = "video";
      break;
    case chai::Subtype::H264_RTX:
    case chai::Subtype::AV1_RTX:
      customize["subtype"] = "rtx";
      break;
    case chai::Subtype::FLEXFEC:
      customize["subtype"] = "fec";
      break;
    default:
      break;
  }

  if (json["payload"].is_object() &&
      json["payload"]["frame_key"].get<uint8_t>() == 1) {
    customize["keyframe"] = true;
  }

  json["customize"] = customize;
  Q_EMIT this->message("rtp", QString::fromStdString(json.dump()));
}

void QmlVideoFrame::onRtcpPakcet(nlohmann::json& json) {
  Q_EMIT this->message("rtcp", QString::fromStdString(json.dump()));
}

void QmlVideoFrame::setFormat(QVideoFrame::PixelFormat pixelFormat) {
  QSize size(this->_width, this->_height);
  this->_format = QVideoSurfaceFormat(size, pixelFormat);

  if (_surface) {
    if (_surface->isActive()) {
      _surface->stop();
    }
    _format = _surface->nearestFormat(_format);
    _surface->start(_format);
  }
}
