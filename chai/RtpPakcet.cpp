#define MSC_CLASS "RtpPacket"

#include "RtpPakcet.h"

#include <api/audio_codecs/opus/audio_decoder_opus.h>
#include <modules/rtp_rtcp/source/byte_io.h>
#include <modules/rtp_rtcp/source/create_video_rtp_depacketizer.h>
#include <modules/rtp_rtcp/source/video_rtp_depacketizer_av1.h>
#include <modules/video_coding/frame_object.h>
#include <api/video/i420_buffer.h>
#include <third_party/libyuv/include/libyuv/convert.h>
//#include <third_party/libaom/source/libaom/aom_util/aom_thread.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <memory>  // std::unique_ptr
#include <sdptransform.hpp>

#include "PayloadAV1.h"

using json = nlohmann::json;

namespace {
const uint16_t kFixedHeaderSize{12};
const uint16_t kOneByteExtensionProfileId{0xBEDE};
const uint16_t kTwoByteExtensionProfileId{0x1000};

std::map<uint8_t, std::string> opusConfig2String = {
    {0, "SILK-only, 8kHz, 10ms"},    {1, "SILK-only, 8kHz, 20ms"},
    {2, "SILK-only, 8kHz, 40ms"},    {3, "SILK-only, 8kHz, 60ms"},
    {4, "SILK-only, 12kHz, 10ms"},   {5, "SILK-only, 12kHz, 20ms"},
    {6, "SILK-only, 12kHz, 40ms"},   {7, "SILK-only, 12kHz, 60ms"},
    {8, "SILK-only, 16kHz, 10ms"},   {9, "SILK-only, 16kHz, 20ms"},
    {10, "SILK-only, 16kHz, 40ms"},  {11, "SILK-only, 16kHz, 60ms"},
    {12, "Hybrid, 24kHz, 10ms"},     {13, "Hybrid, 24kHz, 20ms"},
    {14, "Hybrid, 48kHz, 10ms"},     {15, "Hybrid, 48kHz, 20ms"},
    {16, "CELT-only, 8kHz, 2.5ms"},  {17, "CELT-only, 8kHz, 5ms"},
    {18, "CELT-only, 8kHz, 10ms"},   {19, "CELT-only, 8kHz, 20ms"},
    {20, "CELT-only, 16kHz, 2.5ms"}, {21, "CELT-only, 16kHz, 5ms"},
    {22, "CELT-only, 16kHz, 10ms"},  {23, "CELT-only, 16kHz, 20ms"},
    {24, "CELT-only, 24kHz, 2.5ms"}, {25, "CELT-only, 24kHz, 5ms"},
    {26, "CELT-only, 24kHz, 10ms"},  {27, "CELT-only, 24kHz, 20ms"},
    {28, "CELT-only, 48kHz, 2.5ms"}, {29, "CELT-only, 48kHz, 5ms"},
    {30, "CELT-only, 48kHz, 10ms"},  {31, "CELT-only, 48kHz, 20ms"},
};
}  // namespace

namespace chai {

void XorHeaders(const uint8_t* src_data, uint8_t* dst_data, uint16_t length) {
  // XOR the first 2 bytes of the header: V, P, X, CC, M, PT fields.
  dst_data[0] ^= src_data[0];
  dst_data[1] ^= src_data[1];

  // XOR the length recovery field.
  uint8_t src_payload_length_network_order[2];
  webrtc::ByteWriter<uint16_t>::WriteBigEndian(src_payload_length_network_order,
                                               length);
  dst_data[2] ^= src_payload_length_network_order[0];
  dst_data[3] ^= src_payload_length_network_order[1];

  // XOR the 5th to 8th bytes of the header: the timestamp field.
  dst_data[4] ^= src_data[4];
  dst_data[5] ^= src_data[5];
  dst_data[6] ^= src_data[6];
  dst_data[7] ^= src_data[7];

  // Skip the 9th to 12th bytes of the header.
}

void XorPayloads(const uint8_t* src_data,
                 size_t payload_length,
                 uint8_t* dst_data) {
  for (size_t i = 0; i < payload_length; ++i) {
    dst_data[i] ^= src_data[i];
  }
}

void PayloadFlexFec::insertMediaPacket(webrtc::RtpPacketReceived& rtpPacket) {
  auto it = fecPackets.find(rtpPacket.SequenceNumber());
  if (it == fecPackets.end()) {
    return;
  }

  const uint8_t* buff = rtpPacket.data();
  const uint8_t* fecBuff = it->second.payload().data();

  uint16_t seqBase = webrtc::ByteReader<uint16_t>::ReadBigEndian(fecBuff + 16);
  char mask[128]{0};
  uint16_t fecHeaderLen = {20};
  for (uint8_t i = 0; i < 1; ++i) {
    uint32_t offset{18};
    // 0-14
    uint8_t kKBit = fecBuff[offset] & 0x80;
    uint16_t mask1 =
        webrtc::ByteReader<uint16_t>::ReadBigEndian(fecBuff + offset) & 0x7fff;
    setMask(mask, 0, mask1, 15);
    fecHeaderLen = 20;

    // 15-45
    if (!kKBit) {
      offset += 2;
      kKBit = fecBuff[offset] & 0x80;
      uint32_t mask2 =
          webrtc::ByteReader<uint32_t>::ReadBigEndian(fecBuff + offset) &
          0x7fffffff;
      setMask(mask, 15, mask2, 31);
      fecHeaderLen = 24;
    }

    // 46-108
    if (!kKBit) {
      offset += 4;
      kKBit = fecBuff[offset] & 0x80;
      uint64_t mask3 =
          webrtc::ByteReader<uint64_t>::ReadBigEndian(fecBuff + offset) &
          0x7fffffffffffffff;
      setMask(mask, 46, mask3, 63);
      fecHeaderLen = 32;
    }
  }

  uint16_t seq = seqBase;
  bool isRecoveredXOR = false;
  for (int i = 107; i >= 0; --i) {
    if (mask[i] == '1') {
      if (isRecoveredXOR) {
        seq = seqBase + i + 1;
        break;
      } else {
        isRecoveredXOR = true;
      }
    }
  }

  // XOR
  std::unique_ptr<uint8_t> recovered_buffer(new uint8_t[2048]);
  memcpy(recovered_buffer.get(), fecBuff, 12);
  memcpy(recovered_buffer.get() + 12, fecBuff + fecHeaderLen,
         it->second.payload().size() - fecHeaderLen);

  XorHeaders(buff, recovered_buffer.get(),
             it->second.payload().size() - fecHeaderLen);
  XorPayloads(it->second.data() + 12,
              it->second.payload().size() - fecHeaderLen,
              recovered_buffer.get() + 12);

  recovered_buffer.get()[0] |= 0x80;
  recovered_buffer.get()[0] &= 0xbf;
  webrtc::ByteWriter<uint16_t>::WriteBigEndian(recovered_buffer.get() + 2, seq);
  memcpy(recovered_buffer.get() + 8, buff + 12, 4);
}

void PayloadFlexFec::insertFecPacket(webrtc::RtpPacketReceived& fecPacket) {
  const auto& payload = fecPacket.payload();
  const uint8_t* buff = payload.data();

  uint16_t seqBase = webrtc::ByteReader<uint16_t>::ReadBigEndian(buff + 16);

  uint32_t offset{18};
  char mask[128]{0};
  for (uint8_t i = 0; i < 1; ++i) {
    // 0-14
    uint8_t kKBit = buff[offset] & 0x80;
    uint16_t mask1 =
        webrtc::ByteReader<uint16_t>::ReadBigEndian(buff + offset) & 0x7fff;
    setMask(mask, 0, mask1, 15);

    // 15-45
    if (!kKBit) {
      offset += 2;
      kKBit = buff[offset] & 0x80;
      uint32_t mask2 =
          webrtc::ByteReader<uint32_t>::ReadBigEndian(buff + offset) &
          0x7fffffff;
      setMask(mask, 15, mask2, 31);
    }

    // 46-108
    if (!kKBit) {
      offset += 4;
      kKBit = buff[offset] & 0x80;
      uint64_t mask3 =
          webrtc::ByteReader<uint64_t>::ReadBigEndian(buff + offset) &
          0x7fffffffffffffff;
      setMask(mask, 46, mask3, 63);
    }
  }

  for (int i = 107; i >= 0; --i) {
    if (mask[i] == '1') {
      fecPackets.emplace(seqBase + i + 1, fecPacket);
      break;
    }
  }
}

nlohmann::json PayloadFlexFec::parse(const uint8_t* buff, uint16_t length) {
  /*
                  0                   1                   2                   3
                  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
     1
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |R|F|P|X|  CC   |M| PT recovery |        length recovery        |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |                          TS recovery                          |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |   SSRCCount   |                    reserved                   |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |                             SSRC_i                            |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |           SN base_i           |k|          Mask [0-14]        |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |k|                   Mask [15-45] (optional)                   |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |k|                                                             |
             +-+                   Mask [46-108] (optional)                  |
             |                                                               |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |                     ... next in SSRC_i ...                    |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  */
  uint8_t kRBit = buff[0] & 0x80;
  uint8_t kFBit = buff[0] & 0x40;
  if (!kRBit && !kFBit) {
    return parseR0F0(buff, length);
  } else if (!kRBit && kFBit) {
    return parseR0F1(buff, length);
  } else if (kRBit && kFBit) {
    return parseR1F1(buff, length);
  }

  return nlohmann::json();
}

nlohmann::json PayloadFlexFec::parseR0F0(const uint8_t* buff, uint16_t length) {
  uint32_t offset{18};
  char mask[128]{0};
  mask[15] = 0;
  mask[46] = 0;

  uint16_t fecHeaderLen = {20};
  for (uint8_t i = 0; i < 1; ++i) {
    uint8_t kKBit{0};
    // 0-14
    {
      kKBit = buff[offset] & 0x80;
      uint16_t mask1 =
          webrtc::ByteReader<uint16_t>::ReadBigEndian(buff + offset) & 0x7fff;
      setMask(mask, 0, mask1, 15);
      fecHeaderLen = 20;
    }

    // 15-45
    if (!kKBit) {
      offset += 2;
      kKBit = buff[offset] & 0x80;
      uint32_t mask2 =
          webrtc::ByteReader<uint32_t>::ReadBigEndian(buff + offset) &
          0x7fffffff;
      setMask(mask, 15, mask2, 31);
      fecHeaderLen = 24;
    }

    // 46-108
    if (!kKBit) {
      offset += 4;
      kKBit = buff[offset] & 0x80;
      uint64_t mask3 =
          webrtc::ByteReader<uint64_t>::ReadBigEndian(buff + offset) &
          0x7fffffffffffffff;
      setMask(mask, 46, mask3, 63);
      fecHeaderLen = 32;
    }
  }

  uint16_t seqBase = webrtc::ByteReader<uint16_t>::ReadBigEndian(buff + 16);

  std::string binBuffer;
  uint16_t seq = seqBase;
  for (int i = 107; i >= 0; --i) {
    if (mask[i] == '1') {
      if (!binBuffer.empty()) {
        seq = seqBase + i + 1;
        break;
      }

      auto it = packets.find(seqBase + i + 1);
      if (it == packets.end()) {
        return nlohmann::json();
      }

      std::unique_ptr<uint8_t> recovered_buffer(new uint8_t[2048]);
      memcpy(recovered_buffer.get(), buff, 12);
      memcpy(recovered_buffer.get() + 12, buff + fecHeaderLen,
             length - fecHeaderLen);

      XorHeaders(it->second.data(), recovered_buffer.get(),
                 length - fecHeaderLen);
      XorPayloads(it->second.data() + 12, length - fecHeaderLen,
                  recovered_buffer.get() + 12);

      recovered_buffer.get()[0] |= 0x80;
      recovered_buffer.get()[0] &= 0xbf;
      uint16_t seq = seqBase + i + 1;
      webrtc::ByteWriter<uint16_t>::WriteBigEndian(recovered_buffer.get() + 2,
                                                   seq);
      memcpy(recovered_buffer.get() + 8, buff + 12, 4);

      binBuffer.assign((char*)recovered_buffer.get(),
                       12u + length - fecHeaderLen);
    }
  }
  // char buffer[1024];
  // sprintf(buffer, "fec_packet/%u.packet", seq);
  // std::fstream f(buffer, std::ios::binary | std::ios::out);
  // f << binBuffer;

  return nlohmann::json();

  /*
  		0                   1                   2                   3
  		0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  	   |0|0|P|X|  CC   |M| PT recovery |        length recovery        |
  	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  	   |                          TS recovery                          |
  	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  	   |   SSRCCount   |                    reserved                   |
  	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  	   |                             SSRC_i                            |
  	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  	   |           SN base_i           |k|          Mask [0-14]        |
  	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  	   |k|                   Mask [15-45] (optional)                   |
  	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  	   |k|                                                             |
  	   +-+                   Mask [46-108] (optional)                  |
  	   |                                                               |
  	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  	   |                     ... next in SSRC_i ...                    |
  	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  */
}

nlohmann::json PayloadFlexFec::parseR0F1(const uint8_t* buff, uint16_t length) {
  /*
                  0                   1                   2                   3
                  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
     1
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |0|1|P|X|  CC   |M| PT recovery |        length recovery        |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |                          TS recovery                          |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |   SSRCCount   |                    reserved                   |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |                             SSRC_i                            |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |           SN base_i           |  M (columns)  |    N (rows)   |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

     If M>0, N=0,  行FEC, 后面不会有列FEC
                             因此, FEC = SN, SN+1, SN+2, ... , SN+(M-1), SN+M.

     If M>0, N=1,  行FEC, 列FEC紧随其后.
                                   因此, FEC = SN, SN+1, SN+2, ... , SN+(M-1),
     SN+M. 还有更多

     If M>0, N>1,  表示每M个包的FEC列
                                          在从 SN 基地开始的一组 N 个数据包中.
                                   因此, FEC = SN+(Mx0), SN+(Mx1), ... ,
     SN+(MxN).
  */
  uint8_t padding = (buff[0] & 0x20) >> 5;
  uint8_t extension = (buff[0] & 0x10) >> 4;
  uint8_t csrcCount = buff[0] & 0x1f;
  uint8_t marker = (buff[1] & 0x80) >> 7;
  uint8_t ptRecovery = buff[1] & 0x7f;
  uint16_t lengthRecovery =
      webrtc::ByteReader<uint16_t>::ReadBigEndian(buff + 2);
  uint32_t tsRecovery = webrtc::ByteReader<uint32_t>::ReadBigEndian(buff + 4);
  uint8_t ssrcCount = buff[8];
  uint32_t reserved = webrtc::ByteReader<uint32_t, 3>::ReadBigEndian(buff + 9);

  nlohmann::json header = {
      {"retransmission", 0},
      {"inflexible", 1},
      {"padding", padding},
      {"extension", extension},
      {"csrc_count", csrcCount},
      {"marker", marker},
      {"pt_recovery", ptRecovery},
      {"length_recovery", lengthRecovery},
      {"ts_recovery", tsRecovery},
      {"ssrc_count", ssrcCount},
      {"reserved", reserved},  // 保留字段
      {"ssrc", nlohmann::json::array()},
  };

  uint32_t offset{12};
  for (uint8_t i = 0; i < ssrcCount; ++i) {
    uint32_t ssrc = webrtc::ByteReader<uint32_t>::ReadBigEndian(buff + offset);
    offset += 4;
    uint16_t snBase =
        webrtc::ByteReader<uint16_t>::ReadBigEndian(buff + offset);
    offset += 2;
    uint8_t columns = buff[offset];
    offset += 1;
    uint8_t rows = buff[offset];
    offset += 1;

    nlohmann::json s = {
        {"ssrc", ssrc},
        {"sn_base", snBase},
        {"columns", columns},
        {"rows", rows},
    };
    header["ssrc"].push_back(s);
  }
  header["payload_length"] = length - offset;
  return header;
}

nlohmann::json PayloadFlexFec::parseR1F1(const uint8_t* buff, uint16_t length) {
  /*
                  0                   1                   2                   3
                  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
     1
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |1|1|P|X|   CC  |M| PT recovery |        sequence number        |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |                           timestamp                           |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |                              SSRC                             |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
             |                         Retransmission                        |
             :                            payload                            :
             |                                                               |
             +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  */
  uint8_t padding = (buff[0] & 0x20) >> 5;
  uint8_t extension = (buff[0] & 0x10) >> 4;
  uint8_t csrcCount = buff[0] & 0x1f;
  uint8_t marker = (buff[1] & 0x80) >> 7;
  uint8_t ptRecovery = buff[1] & 0x7f;
  uint16_t seq = webrtc::ByteReader<uint16_t>::ReadBigEndian(buff + 2);
  uint32_t timestamp = webrtc::ByteReader<uint32_t>::ReadBigEndian(buff + 4);
  uint32_t ssrc = webrtc::ByteReader<uint32_t>::ReadBigEndian(buff + 8);

  nlohmann::json header = {
      {"retransmission", 1},
      {"inflexible", 1},
      {"padding", padding},
      {"extension", extension},
      {"csrc_count", csrcCount},
      {"marker", marker},
      {"pt_recovery", ptRecovery},
      {"sequence_number", seq},
      {"timestamp", timestamp},
      {"ssrc", ssrc},
      {"payload_length", length - 12},
  };

  return header;
}

void PayloadFlexFec::setMask(char* const mask,
                          uint16_t offset,
                          uint64_t val,
                          uint16_t len) {
  for (int i = 0; i < len; ++i) {
    if (val & (1ul << (len - 1 - i)))
      mask[offset + i] = '1';
    else
      mask[offset + i] = '0';
  }
}

  /*
   +-----------------------+-----------+-----------+-------------------+
   | Configuration         | Mode      | Bandwidth | Frame Sizes       |
   | Number(s)             |           |           |                   |
   +-----------------------+-----------+-----------+-------------------+
   | 0...3                 | SILK-only | NB        | 10, 20, 40, 60 ms |
   |                       |           |           |                   |
   | 4...7                 | SILK-only | MB        | 10, 20, 40, 60 ms |
   |                       |           |           |                   |
   | 8...11                | SILK-only | WB        | 10, 20, 40, 60 ms |
   |                       |           |           |                   |
   | 12...13               | Hybrid    | SWB       | 10, 20 ms         |
   |                       |           |           |                   |
   | 14...15               | Hybrid    | FB        | 10, 20 ms         |
   |                       |           |           |                   |
   | 16...19               | CELT-only | NB        | 2.5, 5, 10, 20 ms |
   |                       |           |           |                   |
   | 20...23               | CELT-only | WB        | 2.5, 5, 10, 20 ms |
   |                       |           |           |                   |
   | 24...27               | CELT-only | SWB       | 2.5, 5, 10, 20 ms |
   |                       |           |           |                   |
   | 28...31               | CELT-only | FB        | 2.5, 5, 10, 20 ms |
   +-----------------------+-----------+-----------+-------------------+
                                Table 2: TOC Byte Configuration Parameters
*/
/*
   +----------------------+-----------------+-------------------------+
   | Abbreviation         | Audio Bandwidth | Sample Rate (Effective) |
   +----------------------+-----------------+-------------------------+
   | NB (narrowband)      |           4 kHz |                   8 kHz |
   |                      |                 |                         |
   | MB (medium-band)     |           6 kHz |                  12 kHz |
   |                      |                 |                         |
   | WB (wideband)        |           8 kHz |                  16 kHz |
   |                      |                 |                         |
   | SWB (super-wideband) |          12 kHz |                  24 kHz |
   |                      |                 |                         |
   | FB (fullband)        |      20 kHz (*) |                  48 kHz |
   +----------------------+-----------------+-------------------------+
*/
nlohmann::json PayloadOpus::parse(const uint8_t* buff, uint16_t length) {
  std::ostringstream oss;
  uint32_t offset{0};

  // OTC
  uint8_t config = (buff[offset] >> 3) & 0x1f;
  uint16_t packet_flag = buff[offset] & 0x3;
  oss << uint16_t(config) << "(" << opusConfig2String[config] << ")";
  nlohmann::json opus = {
      {"config", oss.str()},
      {"payload_length", length},
      {"packet_flag", packet_flag},
  };
  uint16_t channels = (buff[offset] >> 2) & 0x1;
  oss.str("");
  oss << channels << "(" << channels + 1 << ")";
  opus["channels"] = oss.str();
  switch (packet_flag) {
    case 0:
      opus["frame"] = parseC0(buff, length);
      break;
    case 1:
      opus["frame"] = parseC1(buff, length);
      break;
    case 2:
      opus["frame"] = parseC2(buff, length);
      break;
    case 3:
      opus["frame"] = parseC3(buff, length);
      break;
  }

  // default
  auto format = webrtc::AudioDecoderOpus::SdpToConfig(
      {"opus", 48000, 2, {{"minptime", "10"}, {"useinbandfec", "1"}}});
  if (true) {
    format->sample_rate_hz = 16000;
  }

  return opus;
}
nlohmann::json PayloadOpus::parseC0(const uint8_t* buff, uint16_t length) {
  /*
            0                   1                   2                   3
            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           | config  |s|0|0|                                               |
           +-+-+-+-+-+-+-+-+                                               |
           |                    Compressed frame 1 (N-1 bytes)...          :
           :                                                               |
           |                                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  */
  uint32_t offset{1};
  nlohmann::json frame;
  frame["len"] = length - 1;

  return frame;
}
nlohmann::json PayloadOpus::parseC1(const uint8_t* buff, uint16_t length) {
  /*
            0                   1                   2                   3
            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           | config  |s|0|1|                                               |
           +-+-+-+-+-+-+-+-+                                               :
           |             Compressed frame 1 ((N-1)/2 bytes)...             |
           :                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           |                               |                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               :
           |             Compressed frame 2 ((N-1)/2 bytes)...             |
           :                                               +-+-+-+-+-+-+-+-+
           |                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  */
  uint32_t offset{1};
  nlohmann::json frames = {
      {"packet_num", 2},
      {"frame", nlohmann::json::array()},
  };

  std::list<uint16_t> lens(2, (length - 1) / 2);
  for (auto len : lens) {
    nlohmann::json frame;
    frame["len"] = len;

    // parse opus

    offset += len;
    frames["frame"].push_back(frame);
  }

  return frames;
}

nlohmann::json PayloadOpus::parseC2(const uint8_t* buff, uint16_t length) {
  /*
            0                   1                   2                   3
            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           | config  |s|1|0| N1 (1-2 bytes):                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               :
           |               Compressed frame 1 (N1 bytes)...                |
           :                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           |                               |                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               |
           |                     Compressed frame 2...                     :
           :                                                               |
           |                                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  */
  uint32_t offset{1};
  nlohmann::json frames = {
      {"packet_num", 2},
      {"frame", nlohmann::json::array()},
  };

  std::list<uint16_t> lens;
  for (uint16_t i = 0; i < 2; ++i) {
    if (i == 0) {
      uint16_t len = buff[offset];
      offset += 1;
      if (len > 251) {
        len += buff[offset] * 4;
        offset += 1;
      }
      lens.push_back(len);
    } else {
      lens.push_back(length - offset);
    }
  }
  for (auto len : lens) {
    nlohmann::json frame;
    frame["len"] = len;

    // parse opus

    offset += len;
    frames["frame"].push_back(frame);
  }

  return frames;
}

nlohmann::json PayloadOpus::parseC3(const uint8_t* buff, uint16_t length) {
  /*
                                                            0
                                                            0 1 2 3 4 5 6 7
                                                           +-+-+-+-+-+-+-+-+
                                                           |v|p|     M     |
                                                           +-+-+-+-+-+-+-+-+
                                            Figure 5: The frame count byte
  */
  uint32_t offset{1};
  std::ostringstream oss;
  nlohmann::json frames = {
      {"frame", nlohmann::json::array()},
  };

  uint8_t variable_bitrate = (buff[offset] >> 7) & 0x1;
  uint8_t padding = (buff[offset] >> 6) & 0x1;
  frames["padding"] = padding;
  uint8_t packet_num = buff[offset] & 0x3f;
  frames["packet_num"] = packet_num;
  offset += 1;
  uint32_t padding_length{0};  // 填充字节大小不包含填充字节的第一个字节
  if (padding) {
    nlohmann::json frame;
    bool padding_flag = false;
    do {
      padding_length += buff[offset];
      offset += 1;
      buff[offset] == 255 ? (padding_flag = true) : (padding_flag = false);
    } while (padding_flag);
  }
  oss << padding_length << "(" << padding_length + 1 << ")";
  frames["padding_length"] = oss.str();
  if (variable_bitrate == 0) {
    frames["variable_bitrate"] = "0(CBR)";
    frames["frame"] =
        parseC3CBR(buff + offset, length - 3 - padding_length, packet_num);
  } else {
    frames["variable_bitrate"] = "1(VBR)";
    frames["frame"] =
        parseC3VBR(buff + offset, length - 3 - padding_length, packet_num);
  }
  return frames;
}

nlohmann::json PayloadOpus::parseC3CBR(const uint8_t* buff,
                                       uint16_t length,
                                       uint8_t num) {
  /*
            0                   1                   2                   3
            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           | config  |s|1|1|0|p|     M     |  Padding length (Optional)    :
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           |                                                               |
           :               Compressed frame 1 (R/M bytes)...               :
           |                                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           |                                                               |
           :               Compressed frame 2 (R/M bytes)...               :
           |                                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           |                                                               |
           :                              ...                              :
           |                                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           |                                                               |
           :               Compressed frame M (R/M bytes)...               :
           |                                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           :                  Opus Padding (Optional)...                   |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                                             Figure 6: A CBR Code 3 Packet
  */
  uint32_t offset{0};
  nlohmann::json frame = nlohmann::json::array();

  std::list<uint16_t> lens(num, length / num);
  for (auto len : lens) {
    nlohmann::json frame;
    frame["len"] = len;

    // parse opus

    offset += len;
    frame.push_back(frame);
  }
  return frame;
}

nlohmann::json PayloadOpus::parseC3VBR(const uint8_t* buff,
                                       uint16_t length,
                                       uint8_t num) {
  /*
            0                   1                   2                   3
            0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           | config  |s|1|1|1|p|     M     | Padding length (Optional)     :
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           : N1 (1-2 bytes): N2 (1-2 bytes):     ...       :     N[M-1]    |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           |                                                               |
           :               Compressed frame 1 (N1 bytes)...                :
           |                                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           |                                                               |
           :               Compressed frame 2 (N2 bytes)...                :
           |                                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           |                                                               |
           :                              ...                              :
           |                                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           |                                                               |
           :                     Compressed frame M...                     :
           |                                                               |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
           :                  Opus Padding (Optional)...                   |
           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

                                             Figure 7: A VBR Code 3 Packet
  */
  uint32_t offset{0};
  nlohmann::json frame = nlohmann::json::array();

  // 解析包的长度
  std::list<uint16_t> lens;
  for (uint16_t i = 0; i < num; ++i) {
    uint16_t len = buff[offset];
    offset += 1;
    if (i != num - 1) {
      if (len > 251) {
        len += buff[offset] * 4;
        offset += 1;
      }
    } else {
      len = length - offset;
    }
    lens.push_back(len);
  }
  // 解析frame
  for (auto len : lens) {
    nlohmann::json frame;
    frame["len"] = len;

    // parse opus

    offset += len;
    frame.push_back(frame);
  }
  return frame;
}

nlohmann::json RtpPacket::parse(const uint8_t* buff, uint16_t length) {
  webrtc::RtpPacketReceived rtpPacket;
  if (!rtpPacket.Parse(buff, length)) {
    RTC_LOG(LS_ERROR) << "parse rtp header error";
    return nlohmann::json();
  }

  const uint8_t extension = (buff[0] & 0x10) >> 4;
  nlohmann::json json;
  json["header"] = parseHeader(rtpPacket);
  if (extension) {
    json["extension"] = this->parseExtension(rtpPacket);
  }

  switch (rtpPacket.PayloadType()) {
    case 124:
      if (!video_depacketizer_) {
      }
    case 35:
      if (!video_depacketizer_) {
        video_depacketizer_ = webrtc::CreateVideoRtpDepacketizer(
            webrtc::VideoCodecType::kVideoCodecAV1);
        video_.reset(new PayloadAV1);
      }
      // Jitter Buffer
      json["payload"] = assembleFrame(rtpPacket);

      json["customize"] = {{"color1", video_->color1_},
                           {"color2", video_->color2_}};
      break;
    case 107:
    case 36:
      break;
    case 115:
      break;
    default:
      break;
  }
  return json;
}

//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|X|  CC   |M|     PT      |       sequence number         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           timestamp                           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |           synchronization source (SSRC) identifier            |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// |            Contributing source (CSRC) identifiers             |
// |                             ....                              |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// |  header eXtension profile id  |       length in 32bits        |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                          Extensions                           |
// |                             ....                              |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// |                           Payload                             |
// |             ....              :  padding...                   |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |               padding         | Padding size  |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
nlohmann::json RtpPacket::parseHeader(
    const webrtc::RtpPacketReceived& rtpPacket) {
  const uint8_t extension = (rtpPacket.data()[0] & 0x10) >> 4;
  const uint8_t padding = rtpPacket.padding_size() ? 1 : 0;
  const uint8_t csrcCount = rtpPacket.Csrcs().size();
  nlohmann::json header = {
      {"version", rtpPacket.data()[0] >> 6},
      {"padding", padding},
      {"extension", extension},
      {"csrc_count", rtpPacket.Csrcs().size()},
      {"marker", (uint8_t)rtpPacket.Marker()},
      {"payload_type", rtpPacket.PayloadType()},
      {"sequence_number", rtpPacket.SequenceNumber()},
      {"timestamp", rtpPacket.Timestamp()},
      {"ssrc", rtpPacket.Ssrc()},
  };

  if (csrcCount) {
    nlohmann::json csrcs = nlohmann::json::array();
    for (auto csrc : rtpPacket.Csrcs()) {
      csrcs.push_back(csrc);
    }
    header["csrc"] = csrcs;
  }
  if (padding) {
    // 最后一个字节为对齐的长度
    header["padding_length"] = rtpPacket.padding_size();
  }

  return header;
}

/* RTP header extension, RFC 3550.
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      defined by profile       |           length              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        header extension                       |
|                             ....                              |
*/
nlohmann::json RtpPacket::parseExtension(
    const webrtc::RtpPacketReceived& rtpPacket) {
  uint32_t extensionOffset = kFixedHeaderSize + rtpPacket.Csrcs().size() * 4;
  const uint8_t* extensionEnd = rtpPacket.data() + rtpPacket.headers_size();
  const uint8_t* ptr = rtpPacket.data() + extensionOffset +
                       4;  // 前4个字节为扩展描述以及扩展长度
  std::ostringstream oss;

  uint16_t profile = webrtc::ByteReader<uint16_t>::ReadBigEndian(
      rtpPacket.data() + extensionOffset);
  uint16_t length = webrtc::ByteReader<uint16_t>::ReadBigEndian(
      rtpPacket.data() + extensionOffset + 2);

  oss << length << "(" << length * 4 << ")";
  nlohmann::json headerExtension = {
      {"profile", profile},
      {"length",
       oss.str()},  // 不包含profile length的长度，扩展长度为length*4字节
      {"extension", nlohmann::json::array()},
  };

  if (profile == kOneByteExtensionProfileId) {
    oss.str("");
    oss << "0x" << std::hex << kOneByteExtensionProfileId << "(oneByte)";
    headerExtension["profile"] = oss.str();
    while (ptr < extensionEnd) {
      uint8_t id = (*ptr & 0xF0) >> 4;
      uint8_t len = (*ptr & 0x0F) + 1;  // 不包含id len的长度
      nlohmann::json extension = {{"id", id}};
      // id为15表示停止解析
      if (id == 15u) {
        headerExtension["extension"].push_back(extension);
        break;
      } else if (id != 0u) {
        oss.str("");
        oss << "0x";
        ptr += 1;
        for (uint8_t i = 0; i < len; ++i) {
          oss << std::hex << std::setw(2) << std::setfill('0')
              << uint16_t(ptr[i]);
        }
        extension["length"] = len;
        extension["value"] = oss.str();
        headerExtension["extension"].push_back(extension);

        ptr += len;
      }
      // id=0表示对齐
      else {
        extension["length"] = len;
        headerExtension["extension"].push_back(extension);
        break;
      }
    }
  } else if ((profile & 0xfff0) == kTwoByteExtensionProfileId) {
    oss.str("");
    oss << "0x" << std::hex << kTwoByteExtensionProfileId << "(twoByte)";
    headerExtension["profile"] = oss.str();
  }

  return headerExtension;
}

nlohmann::json RtpPacket::assembleFrame(
    const webrtc::RtpPacketReceived& rtpPacket) {
  webrtc::VideoRtpDepacketizerAv1 av1;
  auto parsed = av1.Parse(rtpPacket.PayloadBuffer());

  auto packet = std::make_unique<webrtc::video_coding::PacketBuffer::Packet>(rtpPacket, parsed->video_header);

  webrtc::RTPVideoHeader& video_header = packet->video_header;
  video_header.is_last_packet_in_frame = rtpPacket.Marker();
  packet->video_payload = rtpPacket.PayloadBuffer();

  packet_infos_.emplace(
      rtpPacket.SequenceNumber(),
      webrtc::RtpPacketInfo(
          rtpPacket.Ssrc(), rtpPacket.Csrcs(), rtpPacket.Timestamp(),
          absl::nullopt,
          rtpPacket.GetExtension<webrtc::AbsoluteCaptureTimeExtension>(), 0));

  auto result = packet_buffer_.InsertPacket(std::move(packet));
  if (result.packets.size() == 0) {
    return nlohmann::json();
  }

  // frame buffer
  webrtc::video_coding::PacketBuffer::Packet* first_packet{nullptr};
  int max_nack_count{0};
  int64_t min_recv_time{0};
  int64_t max_recv_time{0};
  std::vector<rtc::ArrayView<const uint8_t>> payloads;
  webrtc::RtpPacketInfos::vector_type packet_infos;
  
  for (auto& packet : result.packets) {
    webrtc::RtpPacketInfo& packet_info = packet_infos_[packet->seq_num];
    if (packet->is_first_packet_in_frame()) {
      first_packet = packet.get();
      max_nack_count = packet->times_nacked;
      min_recv_time = packet_info.receive_time().ms();
      max_recv_time = packet_info.receive_time().ms();
      payloads.clear();
      packet_infos.clear();
    } else {
      max_nack_count = std::max(max_nack_count, packet->times_nacked);
      min_recv_time = std::min(min_recv_time, packet_info.receive_time().ms());
      max_recv_time = std::max(max_recv_time, packet_info.receive_time().ms());
    }
    payloads.emplace_back(packet->video_payload);
    packet_infos.push_back(packet_info);

    if (packet->is_last_packet_in_frame()) {
      auto bitstream = video_depacketizer_->AssembleFrame(payloads);
      if (!bitstream) {
        // Failed to assemble a frame. Discard and continue.
        continue;
      }

      const webrtc::video_coding::PacketBuffer::Packet& last_packet = *packet;
      auto frame = std::make_unique<webrtc::RtpFrameObject>(
          first_packet->seq_num,                             //
          last_packet.seq_num,                               //
          last_packet.marker_bit,                            //
          max_nack_count,                                    //
          min_recv_time,                                     //
          max_recv_time,                                     //
          first_packet->timestamp,                           //
          first_packet->timestamp,  //
          last_packet.video_header.video_timing,             //
          first_packet->payload_type,                        //
          first_packet->codec(),                             //
          last_packet.video_header.rotation,                 //
          last_packet.video_header.content_type,             //
          first_packet->video_header,                        //
          last_packet.video_header.color_space,              //
          webrtc::RtpPacketInfos(std::move(packet_infos)),           //
          std::move(bitstream));

      
      auto frames = reference_finder_.ManageFrame(std::move(frame));
      for (auto& f : frames) {
        return video_->parse(f->data(), f->size());
      }
    }
  }
  if (result.buffer_cleared) {
    packet_infos_.clear();
  }
  return nlohmann::json();
}

nlohmann::json RtxPacket::parse(const uint8_t* buff, uint16_t length) {
  /*
          0                   1                   2                   3
          0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |                         RTP Header                            |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |            OSN                |                               |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               |
     |                  Original RTP Packet Payload                  |
     |                                                               |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  */
  webrtc::RtpPacketReceived rtpPacket;
  if (!rtpPacket.Parse(buff, length)) {
    RTC_LOG(LS_ERROR) << "parse rtp header error";
    return nlohmann::json();
  }

  const uint8_t extension = (buff[0] & 0x10) >> 4;
  nlohmann::json json;
  json["header"] = parseHeader(rtpPacket);
  if (extension) {
    json["extension"] = this->parseExtension(rtpPacket);
  }

  auto& payload = rtpPacket.PayloadBuffer();
  if (rtpPacket.payload_size()) {
    json["osn"] = webrtc::ByteReader<uint16_t>::ReadBigEndian(payload.data());
  }
  return json;
}
}  // namespace chai
