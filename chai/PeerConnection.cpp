#define MSC_CLASS "PeerConnection"

#include "PeerConnection.h"

#include <absl/memory/memory.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/create_peerconnection_factory.h>
#include <api/rtc_event_log/rtc_event_log_factory.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <common_video/h264/h264_common.h>
#include <common_video/h264/pps_parser.h>
#include <common_video/h264/sps_parser.h>
#include <common_video/h264/sps_vui_rewriter.h>
#include <media/engine/webrtc_media_engine.h>
#include <modules/audio_device/include/audio_device.h>
#include <modules/audio_device/include/audio_device_factory.h>
#include <modules/audio_processing/include/audio_processing.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <modules/rtp_rtcp/source/byte_io.h>
#include <modules/rtp_rtcp/source/video_rtp_depacketizer_h264.h>
#include <modules/video_capture/video_capture.h>
#include <modules/video_capture/video_capture_factory.h>
#include <modules/video_coding/frame_buffer2.h>
#include <modules/video_coding/packet_buffer.h>
#include <modules/video_coding/timing.h>
#include <pc/peer_connection_factory.h>
#include <pc/peer_connection_proxy.h>
#include <pc/peer_connection_wrapper.h>
#include <pc/video_track_source.h>
#include <rtc_base/bit_buffer.h>
#include <rtc_base/ssl_adapter.h>
#include <system_wrappers/include/field_trial.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sdptransform.hpp>

#include "test/vcm_capturer.h"
#include "ScreenCapturer.h"

using json = nlohmann::json;

const std::string videoMid = "1";
const size_t kWidth{1920};
const size_t kHeight{1080};
const size_t kFps{30};

const char kAudioLabel[] = "audio_label";
const char kVideoLabel[] = "video_label";
const char kStreamId[] = "stream_id";
const char* kFlexFecEnabledFieldTrials =
    "WebRTC-FlexFEC-03-Advertised/Enabled/WebRTC-FlexFEC-03/Enabled/";

namespace {
class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static rtc::scoped_refptr<CapturerTrackSource> Create(bool screen = false) {
    if (screen) {
      std::unique_ptr<chai::ScreenCapturer> capturer(
          chai::ScreenCapturer::Create(kWidth, kHeight, kFps));
      if (capturer) {
        return new rtc::RefCountedObject<CapturerTrackSource>(
            std::move(capturer));
      }
    } else {
      DeviceList();

      std::unique_ptr<webrtc::test::VcmCapturer> capturer(
          webrtc::test::VcmCapturer::Create(kWidth, kHeight, kFps, 0));
      if (capturer) {
        return new rtc::RefCountedObject<CapturerTrackSource>(
            std::move(capturer));
      }
    }
    return nullptr;
  }

  static void DeviceList() {
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> device_info(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
    if (!device_info) {
      return;
    }

    for (int i = 0; i < device_info->NumberOfDevices(); ++i) {
      char device_name[256];
      char unique_name[256];
      if (device_info->GetDeviceName(static_cast<uint32_t>(i), device_name,
                                     sizeof(device_name), unique_name,
                                     sizeof(unique_name)) != 0) {
        RTC_LOG(LS_WARNING) << "Failed to GetDeviceName(" << i << ")";
        continue;
      }
      RTC_LOG(LS_INFO) << "GetDeviceName(" << i
                       << "): device_name=" << device_name
                       << ", unique_name=" << unique_name;
    }
  }

 protected:
  explicit CapturerTrackSource(
      std::unique_ptr<webrtc::test::VcmCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}
  explicit CapturerTrackSource(std::unique_ptr<chai::ScreenCapturer> screen)
      : VideoTrackSource(/*remote=*/false),
        screenCapturer_(std::move(screen)) {}
 private:
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    if (this->capturer_) {
      return this->capturer_.get();
    } else {
      return this->screenCapturer_.get();
    }
  }
  std::unique_ptr<webrtc::test::VcmCapturer> capturer_{nullptr};
  std::unique_ptr<chai::ScreenCapturer> screenCapturer_{nullptr};
};
}  // namespace

namespace chai {
/* Static. */
std::unique_ptr<rtc::Thread> PeerConnection::networkThread{nullptr};
std::unique_ptr<rtc::Thread> PeerConnection::signalingThread{nullptr};
std::unique_ptr<rtc::Thread> PeerConnection::workerThread{nullptr};
rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
    PeerConnection::peerConnectionFactory{nullptr};

// clang-format off
// clang-format on

/* Instance methods. */

void PeerConnection::Initialize() {
  if (peerConnectionFactory) {
    return;
  }
  // rtc::LogMessage::LogToDebug(rtc::LS_INFO);
  rtc::LogMessage::LogToDebug(rtc::LS_VERBOSE);
  // rtc::LogMessage::LogTimestamps();
  rtc::LogMessage::LogThreads();

  networkThread = rtc::Thread::CreateWithSocketServer();
  workerThread = rtc::Thread::Create();
  signalingThread = rtc::Thread::Create();

  if (!networkThread->Start() || !signalingThread->Start() ||
      !workerThread->Start()) {
    throw std::string("thread start errored");
  }

  webrtc::field_trial::InitFieldTrialsFromString(kFlexFecEnabledFieldTrials);

  peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
      networkThread.get(), workerThread.get(), signalingThread.get(),
      nullptr /*default_adm*/, webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      webrtc::CreateBuiltinVideoEncoderFactory(),
      webrtc::CreateBuiltinVideoDecoderFactory(), nullptr /*audio_mixer*/,
      nullptr /*audio_processing*/);

  webrtc::PeerConnectionFactoryInterface::Options options;
  options.disable_encryption = false;
  peerConnectionFactory->SetOptions(options);
}

PeerConnection::PeerConnection(
    const webrtc::PeerConnectionInterface::RTCConfiguration&
        configuration /*= {}*/,
    PeerConnectionObserver* observer /* = nullptr*/) {
  webrtc::PeerConnectionInterface::RTCConfiguration config = configuration;

  if (config.sdp_semantics == webrtc::SdpSemantics::kPlanB) {
    // Set SDP semantics to Unified Plan.
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  }

  this->observer = observer;
  // Create the webrtc::Peerconnection.
  this->pc = this->peerConnectionFactory->CreatePeerConnection(
      config, nullptr, nullptr, this->privateListener.get());
}

PeerConnection::~PeerConnection() {
  auto* pci = static_cast<webrtc::PeerConnectionProxyWithInternal<
      webrtc::PeerConnectionInterface>*>(this->pc.get());
  auto* pc = static_cast<webrtc::PeerConnection*>(pci->internal());

  signalingThread->Invoke<void>(RTC_FROM_HERE, [this, pc] {
    auto channel = pc->GetRtpTransport(videoMid);
    if (channel) {
      channel->rtpTransport = nullptr;
    }
  });
}

void PeerConnection::Close() {
  this->pc->Close();
}

webrtc::PeerConnectionInterface::RTCConfiguration
PeerConnection::GetConfiguration() const {
  return this->pc->GetConfiguration();
}

bool PeerConnection::SetConfiguration(
    const webrtc::PeerConnectionInterface::RTCConfiguration& config) {
  webrtc::RTCError error = this->pc->SetConfiguration(config);

  if (error.ok()) {
    return true;
  }

  RTC_LOG(WARNING) << "webrtc::PeerConnection::SetConfiguration failed ["
                   << webrtc::ToString(error.type()) << ":" << error.message()
                   << "]";

  return false;
}

std::string PeerConnection::CreateOffer(
    const webrtc::PeerConnectionInterface::RTCOfferAnswerOptions&
        options /*= {}*/) {
  CreateSessionDescriptionObserver* sessionDescriptionObserver =
      new rtc::RefCountedObject<CreateSessionDescriptionObserver>();

  auto future = sessionDescriptionObserver->GetFuture();

  this->pc->CreateOffer(sessionDescriptionObserver, options);

  return future.get();
}

std::string PeerConnection::CreateAnswer(
    const webrtc::PeerConnectionInterface::RTCOfferAnswerOptions& options) {
  CreateSessionDescriptionObserver* sessionDescriptionObserver =
      new rtc::RefCountedObject<CreateSessionDescriptionObserver>();

  auto future = sessionDescriptionObserver->GetFuture();

  this->pc->CreateAnswer(sessionDescriptionObserver, options);

  return future.get();
}

void PeerConnection::SetLocalDescription(webrtc::SdpType type,
                                         const std::string& sdp) {
  webrtc::SdpParseError error;
  rtc::scoped_refptr<SetSessionDescriptionObserver> observer(
      new rtc::RefCountedObject<SetSessionDescriptionObserver>());

  std::string typeStr = type == webrtc::SdpType::kOffer ? "offer" : "answer";
  auto future = observer->GetFuture();

  auto* sessionDescription =
      webrtc::CreateSessionDescription(typeStr, sdp, &error);
  if (sessionDescription == nullptr) {
    RTC_LOG(WARNING) << "webrtc::CreateSessionDescription failed ["
                     << error.line.c_str()
                     << "]: " << error.description.c_str();

    observer->Reject(error.description);

    return future.get();
  }

  this->pc->SetLocalDescription(observer, sessionDescription);

  return future.get();
}

void PeerConnection::SetRemoteDescription(webrtc::SdpType type,
                                          const std::string& sdp) {
  webrtc::SdpParseError error;
  rtc::scoped_refptr<SetSessionDescriptionObserver> observer(
      new rtc::RefCountedObject<SetSessionDescriptionObserver>());

  std::string typeStr = type == webrtc::SdpType::kOffer ? "offer" : "answer";
  auto future = observer->GetFuture();

  auto* sessionDescription =
      webrtc::CreateSessionDescription(typeStr, sdp, &error);
  if (sessionDescription == nullptr) {
    RTC_LOG(WARNING) << "webrtc::CreateSessionDescription failed ["
                     << error.line.c_str()
                     << "]: " << error.description.c_str();

    observer->Reject(error.description);

    return future.get();
  }

  this->pc->SetRemoteDescription(observer, sessionDescription);
  future.get();

  if (typeStr == "answer") {
    // 新增 rtp回调
    auto* pci = static_cast<webrtc::PeerConnectionProxyWithInternal<
        webrtc::PeerConnectionInterface>*>(this->pc.get());
    auto* pc = static_cast<webrtc::PeerConnection*>(pci->internal());

    signalingThread->Invoke<void>(RTC_FROM_HERE, [this, pc] {
      auto channel = pc->GetRtpTransport(videoMid);
      if (this->observer && channel && !this->rtpTransport) {
        this->rtpTransport.reset(
            new PeerConnection::RtpTransport(this->observer));
        channel->rtpTransport = this->rtpTransport.get();
      }
    });
  }
}

std::string PeerConnection::GetLocalDescription() const {
  auto* desc = this->pc->local_description();
  std::string sdp;

  desc->ToString(&sdp);

  return sdp;
}

std::string PeerConnection::GetRemoteDescription() const {
  auto* desc = this->pc->remote_description();
  std::string sdp;

  desc->ToString(&sdp);

  return sdp;
}

std::vector<rtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
PeerConnection::GetTransceivers() const {
  return this->pc->GetTransceivers();
}

rtc::scoped_refptr<webrtc::RtpTransceiverInterface>
PeerConnection::AddTransceiver(cricket::MediaType mediaType) {
  auto result = this->pc->AddTransceiver(mediaType);

  if (!result.ok()) {
    rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver = nullptr;

    return transceiver;
  }

  return result.value();
}

rtc::scoped_refptr<webrtc::RtpTransceiverInterface>
PeerConnection::AddTransceiver(
    rtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
    webrtc::RtpTransceiverInit rtpTransceiverInit) {
  /*
   * Define a stream id so the generated local description is correct.
   * - with a stream id:    "a=ssrc:<ssrc-id> mslabel:<value>"
   * - without a stream id: "a=ssrc:<ssrc-id> mslabel:"
   *
   * The second is incorrect (https://tools.ietf.org/html/rfc5576#section-4.1)
   */
  rtpTransceiverInit.stream_ids.emplace_back("0");

  auto result = this->pc->AddTransceiver(
      track,
      rtpTransceiverInit);  // NOLINT(performance-unnecessary-value-param)

  if (!result.ok()) {
    rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver = nullptr;

    return transceiver;
  }

  return result.value();
}

std::vector<rtc::scoped_refptr<webrtc::RtpSenderInterface>>
PeerConnection::GetSenders() {
  return this->pc->GetSenders();
}

std::vector<rtc::scoped_refptr<webrtc::RtpReceiverInterface>>
PeerConnection::GetReceivers() {
  return this->pc->GetReceivers();
}

bool PeerConnection::RemoveTrack(webrtc::RtpSenderInterface* sender) {
  return this->pc->RemoveTrack(sender);
}

json PeerConnection::GetStats() {
  rtc::scoped_refptr<RTCStatsCollectorCallback> callback(
      new rtc::RefCountedObject<RTCStatsCollectorCallback>());

  auto future = callback->GetFuture();

  this->pc->GetStats(callback.get());

  return future.get();
}

json PeerConnection::GetStats(
    rtc::scoped_refptr<webrtc::RtpSenderInterface> selector) {
  rtc::scoped_refptr<RTCStatsCollectorCallback> callback(
      new rtc::RefCountedObject<RTCStatsCollectorCallback>());

  auto future = callback->GetFuture();

  this->pc->GetStats(std::move(selector), callback);

  return future.get();
}

json PeerConnection::GetStats(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> selector) {
  rtc::scoped_refptr<RTCStatsCollectorCallback> callback(
      new rtc::RefCountedObject<RTCStatsCollectorCallback>());

  auto future = callback->GetFuture();

  this->pc->GetStats(std::move(selector), callback);

  return future.get();
}

rtc::scoped_refptr<webrtc::DataChannelInterface>
PeerConnection::CreateDataChannel(const std::string& label,
                                  const webrtc::DataChannelInit* config) {
  rtc::scoped_refptr<webrtc::DataChannelInterface> webrtcDataChannel =
      this->pc->CreateDataChannel(label, config);

  if (webrtcDataChannel.get()) {
    RTC_LOG(LS_VERBOSE) << "Success creating data channel";
  } else {
    RTC_LOG(LS_VERBOSE) << "Failed creating data channel";
  }

  return webrtcDataChannel;
}

void PeerConnection::CreateTrack(const cricket::AudioOptions& audio,
                                 const cricket::VideoOptions& video) {
  if (!this->pc->GetSenders().empty()) {
    return;  // Already added tracks.
  }

  // this->AddTransceiver(cricket::MediaType::MEDIA_TYPE_AUDIO);
  // this->AddTransceiver(cricket::MediaType::MEDIA_TYPE_VIDEO);

  // webrtc::DesktopCaptureOptions options =
  // webrtc::DesktopCaptureOptions::CreateDefault();
  // std::unique_ptr<webrtc::DesktopCapturer> screen_capturer(
  //	//webrtc::DesktopCapturer::CreateWindowCapturer(options));
  //	webrtc::DesktopCapturer::CreateScreenCapturer(options));
  // screen_capturer->Start();
  // return;

  {
    auto audio_source = peerConnectionFactory->CreateAudioSource(audio);
    auto audio_track =
        peerConnectionFactory->CreateAudioTrack(kAudioLabel, audio_source);

    webrtc::RtpTransceiverInit init;
    init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
    init.stream_ids.push_back(kStreamId);

    // auto result_or_error = this->pc->AddTrack(audio_track, { kStreamId });
    auto result_or_error = this->pc->AddTransceiver(audio_track, init);
    if (!result_or_error.ok()) {
      RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
                        << result_or_error.error().message();
    }
  }

  {
    rtc::scoped_refptr<CapturerTrackSource> video_device =
        CapturerTrackSource::Create(false);
    if (video_device) {
      rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_(
          peerConnectionFactory->CreateVideoTrack(kVideoLabel, video_device));

      webrtc::RtpTransceiverInit init;
      init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
      {
        init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
        webrtc::RtpEncodingParameters encoding;
        encoding.num_temporal_layers = 3;
        init.stream_ids.push_back(kStreamId);
        init.send_encodings.push_back(encoding);
      }

      auto result_or_error = this->pc->AddTransceiver(video_track_, init);
      if (!result_or_error.ok()) {
        RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
                          << result_or_error.error().message();
      }
    } else {
      RTC_LOG(LS_ERROR) << "OpenVideoCaptureDevice failed";
    }
  }
}

/* SetSessionDescriptionObserver */

std::future<void> PeerConnection::SetSessionDescriptionObserver::GetFuture() {
  return this->promise.get_future();
}

void PeerConnection::SetSessionDescriptionObserver::Reject(
    const std::string& error) {
  this->promise.set_exception(
      std::make_exception_ptr(std::string(error.c_str())));
}

void PeerConnection::SetSessionDescriptionObserver::OnSuccess() {
  this->promise.set_value();
};

void PeerConnection::SetSessionDescriptionObserver::OnFailure(
    webrtc::RTCError error) {
  RTC_LOG(WARNING) << "webtc::SetSessionDescriptionObserver failure ["
                   << webrtc::ToString(error.type()) << ":" << error.message()
                   << "]";

  auto message = std::string(error.message());

  this->Reject(message);
};

/* CreateSessionDescriptionObserver */

std::future<std::string>
PeerConnection::CreateSessionDescriptionObserver::GetFuture() {
  return this->promise.get_future();
}

void PeerConnection::CreateSessionDescriptionObserver::Reject(
    const std::string& error) {
  this->promise.set_exception(
      std::make_exception_ptr(std::string(error.c_str())));
}

void PeerConnection::CreateSessionDescriptionObserver::OnSuccess(
    webrtc::SessionDescriptionInterface* desc) {
  // This callback should take the ownership of |desc|.
  std::unique_ptr<webrtc::SessionDescriptionInterface> ownedDesc(desc);

  std::string sdp;

  ownedDesc->ToString(&sdp);
  this->promise.set_value(sdp);
};

void PeerConnection::CreateSessionDescriptionObserver::OnFailure(
    webrtc::RTCError error) {
  RTC_LOG(WARNING) << "webtc::CreateSessionDescriptionObserver failure ["
                   << webrtc::ToString(error.type()) << ":" << error.message()
                   << "]";

  auto message = std::string(error.message());

  this->Reject(message);
}

/* RTCStatsCollectorCallback */

std::future<json> PeerConnection::RTCStatsCollectorCallback::GetFuture() {
  return this->promise.get_future();
}

void PeerConnection::RTCStatsCollectorCallback::OnStatsDelivered(
    const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
  std::string s = report->ToJson();

  // RtpReceiver stats JSON string is sometimes empty.
  if (s.empty())
    this->promise.set_value(json::array());
  else
    this->promise.set_value(json::parse(s));
};

/* PeerConnection::PrivateListener */

/**
 * Triggered when the SignalingState changed.
 */
void PeerConnection::PrivateListener::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState newState) {
  std::string signalingState;
  switch (newState) {
    case webrtc::PeerConnectionInterface::kStable:
      signalingState = "stable";
      break;
    case webrtc::PeerConnectionInterface::kHaveLocalOffer:
      signalingState = "have-local-offer";
      break;
    case webrtc::PeerConnectionInterface::kHaveLocalPrAnswer:
      signalingState = "have-local-pranswer";
      break;
    case webrtc::PeerConnectionInterface::kHaveRemoteOffer:
      signalingState = "have-remote-offer";
      break;
    case webrtc::PeerConnectionInterface::kHaveRemotePrAnswer:
      signalingState = "have-remote-pranswer";
      break;
    case webrtc::PeerConnectionInterface::kClosed:
      signalingState = "closed";
      break;
  }
  RTC_LOG(LS_VERBOSE) << "[newState:" << signalingState << "]";
}

/**
 * Triggered when media is received on a new stream from remote peer.
 */
void PeerConnection::PrivateListener::OnAddStream(
    rtc::scoped_refptr<webrtc::MediaStreamInterface> /*stream*/) {}

/**
 * Triggered when a remote peer closes a stream.
 */
void PeerConnection::PrivateListener::OnRemoveStream(
    rtc::scoped_refptr<webrtc::MediaStreamInterface> /*stream*/) {}

/**
 * Triggered when a remote peer opens a data channel.
 */
void PeerConnection::PrivateListener::OnDataChannel(
    rtc::scoped_refptr<webrtc::DataChannelInterface> /*dataChannel*/) {}

/**
 * Triggered when renegotiation is needed. For example, an ICE restart has
 * begun.
 */
void PeerConnection::PrivateListener::OnRenegotiationNeeded() {}

/**
 * Triggered any time the IceConnectionState changes.
 *
 * Note that our ICE states lag behind the standard slightly. The most
 * notable differences include the fact that "failed" occurs after 15
 * seconds, not 30, and this actually represents a combination ICE + DTLS
 * state, so it may be "failed" if DTLS fails while ICE succeeds.
 */
void PeerConnection::PrivateListener::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState newState) {
  std::string iceConnectionState;
  switch (newState) {
    case webrtc::PeerConnectionInterface::kIceConnectionNew:
      iceConnectionState = "new";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionChecking:
      iceConnectionState = "checking";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionConnected:
      iceConnectionState = "connected";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
      iceConnectionState = "completed";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionFailed:
      iceConnectionState = "failed";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
      iceConnectionState = "disconnected";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionClosed:
      iceConnectionState = "closed";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionMax:
      iceConnectionState = "max";
      break;
  }
  RTC_LOG(LS_VERBOSE) << "[newState:" << iceConnectionState << "]";
}

/**
 * Triggered any time the IceGatheringState changes.
 */
void PeerConnection::PrivateListener::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState newState) {
  std::string iceGatheringState;
  switch (newState) {
    case webrtc::PeerConnectionInterface::kIceGatheringNew:
      iceGatheringState = "new";
      break;
    case webrtc::PeerConnectionInterface::kIceGatheringGathering:
      iceGatheringState = "gathering";
      break;
    case webrtc::PeerConnectionInterface::kIceGatheringComplete:
      iceGatheringState = "complete";
      break;
  }
  RTC_LOG(LS_VERBOSE) << "[newState:" << iceGatheringState << "]";
}

/**
 * Triggered when a new ICE candidate has been gathered.
 */
void PeerConnection::PrivateListener::OnIceCandidate(
    const webrtc::IceCandidateInterface* candidate) {
  std::string candidateStr;

  candidate->ToString(&candidateStr);

  RTC_LOG(LS_VERBOSE) << "[candidate:" << candidateStr << "]";
}

/**
 * Triggered when the ICE candidates have been removed.
 */
void PeerConnection::PrivateListener::OnIceCandidatesRemoved(
    const std::vector<cricket::Candidate>& /*candidates*/) {}

/**
 * Triggered when the ICE connection receiving status changes.
 */
void PeerConnection::PrivateListener::OnIceConnectionReceivingChange(
    bool /*receiving*/) {}

/**
 * Triggered when a receiver and its track are created.
 *
 * Note: This is called with both Plan B and Unified Plan semantics. Unified
 * Plan users should prefer OnTrack, OnAddTrack is only called as backwards
 * compatibility (and is called in the exact same situations as OnTrack).
 */
void PeerConnection::PrivateListener::OnAddTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<
        rtc::scoped_refptr<webrtc::MediaStreamInterface>>& /*streams*/) {}

/**
 * Triggered when signaling indicates a transceiver will be receiving
 *
 * media from the remote endpoint. This is fired during a call to
 * SetRemoteDescription. The receiving track can be accessed by:
 * transceiver->receiver()->track() and its associated streams by
 * transceiver->receiver()->streams().
 *
 * NOTE: This will only be called if Unified Plan semantics are specified.
 * This behavior is specified in section 2.2.8.2.5 of the "Set the
 * RTCSessionDescription" algorithm:
 *   https://w3c.github.io/webrtc-pc/#set-description
 */
void PeerConnection::PrivateListener::OnTrack(
    rtc::scoped_refptr<webrtc::RtpTransceiverInterface> /*transceiver*/) {}

/**
 * Triggered when signaling indicates that media will no longer be received on a
 * track.
 *
 * With Plan B semantics, the given receiver will have been removed from the
 * PeerConnection and the track muted.
 * With Unified Plan semantics, the receiver will remain but the transceiver
 * will have changed direction to either sendonly or inactive.
 *   https://w3c.github.io/webrtc-pc/#process-remote-track-removal
 */
void PeerConnection::PrivateListener::OnRemoveTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> /*receiver*/) {}

/**
 * Triggered when an interesting usage is detected by WebRTC.
 *
 * An appropriate action is to add information about the context of the
 * PeerConnection and write the event to some kind of "interesting events"
 * log function.
 * The heuristics for defining what constitutes "interesting" are
 * implementation-defined.
 */
void PeerConnection::PrivateListener::OnInterestingUsage(int /*usagePattern*/) {
}

// void complete_frame_call(frame_t* frame, void* data)
//{
//	RTC_LOG(LS_ERROR) << "complete_frame_call " << frame->picture_id << ",
//payload type" << frame->payload_type; 	release_frame(frame);
//}

PeerConnection::RtpTransport::RtpTransport(PeerConnectionObserver* observer)
    : observer(observer) {
  // frame_buffer_option_t option;
  // option.initial_time_us = 0;
  // option.start_buffer_size = 64;
  // option.max_buffer_size = 128;
  // option.call = &complete_frame_call;

  // init_frame_buffer(&this->frameBuffer, &option);
  // append_payload_type(&this->frameBuffer, 115,
  // codec_type_t::kVideoCodecH264); append_payload_type(&this->frameBuffer, 107,
  // codec_type_t::kVideoRtx); append_payload_type(&this->frameBuffer, 115,
  // codec_type_t::kVideoFlexFEC);
}

void PeerConnection::RtpTransport::SendRtpPacket(
    rtc::CopyOnWriteBuffer* packet,
    const rtc::PacketOptions& options,
    int flags) {
  this->parseRtpPacket(packet);
}

void PeerConnection::RtpTransport::SendRtcpPacket(
    rtc::CopyOnWriteBuffer* packet,
    const rtc::PacketOptions& options,
    int flags) {
  // RTC_LOG(LS_VERBOSE) << "SendRtcpPacket:" << packet->size();
}

void PeerConnection::RtpTransport::OnRtpPacketReceived(
    rtc::CopyOnWriteBuffer* packet,
    int64_t packet_time_us) {
  this->parseRtpPacket(packet);
}

void PeerConnection::RtpTransport::OnRtcpPacketReceived(
    rtc::CopyOnWriteBuffer* packet,
    int64_t packet_time_us) {}

void PeerConnection::RtpTransport::parseRtpPacket(
    rtc::CopyOnWriteBuffer* packet) {
  if (parseQueue == nullptr) {
    auto task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
    parseQueue = new rtc::TaskQueue(task_queue_factory->CreateTaskQueue(
        "parsePacket", webrtc::TaskQueueFactory::Priority::LOW));
  }

  std::unique_ptr<uint8_t> buff(new uint8_t[packet->size()]);
  uint32_t len = packet->size();
  memcpy(buff.get(), packet->data(), len);

  uint8_t payloadType = buff.get()[1] & 0x7f;
  switch (payloadType) {
    case Subtype::FLEXFEC:
      payloadType = buff.get()[1] & 0x7f;
      break;
  }

  parseQueue->PostTask([this, buff = std::move(buff), len]() {
    //uint8_t payloadType = buff.get()[1] & 0x7f;
    ////// frame buffer
    //// append_packet(&frameBuffer, buff.get(), len, nullptr);

    //// parse payload
    //nlohmann::json json;
    //switch (payloadType) {
    //  case Subtype::OPUS:  // opus
    //  case Subtype::H264:  // h264
    //  case Subtype::AV1:
    //  case Subtype::FLEXFEC: {
    //    json = this->rtpPacket.parse(buff.get(), len);
    //    break;
    //  } 
    //  case Subtype::H264_RTX:  // rtx
    //  case Subtype::AV1_RTX: {
    //    RtxPacket rtx;
    //    json = rtx.parse(buff.get(), len);
    //    break;
    //  } 
    //  case Subtype::TEST: {
    //    return;
    //  }
    //  default: {
    //    return;
    //  }
    //}
    //this->observer->onRtpPakcet(json);
  });
}

}  // namespace chai
