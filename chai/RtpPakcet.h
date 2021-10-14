#ifndef CHAI_RTP_PACKET_H
#define CHAI_RTP_PACKET_H

#include <stdint.h>
#include <pc/peer_connection.h>
#include <rtc_base/bit_buffer.h>
#include <modules/video_coding/packet_buffer.h>
#include <modules/rtp_rtcp/source/video_rtp_depacketizer.h>
#include <common_video/include/video_frame_buffer_pool.h>

#include <third_party/libaom/source/libaom/av1/decoder/decoder.h>

#include <json.hpp>

namespace chai {
class PayloadBase {
 public:
  virtual ~PayloadBase() = default;
  virtual nlohmann::json parse(const uint8_t* buff, uint16_t length) = 0;
};

class PayloadFlexFec : public PayloadBase {
 public:
  void insertMediaPacket(webrtc::RtpPacket& rtpPacket);
  void insertFecPacket(webrtc::RtpPacket& fecPacket);
  nlohmann::json parse(const uint8_t* buff, uint16_t length) override;

 protected:
  nlohmann::json parseR0F0(const uint8_t* buff, uint16_t length);
  nlohmann::json parseR0F1(const uint8_t* buff, uint16_t length);
  nlohmann::json parseR1F1(const uint8_t* buff, uint16_t length);

  void setMask(char* const mask, uint16_t pos, uint64_t val, uint16_t len);

 private:
  std::map<uint16_t, webrtc::RtpPacket> packets;
  std::map<uint16_t, webrtc::RtpPacket> fecPackets;
};

class PayloadH264 : public PayloadBase {
 public:
  nlohmann::json parse(const uint8_t* buff, uint16_t length) override;

 protected:
  nlohmann::json parseSps(const uint8_t* buff, uint16_t length);
  nlohmann::json parsePps(const uint8_t* buff, uint16_t length);
  nlohmann::json parseSliceHeader(const uint8_t* buff, uint16_t length);

 private:
  uint32_t separate_colour_plane_flag{0};
  uint32_t frame_mbs_only_flag{0};
  uint32_t pic_order_cnt_type{0};
  uint32_t bottom_field_pic_order_in_frame_present_flag{0};
  uint32_t delta_pic_order_always_zero_flag{0};
  uint32_t redundant_pic_cnt_present_flag{0};
  uint32_t entropy_coding_mode_flag{0};
  uint32_t deblocking_filter_control_present_flag{0};
  uint32_t num_slice_groups_minus1{0};
  uint32_t slice_group_map_type{0};
  uint32_t log2_max_frame_num{0};
  uint32_t log2_max_pic_order_cnt_lsb{0};
};

class PayloadAV1 : public PayloadBase {
 public:
  PayloadAV1();
  nlohmann::json parse(const uint8_t* buff, uint16_t length) override;

 protected:
  nlohmann::json open_bitstream_unit(const uint8_t* buff, uint16_t length);
  uint32_t leb128(const uint8_t* buff, uint64_t& value);
 protected:
  nlohmann::json obu_sequence_header();
  nlohmann::json obu_temporal_delimiter();
  nlohmann::json obu_frame_header();
  nlohmann::json obu_redundant_frame_header();
  nlohmann::json obu_tile_group();
  nlohmann::json obu_metadata();
  nlohmann::json obu_frame();
  nlohmann::json obu_tile_list();
  nlohmann::json obu_padding();

 private:
  std::unique_ptr<aom_codec_ctx_t> context_;
  webrtc::VideoFrameBufferPool buffer_pool_{false, 150};
  std::vector<rtc::CopyOnWriteBuffer> payloads_;
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
  nlohmann::json parseHeader(const webrtc::RtpPacket& rtpPacket);
  nlohmann::json parseExtension(const webrtc::RtpPacket& rtpPacket);
  nlohmann::json parsePayload(const rtc::CopyOnWriteBuffer& payload,
                              uint8_t payloadType);

 private:
  std::unique_ptr<PayloadBase> video_;
  std::unique_ptr<PayloadBase> audio_;

  std::unique_ptr<PayloadFlexFec> flexfec_{new PayloadFlexFec};
};

class RtxPacket : public RtpPacket {
 public:
  nlohmann::json parse(const uint8_t* buff, uint16_t length) override;

 protected:
};

}  // namespace chai

#endif
