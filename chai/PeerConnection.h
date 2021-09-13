#ifndef CHAI_PEERCONNECTION_H
#define CHAI_PEERCONNECTION_H

#include <api/peer_connection_interface.h>  // webrtc::PeerConnectionInterface
#include <pc/peer_connection.h>

#include <future>  // std::promise, std::future
#include <json.hpp>
#include <memory>  // std::unique_ptr

#include "RtpPakcet.h"
//#include "../zx/frame_buffer.h"

namespace chai {
enum Subtype : uint8_t {
  OPUS = 111,
  H264 = 124,
  H264_RTX = 107,
  AV1 = 35,
  AV1_RTX = 36,
  FLEXFEC = 115,
  TEST = 127
};

class PeerConnectionObserver {
 public:
  virtual ~PeerConnectionObserver() = default;
  virtual void onRtpPakcet(nlohmann::json& json) = 0;
  virtual void onRtcpPakcet(nlohmann::json& json) = 0;
};

class PeerConnection {
 public:
  static void Initialize();
  PeerConnection(const webrtc::PeerConnectionInterface::RTCConfiguration&
                     configuration = {},
                 PeerConnectionObserver* observer = nullptr);
  ~PeerConnection();

  void Close();
  webrtc::PeerConnectionInterface::RTCConfiguration GetConfiguration() const;
  bool SetConfiguration(
      const webrtc::PeerConnectionInterface::RTCConfiguration& config);
  std::string CreateOffer(
      const webrtc::PeerConnectionInterface::RTCOfferAnswerOptions& options =
          {});
  std::string CreateAnswer(
      const webrtc::PeerConnectionInterface::RTCOfferAnswerOptions& options);
  void SetLocalDescription(webrtc::SdpType type, const std::string& sdp);
  void SetRemoteDescription(webrtc::SdpType type, const std::string& sdp);
  std::string GetLocalDescription() const;
  std::string GetRemoteDescription() const;
  std::vector<rtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
  GetTransceivers() const;
  rtc::scoped_refptr<webrtc::RtpTransceiverInterface> AddTransceiver(
      cricket::MediaType mediaType);
  rtc::scoped_refptr<webrtc::RtpTransceiverInterface> AddTransceiver(
      rtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
      webrtc::RtpTransceiverInit rtpTransceiverInit);
  std::vector<rtc::scoped_refptr<webrtc::RtpSenderInterface>> GetSenders();
  std::vector<rtc::scoped_refptr<webrtc::RtpReceiverInterface>> GetReceivers();
  bool RemoveTrack(webrtc::RtpSenderInterface* sender);
  nlohmann::json GetStats();
  nlohmann::json GetStats(
      rtc::scoped_refptr<webrtc::RtpSenderInterface> selector);
  nlohmann::json GetStats(
      rtc::scoped_refptr<webrtc::RtpReceiverInterface> selector);
  rtc::scoped_refptr<webrtc::DataChannelInterface> CreateDataChannel(
      const std::string& label,
      const webrtc::DataChannelInit* config);

  void CreateTrack(const cricket::AudioOptions& audio,
                   const cricket::VideoOptions& video);

 protected:
  class PrivateListener : public webrtc::PeerConnectionObserver {
    /* Virtual methods inherited from PeerConnectionObserver. */
   public:
    ~PrivateListener() override = default;
    void OnSignalingChange(
        webrtc::PeerConnectionInterface::SignalingState newState) override;
    void OnAddStream(
        rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override;
    void OnRemoveStream(
        rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override;
    void OnDataChannel(
        rtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel) override;
    void OnRenegotiationNeeded() override;
    void OnIceConnectionChange(
        webrtc::PeerConnectionInterface::IceConnectionState newState) override;
    void OnIceGatheringChange(
        webrtc::PeerConnectionInterface::IceGatheringState newState) override;
    void OnIceCandidate(
        const webrtc::IceCandidateInterface* candidate) override;
    void OnIceCandidatesRemoved(
        const std::vector<cricket::Candidate>& candidates) override;
    void OnIceConnectionReceivingChange(bool receiving) override;
    void OnAddTrack(
        rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
        const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
            streams) override;
    void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface>
                     transceiver) override;
    void OnRemoveTrack(
        rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;
    void OnInterestingUsage(int usagePattern) override;
  };

  class SetSessionDescriptionObserver
      : public webrtc::SetSessionDescriptionObserver {
   public:
    SetSessionDescriptionObserver() = default;
    ~SetSessionDescriptionObserver() override = default;

    std::future<void> GetFuture();
    void Reject(const std::string& error);

    /* Virtual methods inherited from webrtc::SetSessionDescriptionObserver. */
   public:
    void OnSuccess() override;
    void OnFailure(webrtc::RTCError error) override;

   private:
    std::promise<void> promise;
  };

  class CreateSessionDescriptionObserver
      : public webrtc::CreateSessionDescriptionObserver {
   public:
    CreateSessionDescriptionObserver() = default;
    ~CreateSessionDescriptionObserver() override = default;

    std::future<std::string> GetFuture();
    void Reject(const std::string& error);

    /* Virtual methods inherited from webrtc::CreateSessionDescriptionObserver.
     */
   public:
    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override;

   private:
    std::promise<std::string> promise;
  };

  class RTCStatsCollectorCallback : public webrtc::RTCStatsCollectorCallback {
   public:
    RTCStatsCollectorCallback() = default;
    ~RTCStatsCollectorCallback() override = default;

    std::future<nlohmann::json> GetFuture();

    /* Virtual methods inherited from webrtc::RTCStatsCollectorCallback. */
   public:
    void OnStatsDelivered(
        const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report)
        override;

   private:
    std::promise<nlohmann::json> promise;
  };

  // pc/rtp_transport_internal.h
  class RtpTransport : public webrtc::RtpTransportDevelop {
   public:
    RtpTransport(PeerConnectionObserver* observer);
    virtual ~RtpTransport() override = default;

    void SendRtpPacket(rtc::CopyOnWriteBuffer* packet,
                       const rtc::PacketOptions& options,
                       int flags) override;
    void SendRtcpPacket(rtc::CopyOnWriteBuffer* packet,
                        const rtc::PacketOptions& options,
                        int flags) override;
    void OnRtpPacketReceived(rtc::CopyOnWriteBuffer* packet,
                             int64_t packet_time_us) override;
    void OnRtcpPacketReceived(rtc::CopyOnWriteBuffer* packet,
                              int64_t packet_time_us) override;

    void parseRtpPacket(rtc::CopyOnWriteBuffer* packet);

   private:
    // frame_buffer_t frameBuffer;
    RtpPacket rtpPacket;

    rtc::TaskQueue* parseQueue{nullptr};
    PeerConnectionObserver* observer{nullptr};
  };

 private:
  // Signaling and worker threads.
  static std::unique_ptr<rtc::Thread> networkThread;
  static std::unique_ptr<rtc::Thread> signalingThread;
  static std::unique_ptr<rtc::Thread> workerThread;
  // PeerConnection factory.
  static rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
      peerConnectionFactory;

  // PeerConnection instance.
  std::unique_ptr<RtpTransport> rtpTransport{nullptr};
  PeerConnectionObserver* observer{nullptr};
  std::unique_ptr<PrivateListener> privateListener{new PrivateListener};
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> pc{nullptr};
};
}  // namespace chai

#endif
