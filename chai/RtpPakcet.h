#ifndef CHAI_RTP_PACKET_H
#define CHAI_RTP_PACKET_H

#include <stdint.h>
#include <pc/peer_connection.h>
#include <rtc_base/bit_buffer.h>
#include <modules/video_coding/packet_buffer.h>
#include <modules/rtp_rtcp/source/video_rtp_depacketizer.h>
#include <common_video/include/video_frame_buffer_pool.h>
#include <third_party/libaom/source/libaom/av1/decoder/decoder.h>
#include <modules/video_coding/rtp_frame_reference_finder.h>

#include <json.hpp>

namespace chai {
#define AUDIO_COLOR "#000000"
#define WHITE_COLOR "#FFFFFF"
#define VIDEO_KEY_COLOR "#00008B"
#define VIDEO_L0T0_COLOR "#0000FF"
#define VIDEO_L0T1_COLOR "#87CEEB"
#define VIDEO_L0T2_COLOR "#ADD8E6"
#define FEC_COLOR "#DCDCDC"
#define RTX_COLOR "#C0C0C0"

class PayloadBase {
 public:
  virtual ~PayloadBase() = default;
  virtual nlohmann::json parse(const uint8_t* buff, uint16_t length) = 0;

 public:
  int frame_type_{0};
  std::string color1_{WHITE_COLOR};
  std::string color2_{WHITE_COLOR};
};

class PayloadFlexFec : public PayloadBase {
 public:
  void insertMediaPacket(webrtc::RtpPacketReceived& rtpPacket);
  void insertFecPacket(webrtc::RtpPacketReceived& fecPacket);
  nlohmann::json parse(const uint8_t* buff, uint16_t length) override;

 protected:
  nlohmann::json parseR0F0(const uint8_t* buff, uint16_t length);
  nlohmann::json parseR0F1(const uint8_t* buff, uint16_t length);
  nlohmann::json parseR1F1(const uint8_t* buff, uint16_t length);

  void setMask(char* const mask, uint16_t pos, uint64_t val, uint16_t len);

 private:
  std::map<uint16_t, webrtc::RtpPacketReceived> packets;
  std::map<uint16_t, webrtc::RtpPacketReceived> fecPackets;
};


class PayloadOpus : public PayloadBase {
 public:
  nlohmann::json parse(const uint8_t* buff, uint16_t length) override;

 protected:
  nlohmann::json parseC0(const uint8_t* buff, uint16_t length);
  nlohmann::json parseC1(const uint8_t* buff, uint16_t length);
  nlohmann::json parseC2(const uint8_t* buff, uint16_t length);
  nlohmann::json parseC3(const uint8_t* buff, uint16_t length);
  nlohmann::json parseC3CBR(const uint8_t* buff, uint16_t length, uint8_t num);
  nlohmann::json parseC3VBR(const uint8_t* buff, uint16_t length, uint8_t num);
};

class RtpPacket {
 public:
  virtual ~RtpPacket() = default;
  virtual nlohmann::json parse(const uint8_t* buff, uint16_t length);

 protected:
  nlohmann::json parseHeader(const webrtc::RtpPacketReceived& rtpPacket);
  nlohmann::json parseExtension(const webrtc::RtpPacketReceived& rtpPacket);

  nlohmann::json assembleFrame(const webrtc::RtpPacketReceived& rtpPacket);

 private:
  std::unique_ptr<PayloadBase> video_;
  std::unique_ptr<PayloadBase> audio_;

std::map<int64_t, webrtc::RtpPacketInfo> packet_infos_;
  webrtc::video_coding::PacketBuffer packet_buffer_{512, 2048};
  std::unique_ptr<webrtc::VideoRtpDepacketizer> video_depacketizer_;
  webrtc::RtpFrameReferenceFinder reference_finder_;

  std::unique_ptr<PayloadFlexFec> flexfec_{new PayloadFlexFec};
};

class RtxPacket : public RtpPacket {
 public:
  nlohmann::json parse(const uint8_t* buff, uint16_t length) override;

 protected:
};

}  // namespace chai

#endif
