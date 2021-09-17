#define MSC_CLASS "RtpPacket"

#include "RtpPakcet.h"

#include <api/audio_codecs/opus/audio_decoder_opus.h>
#include <common_video/h264/h264_common.h>
#include <modules/rtp_rtcp/source/byte_io.h>
#include <modules/rtp_rtcp/source/video_rtp_depacketizer_av1.h>
#include <api/video/i420_buffer.h>
#include <third_party/libyuv/include/libyuv/convert.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <memory>  // std::unique_ptr
#include <sdptransform.hpp>

using json = nlohmann::json;

namespace {
const uint16_t kFixedHeaderSize{12};
const uint16_t kOneByteExtensionProfileId{0xBEDE};
const uint16_t kTwoByteExtensionProfileId{0x1000};

const uint16_t kNalHeaderSize{1};
const uint16_t kLengthFieldSize{2};
const uint16_t kStapAHeaderSize = kNalHeaderSize + kLengthFieldSize;
// Bit masks for FU (A and B) indicators.
enum NalDefs : uint8_t { kFBit = 0x80, kNriMask = 0x60, kTypeMask = 0x1F };
// Bit masks for FU (A and B) headers.
enum FuDefs : uint8_t { kSBit = 0x80, kEBit = 0x40, kRBit = 0x20 };

std::map<OBU_TYPE, std::string> obuType2String = {
    {OBU_TYPE::OBU_SEQUENCE_HEADER, "sequence header"},
    {OBU_TYPE::OBU_TEMPORAL_DELIMITER, "temporal delimiter"},
    {OBU_TYPE::OBU_FRAME_HEADER, "frame header"},
    {OBU_TYPE::OBU_TILE_GROUP, "tile group"},
    {OBU_TYPE::OBU_METADATA, "metadata"},
    {OBU_TYPE::OBU_FRAME, "frame"},
    {OBU_TYPE::OBU_REDUNDANT_FRAME_HEADER, "redundant frame header"},
    {OBU_TYPE::OBU_TILE_LIST, "tile_list"},
    {OBU_TYPE::OBU_PADDING, "padding"},
};

std::map<uint8_t, std::string> nalType2String = {
    {webrtc::H264::NaluType::kSlice, "slice"},
    {webrtc::H264::NaluType::kIdr, "idr"},
    {webrtc::H264::NaluType::kSei, "sei"},
    {webrtc::H264::NaluType::kSps, "sps"},
    {webrtc::H264::NaluType::kPps, "pps"},
    {webrtc::H264::NaluType::kAud, "aud"},
    {webrtc::H264::NaluType::kEndOfSequence, "end of sequence"},
    {webrtc::H264::NaluType::kEndOfStream, "end of stream"},
    {webrtc::H264::NaluType::kFiller, "filler"},
    {webrtc::H264::NaluType::kStapA, "stap A"},
    {webrtc::H264::NaluType::kFuA, "fu A"},
};

std::map<uint8_t, std::string> profileIdc2String = {
    {66, "Baseline"}, {77, "Main"},        {88, "Extended"},    {100, "High"},
    {110, "High 10"}, {122, "High 4:2:2"}, {144, "High 4:4:4"},
};
std::map<uint8_t, std::string> chromaFormatIdc2String = {
    {0, "4:0:0"},
    {1, "4:2:0"},
    {2, "4:2:2"},
    {3, "4:4:4"},
};
std::map<uint8_t, std::string> sliceType2String = {
    {webrtc::H264::SliceType::kP, "P slice"},
    {webrtc::H264::SliceType::kB, "B slice"},
    {webrtc::H264::SliceType::kI, "I slice"},
    {webrtc::H264::SliceType::kSp, "SP slice"},
    {webrtc::H264::SliceType::kSi, "SI slice"},
};
std::map<uint8_t, std::string> entropyCodingMode2String = {
    {0, "CAVLC"},
    {1, "CABAC"},
};

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

void PayloadFlexFec::insertMediaPacket(webrtc::RtpPacket& rtpPacket) {
  {
    char buffer[1024];
    sprintf(buffer, "rtp_packet/%u.packet", rtpPacket.SequenceNumber());
    std::fstream f(buffer, std::ios::binary | std::ios::out);
    f << std::string((char*)rtpPacket.data(), rtpPacket.size());
  }

  // packets.emplace(rtpPacket.SequenceNumber(), rtpPacket);

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

  //{
  //	char buffer[1024];
  //	sprintf(buffer, "fec_packet/%u.packet", rtpPacket.SequenceNumber());
  //	std::fstream f(buffer, std::ios::binary | std::ios::out);
  //	f << std::string((char*)recovered_buffer.get(), 12u +
  //it->second.payload().size() - fecHeaderLen);
  //}
}

void PayloadFlexFec::insertFecPacket(webrtc::RtpPacket& fecPacket) {
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

  ///*
  //		0                   1                   2                   3
  //		0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  //	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //	   |0|0|P|X|  CC   |M| PT recovery |        length recovery        |
  //	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //	   |                          TS recovery                          |
  //	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //	   |   SSRCCount   |                    reserved                   |
  //	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //	   |                             SSRC_i                            |
  //	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //	   |           SN base_i           |k|          Mask [0-14]        |
  //	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //	   |k|                   Mask [15-45] (optional)                   |
  //	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //	   |k|                                                             |
  //	   +-+                   Mask [46-108] (optional)                  |
  //	   |                                                               |
  //	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //	   |                     ... next in SSRC_i ...                    |
  //	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  //*/
  // uint8_t padding = (buff[0] & 0x20) >> 5;
  // uint8_t extension = (buff[0] & 0x10) >> 4;
  // uint8_t csrcCount = buff[0] & 0x1f;
  // uint8_t marker = (buff[1] & 0x80) >> 7;
  // uint8_t ptRecovery = buff[1] & 0x7f;
  // uint16_t lengthRecovery = webrtc::ByteReader<uint16_t>::ReadBigEndian(buff
  // + 2); uint32_t tsRecovery = webrtc::ByteReader<uint32_t>::ReadBigEndian(buff
  // + 4); uint8_t ssrcCount = buff[8]; uint32_t reserved =
  // webrtc::ByteReader<uint32_t, 3>::ReadBigEndian(buff + 9);

  // nlohmann::json header =
  //	{
  //		{"retransmission", 0},
  //		{"inflexible", 0},
  //		{"padding", padding},
  //		{"extension", extension},
  //		{"csrc_count", csrcCount},
  //		{"marker", marker},
  //		{"pt_recovery", ptRecovery},
  //		{"length_recovery", lengthRecovery},
  //		{"ts_recovery", tsRecovery},
  //		{"ssrc_count", ssrcCount},
  //		{"reserved", reserved}, // 保留字段
  //		{"ssrc", nlohmann::json::array()},
  //	};

  // uint32_t offset{ 12 };
  // for (uint8_t i = 0; i < ssrcCount; ++i)
  //{
  //	std::ostringstream oss;
  //	uint32_t ssrc = webrtc::ByteReader<uint32_t>::ReadBigEndian(buff +
  //offset); 	offset += 4; 	uint16_t snBase =
  //webrtc::ByteReader<uint16_t>::ReadBigEndian(buff + offset); 	offset += 2;
  //	nlohmann::json s =
  //	{
  //		{"ssrc", ssrc},
  //		{"sn_base", snBase},
  //	};

  //	// 0-14
  //	uint8_t kKBit = buff[offset] & 0x80;
  //	uint16_t mask1 = webrtc::ByteReader<uint16_t>::ReadBigEndian(buff +
  //offset) & 0x7fff; 	offset += 2; 	s["mask1"] =
  //this->tobit<15>(mask1).to_string();

  //	// 15-45
  //	if (!kKBit)
  //	{
  //		kKBit = buff[offset] & 0x80;
  //		uint32_t mask2 = webrtc::ByteReader<uint32_t>::ReadBigEndian(buff +
  //offset) & 0x7fffffff; 		offset += 4; 		s["mask2"] =
  //this->tobit<31>(mask2).to_string();
  //	}

  //	// 46-108
  //	if (!kKBit)
  //	{
  //		kKBit = buff[offset] & 0x80;
  //		uint64_t mask3 = webrtc::ByteReader<uint64_t>::ReadBigEndian(buff +
  //offset) & 0x7fffffffffffffff; 		offset += 8; 		s["mask3"] =
  //this->tobit<63>(mask3).to_string();
  //	}

  //	header["ssrc"].push_back(s);
  //}
  // header["payload_length"] = length - offset;
  // return header;
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

nlohmann::json PayloadH264::parse(const uint8_t* buff, uint16_t length) {
  const uint8_t* ptr = buff;
  uint16_t offset{0};
  std::ostringstream oss;

  uint8_t forbidden_zero_bit = (ptr[0] & kFBit) >> 7;
  uint8_t nal_ref_idc = (ptr[0] & kNriMask) >> 5;
  uint8_t nal_type = ptr[0] & kTypeMask;
  oss << uint16_t(nal_type) << "(" << nalType2String[nal_type] << ")";
  nlohmann::json nalus = {
      {"forbidden_zero_bit", forbidden_zero_bit},
      {"nal_ref_idc", nal_ref_idc},
      {"nalu_type", oss.str()},
      {"payload_length", length},
      {"video_codec", "h264"},
      {"frame_key", 0},
  };
  // header
  if (nal_type == webrtc::H264::NaluType::kFuA) {
    /*
             0                   1                   2                   3
             0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            | FU indicator  |   FU header   |                               |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               |
            |                                                               |
            |                         FU payload                            |
            |                                                               |
            |                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |                               :...OPTIONAL RTP padding        |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

       The FU indicator octet has the following format:
               +---------------+
               |0|1|2|3|4|5|6|7|
               +-+-+-+-+-+-+-+-+
               |F|NRI|  Type   |
               +---------------+

       The FU header has the following format:
              +---------------+
              |0|1|2|3|4|5|6|7|
              +-+-+-+-+-+-+-+-+
              |S|E|R|  Type   |
              +---------------+
    */
    nalus["nalus"] = nlohmann::json::array();
    offset += kNalHeaderSize;
    uint8_t first_fragment = (ptr[offset] & kSBit) >> 7;
    uint8_t last_fragment = (ptr[offset] & kEBit) >> 6;
    uint8_t reserved = (ptr[offset] & kRBit) >> 5;
    nal_type = ptr[offset] & kTypeMask;
    uint16_t len = length - offset;
    oss.str("");
    oss << uint16_t(nal_type) << "(" << nalType2String[nal_type] << ")";
    nlohmann::json nalu = {
        {"first_fragment", first_fragment},
        {"last_fragment", last_fragment},
        {"reserved", reserved},
        {"nalu_type", oss.str()},
        {"nalu_length", len},
    };
    if (first_fragment) {
      offset += kNalHeaderSize;
      nalu["slice_header"] = this->parseSliceHeader(ptr + offset, len - 1);
    }

    if (nal_type == webrtc::H264::kIdr) {
      nalus["frame_key"] = 1;
    }
    nalus["nalus"].push_back(nalu);
  } else if (nal_type == webrtc::H264::NaluType::kStapA) {
    /*
             0                   1                   2                   3
             0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |                          RTP Header                           |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |STAP-A NAL HDR |         NALU 1 Size           | NALU 1 HDR    |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |                         NALU 1 Data                           |
            :                                                               :
            +               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |               | NALU 2 Size                   | NALU 2 HDR    |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |                         NALU 2 Data                           |
            :                                                               :
            |                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |                               :...OPTIONAL RTP padding        |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    */
    nalus["nalus"] = nlohmann::json::array();
    offset += kNalHeaderSize;
    while (offset < length) {
      uint16_t len = webrtc::ByteReader<uint16_t>::ReadBigEndian(ptr + offset);
      offset += kLengthFieldSize;
      forbidden_zero_bit = (ptr[offset] & kFBit) >> 7;
      nal_ref_idc = (ptr[offset] & kNriMask) >> 5;
      nal_type = ptr[offset] & kTypeMask;
      oss.str("");
      oss << uint16_t(nal_type) << "(" << nalType2String[nal_type] << ")";
      nlohmann::json nalu = {
          {"forbidden_zero_bit", forbidden_zero_bit},
          {"nal_ref_idc", nal_ref_idc},
          {"nalu_type", oss.str()},
          {"nalu_length", len},
      };

      uint16_t offset2 = offset + kNalHeaderSize;
      switch (nal_type) {
        case webrtc::H264::kSlice: {
          nalu["slice_header"] = this->parseSliceHeader(ptr + offset2, len - 1);
        } break;
        case webrtc::H264::kIdr:
          nalus["frame_key"] = 1;
          break;
        case webrtc::H264::kSei:
          break;
        case webrtc::H264::kSps: {
          nalu["sps"] = this->parseSps(ptr + offset2, len - 1);
        } break;
        case webrtc::H264::kPps: {
          nalu["pps"] = this->parsePps(ptr + offset2, len - 1);
        } break;
        case webrtc::H264::kAud:
          break;
        case webrtc::H264::kEndOfSequence:
          break;
        case webrtc::H264::kEndOfStream:
          break;
        case webrtc::H264::kFiller:
          break;
        case webrtc::H264::kStapA:
          break;
        case webrtc::H264::kFuA:
          break;
        default:
          break;
      }
      nalus["nalus"].push_back(nalu);
      offset += len;
    }
  } else {
    nalus["nalu_length"] = length;
  }
  return nalus;
}

// https://www.itu.int/rec/T-REC-H.264 T-REC-H.264-201402-S 7.3.2.1.1
nlohmann::json PayloadH264::parseSps(const uint8_t* buff, uint16_t length) {
  this->separate_colour_plane_flag = 0;
  this->frame_mbs_only_flag = 0;
  this->pic_order_cnt_type = 0;
  this->delta_pic_order_always_zero_flag = 0;
  this->log2_max_frame_num = 0;
  this->log2_max_pic_order_cnt_lsb = 0;

  // framerate = sps->vui.vui_time_scale / sps->vui.vui_num_units_in_tick / 2;
  std::unique_ptr<rtc::BitBuffer> buffer(new rtc::BitBuffer(buff, length));

  // profile_idc和level_idc指示编码视频序列符合的配置文件和级别
  uint8_t profile_idc{0};
  // 编码约束
  uint32_t constraint_set0_flag{0};
  uint32_t constraint_set1_flag{0};
  uint32_t constraint_set2_flag{0};
  uint32_t constraint_set3_flag{0};
  uint32_t constraint_set4_flag{0};
  uint32_t constraint_set5_flag{0};
  uint32_t reserved_zero_2bit{0};

  uint8_t level_idc{0};
  uint32_t seq_parameter_set_id{0};
  uint32_t chroma_format_idc{1};

  // 等于1表示4:4:4色度格式的三个颜色分量分别编码
  uint32_t separate_colour_plane_flag{0};

  // 亮度样本的位深
  uint32_t bit_depth_luma_minus8{0};
  // 色度样本的位深
  uint32_t bit_depth_chroma_minus8{0};

  uint32_t qpprime_y_zero_transform_bypass_flag{0};
  uint32_t seq_scaling_matrix_present_flag{0};
  uint32_t seq_scaling_list_present_flag{0};

  // 一个gop中最大帧的数量 2的(log2_max_frame_num_minus4+4)次方
  uint32_t log2_max_frame_num_minus4{0};

  // 计算显示帧顺序的
  uint32_t pic_order_cnt_type{0};
  uint32_t log2_max_pic_order_cnt_lsb_minus4{0};
  uint32_t delta_pic_order_always_zero_flag{0};
  uint32_t offset_for_non_ref_pic{0};
  uint32_t offset_for_top_to_bottom_field{0};
  uint32_t num_ref_frames_in_pic_order_cnt_cycle{0};
  uint32_t offset_for_ref_frame{0};

  // 最大参考帧数
  uint32_t max_num_ref_frames{0};
  // 不重要的参数
  uint32_t gaps_in_frame_num_value_allowed_flag{0};

  // 宽=(pic_width_in_mbs_minus1 + 1) * 16
  uint32_t pic_width_in_mbs_minus1{0};
  // 高=(pic_height_in_map_units_minus1 + 1) * (2 - frame_mbs_only_flag) * 16
  uint32_t pic_height_in_map_units_minus1{0};

  // 0场编码 1帧编码(场是隔行扫描，产生两张图片)
  uint32_t frame_mbs_only_flag{0};

  uint32_t mb_adaptive_frame_field_flag{0};
  uint32_t direct_8x8_inference_flag{0};
  // 视频裁剪
  uint32_t frame_cropping_flag{0};
  uint32_t frame_crop_left_offset{0};
  uint32_t frame_crop_right_offset{0};
  uint32_t frame_crop_top_offset{0};
  uint32_t frame_crop_bottom_offset{0};

  uint32_t vui_parameters_present_flag{0};

  std::ostringstream oss;
  nlohmann::json psp;

  buffer->ReadUInt8(&profile_idc);  // u(8)
  oss << uint16_t(profile_idc) << "(" << profileIdc2String[profile_idc] << ")";
  psp["profile_idc"] = oss.str();
  buffer->ReadBits(&constraint_set0_flag, 1);  // u(1)
  psp["constraint_set0_flag"] = constraint_set0_flag;
  buffer->ReadBits(&constraint_set1_flag, 1);  // u(1)
  psp["constraint_set1_flag"] = constraint_set1_flag;
  buffer->ReadBits(&constraint_set2_flag, 1);  // u(1)
  psp["constraint_set2_flag"] = constraint_set2_flag;
  buffer->ReadBits(&constraint_set3_flag, 1);  // u(1)
  psp["constraint_set3_flag"] = constraint_set3_flag;
  buffer->ReadBits(&constraint_set4_flag, 1);  // u(1)
  psp["constraint_set4_flag"] = constraint_set4_flag;
  buffer->ReadBits(&constraint_set5_flag, 1);  // u(1)
  psp["constraint_set5_flag"] = constraint_set5_flag;
  buffer->ReadBits(&reserved_zero_2bit, 2);  // u(2)
  psp["reserved_zero_2bit"] = reserved_zero_2bit;
  buffer->ReadUInt8(&level_idc);  // u(8)
  psp["level_idc"] = level_idc;
  buffer->ReadExponentialGolomb(&seq_parameter_set_id);  // ue(v)
  psp["seq_parameter_set_id"] = seq_parameter_set_id;
  if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
      profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
      profile_idc == 86 || profile_idc == 118 || profile_idc == 128 ||
      profile_idc == 138 || profile_idc == 139 || profile_idc == 134) {
    buffer->ReadExponentialGolomb(&chroma_format_idc);  // ue(v)
    oss.str("");
    oss << chroma_format_idc << "(" << chromaFormatIdc2String[chroma_format_idc]
        << ")";
    psp["chroma_format_idc"] = oss.str();
    if (chroma_format_idc == 3) {
      buffer->ReadBits(&separate_colour_plane_flag, 1);  // u(1)
      psp["separate_colour_plane_flag"] = separate_colour_plane_flag;
    }
    buffer->ReadExponentialGolomb(&bit_depth_luma_minus8);  // ue(v)
    oss.str("");
    oss << bit_depth_luma_minus8 << "(" << 8 + bit_depth_luma_minus8 << ")";
    psp["bit_depth_luma_minus8"] = oss.str();
    buffer->ReadExponentialGolomb(&bit_depth_chroma_minus8);  // ue(v)
    oss.str("");
    oss << bit_depth_chroma_minus8 << "(" << 8 + bit_depth_chroma_minus8 << ")";
    psp["bit_depth_chroma_minus8"] = oss.str();
    buffer->ReadBits(&qpprime_y_zero_transform_bypass_flag, 1);  // u(1)
    psp["qpprime_y_zero_transform_bypass_flag"] =
        qpprime_y_zero_transform_bypass_flag;
    buffer->ReadBits(&seq_scaling_matrix_present_flag, 1);  // u(1)
    psp["seq_scaling_matrix_present_flag"] = seq_scaling_matrix_present_flag;
    if (seq_scaling_matrix_present_flag) {
      psp["seq_scaling_list_present_flag"] = nlohmann::json::array();
      uint32_t scaling_list_count = (chroma_format_idc != 3 ? 8 : 12);
      for (uint32_t i = 0; i < scaling_list_count; ++i) {
        buffer->ReadBits(&seq_scaling_list_present_flag, 1);  // u(1)
        psp["seq_scaling_list_present_flag"].push_back(
            seq_scaling_list_present_flag);
      }
    }
  }
  buffer->ReadExponentialGolomb(&log2_max_frame_num_minus4);  // ue(v)
  oss.str("");
  oss << log2_max_frame_num_minus4 << "("
      << std::pow(2, log2_max_frame_num_minus4 + 4) << ")";
  psp["log2_max_frame_num_minus4"] = oss.str();
  buffer->ReadExponentialGolomb(&pic_order_cnt_type);  // ue(v)
  psp["pic_order_cnt_type"] = pic_order_cnt_type;
  if (pic_order_cnt_type == 0) {
    buffer->ReadExponentialGolomb(&log2_max_pic_order_cnt_lsb_minus4);  // ue(v)
    oss.str("");
    oss << log2_max_pic_order_cnt_lsb_minus4 << "("
        << std::pow(2, log2_max_pic_order_cnt_lsb_minus4 + 4) << ")";
    psp["log2_max_pic_order_cnt_lsb_minus4"] = oss.str();
  } else if (pic_order_cnt_type == 1) {
    buffer->ReadBits(&delta_pic_order_always_zero_flag, 1);  // u(1)
    psp["delta_pic_order_always_zero_flag"] = delta_pic_order_always_zero_flag;
    buffer->ReadExponentialGolomb(&offset_for_non_ref_pic);  // se(v)
    psp["offset_for_non_ref_pic"] = offset_for_non_ref_pic;
    buffer->ReadExponentialGolomb(&offset_for_top_to_bottom_field);  // se(v)
    psp["offset_for_top_to_bottom_field"] = offset_for_top_to_bottom_field;
    buffer->ReadExponentialGolomb(
        &num_ref_frames_in_pic_order_cnt_cycle);  // ue(v)
    psp["num_ref_frames_in_pic_order_cnt_cycle"] =
        num_ref_frames_in_pic_order_cnt_cycle;
    psp["offset_for_ref_frame"] = nlohmann::json::array();
    for (uint32_t i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; ++i) {
      buffer->ReadExponentialGolomb(&offset_for_ref_frame);  // se(v)
      psp["offset_for_ref_frame"].push_back(offset_for_ref_frame);
    }
  }
  buffer->ReadExponentialGolomb(&max_num_ref_frames);  // ue(v)
  psp["max_num_ref_frames"] = max_num_ref_frames;
  buffer->ReadBits(&gaps_in_frame_num_value_allowed_flag, 1);  // u(1)
  psp["max_num_regaps_in_frame_num_value_allowed_flagf_frames"] =
      gaps_in_frame_num_value_allowed_flag;
  buffer->ReadExponentialGolomb(&pic_width_in_mbs_minus1);  // ue(v)
  oss.str("");
  oss << pic_width_in_mbs_minus1 << "(" << (pic_width_in_mbs_minus1 + 1) * 16
      << ")";
  psp["pic_width_in_mbs_minus1"] = oss.str();
  buffer->ReadExponentialGolomb(&pic_height_in_map_units_minus1);  // ue(v)
  buffer->ReadBits(&frame_mbs_only_flag, 1);                       // u(1)
  oss.str("");
  oss << pic_height_in_map_units_minus1 << "("
      << (pic_height_in_map_units_minus1 + 1) * (2 - frame_mbs_only_flag) * 16
      << ")";
  psp["pic_height_in_map_units_minus1"] = pic_height_in_map_units_minus1;
  psp["frame_mbs_only_flag"] = frame_mbs_only_flag;
  if (!frame_mbs_only_flag) {
    buffer->ReadBits(&mb_adaptive_frame_field_flag, 1);  // u(1)
    psp["mb_adaptive_frame_field_flag"] = mb_adaptive_frame_field_flag;
  }
  buffer->ReadBits(&direct_8x8_inference_flag, 1);  // u(1)
  psp["direct_8x8_inference_flag"] = direct_8x8_inference_flag;
  buffer->ReadBits(&frame_cropping_flag, 1);  // u(1)
  psp["frame_cropping_flag"] = frame_cropping_flag;
  if (frame_cropping_flag) {
    buffer->ReadExponentialGolomb(&frame_crop_left_offset);  // ue(v)
    psp["frame_crop_left_offset"] = frame_crop_left_offset;
    buffer->ReadExponentialGolomb(&frame_crop_right_offset);  // ue(v)
    psp["frame_crop_right_offset"] = frame_crop_right_offset;
    buffer->ReadExponentialGolomb(&frame_crop_top_offset);  // ue(v)
    psp["frame_crop_top_offset"] = frame_crop_top_offset;
    buffer->ReadExponentialGolomb(&frame_crop_bottom_offset);  // ue(v)
    psp["frame_crop_bottom_offset"] = frame_crop_bottom_offset;
  }
  buffer->ReadBits(&vui_parameters_present_flag, 1);  // u(1)
  psp["vui_parameters_present_flag"] = vui_parameters_present_flag;
  if (vui_parameters_present_flag) {
  }

  this->separate_colour_plane_flag = separate_colour_plane_flag;
  this->frame_mbs_only_flag = frame_mbs_only_flag;
  this->pic_order_cnt_type = pic_order_cnt_type;
  this->delta_pic_order_always_zero_flag = delta_pic_order_always_zero_flag;
  this->log2_max_frame_num = log2_max_frame_num_minus4 + 4;
  this->log2_max_pic_order_cnt_lsb = log2_max_pic_order_cnt_lsb_minus4 + 4;
  return psp;
}

// https://www.itu.int/rec/T-REC-H.264 T-REC-H.264-201402-S 7.3.2.2
nlohmann::json PayloadH264::parsePps(const uint8_t* buff, uint16_t length) {
  this->bottom_field_pic_order_in_frame_present_flag = 0;
  this->redundant_pic_cnt_present_flag = 0;
  this->entropy_coding_mode_flag = 0;
  this->deblocking_filter_control_present_flag = 0;
  this->num_slice_groups_minus1 = 0;
  this->slice_group_map_type = 0;
  std::unique_ptr<rtc::BitBuffer> buffer(new rtc::BitBuffer(buff, length));

  uint32_t pic_parameter_set_id{0};
  uint32_t seq_parameter_set_id{0};
  // 熵编码模式
  uint32_t entropy_coding_mode_flag{0};

  // 场编码相关信息标志
  uint32_t bottom_field_pic_order_in_frame_present_flag{0};
  // 一帧中的slice个数
  uint32_t num_slice_groups_minus1{0};

  uint32_t slice_group_map_type{0};
  uint32_t run_length_minus1{0};
  uint32_t top_left{0};
  uint32_t bottom_right{0};
  uint32_t slice_group_change_direction_flag{0};
  uint32_t slice_group_change_rate_minus1{0};
  uint32_t pic_size_in_map_units_minus1{0};
  uint32_t slice_group_id{0};
  uint32_t num_ref_idx_l0_default_active_minus1{0};
  uint32_t num_ref_idx_l1_default_active_minus1{0};
  // 在P/SP slice中是否开启权重预测
  uint32_t weighted_pred_flag{0};
  // 在B slice中加权预测的方法id
  uint32_t weighted_bipred_idc{0};
  // 初始化量化参数，实际参数在slice header中
  uint32_t pic_init_qp_minus26{0};
  uint32_t pic_init_qs_minus26{0};
  // 用于计算色度分量的量化参数
  uint32_t chroma_qp_index_offset{0};
  // 表示slice header中是否存在用于去块滤波器控制的信息
  uint32_t deblocking_filter_control_present_flag{0};
  // 表示I宏块在进行帧内预测时只能使用来自I和SI类型的宏块信息
  uint32_t constrained_intra_pred_flag{0};
  // 用于表示slice header中是否存在redundant_pic_cnt语法元素
  uint32_t redundant_pic_cnt_present_flag{0};

  uint32_t transform_8x8_mode_flag{0};
  uint32_t pic_scaling_matrix_present_flag{0};
  uint32_t pic_scaling_list_present_flag{0};
  uint32_t second_chroma_qp_index_offset{0};

  std::ostringstream oss;
  nlohmann::json pps;

  buffer->ReadExponentialGolomb(&pic_parameter_set_id);  // ue(v)
  pps["pic_parameter_set_id"] = pic_parameter_set_id;
  buffer->ReadExponentialGolomb(&seq_parameter_set_id);  // ue(v)
  pps["seq_parameter_set_id"] = seq_parameter_set_id;
  buffer->ReadBits(&entropy_coding_mode_flag, 1);  // u(1)
  oss << entropy_coding_mode_flag << "("
      << entropyCodingMode2String[entropy_coding_mode_flag] << ")";
  pps["entropy_coding_mode_flag"] = oss.str();
  buffer->ReadBits(&bottom_field_pic_order_in_frame_present_flag, 1);  // u(1)
  pps["bottom_field_pic_order_in_frame_present_flag"] =
      bottom_field_pic_order_in_frame_present_flag;
  buffer->ReadExponentialGolomb(&num_slice_groups_minus1);  // ue(v)
  oss.str("");
  oss << num_slice_groups_minus1 << "(" << num_slice_groups_minus1 + 1 << ")";
  pps["num_slice_groups_minus1"] = oss.str();
  if (num_slice_groups_minus1 > 0) {
    buffer->ReadExponentialGolomb(&slice_group_map_type);  // ue(v)
    pps["slice_group_map_type"] = slice_group_map_type;
    if (slice_group_map_type == 0) {
      pps["run_length_minus1"] = nlohmann::json::array();
      for (uint32_t i = 0; i <= num_slice_groups_minus1; ++i) {
        buffer->ReadExponentialGolomb(&run_length_minus1);  // ue(v)
        pps["run_length_minus1"].push_back(run_length_minus1);
      }
    } else if (slice_group_map_type == 2) {
      pps["top_left_bottom_right"] = nlohmann::json::array();
      for (uint32_t i = 0; i <= num_slice_groups_minus1; ++i) {
        nlohmann::json top_left_bottom_right;
        buffer->ReadExponentialGolomb(&top_left);  // ue(v)
        top_left_bottom_right["top_left"] = top_left;
        buffer->ReadExponentialGolomb(&bottom_right);  // ue(v)
        top_left_bottom_right["bottom_right"] = bottom_right;
        pps["top_left_bottom_right"].push_back(top_left_bottom_right);
      }
    } else if (slice_group_map_type == 3 || slice_group_map_type == 4 ||
               slice_group_map_type == 5) {
      buffer->ReadBits(&slice_group_change_direction_flag, 1);  // u(1)
      pps["slice_group_change_direction_flag"] =
          slice_group_change_direction_flag;
      buffer->ReadExponentialGolomb(&slice_group_change_rate_minus1);  // ue(v)
      pps["slice_group_change_rate_minus1"] = slice_group_change_rate_minus1;
    } else if (slice_group_map_type == 6) {
      buffer->ReadExponentialGolomb(&pic_size_in_map_units_minus1);  // ue(v)
      pps["pic_size_in_map_units_minus1"] = pic_size_in_map_units_minus1;

      uint32_t slice_group_id_bits = 0;
      uint32_t num_slice_groups = num_slice_groups_minus1 + 1;
      if ((num_slice_groups & (num_slice_groups - 1)) != 0) {
        ++slice_group_id_bits;
      }
      while (num_slice_groups > 0) {
        num_slice_groups >>= 1;
        ++slice_group_id_bits;
      }
      pps["slice_group_id"] = nlohmann::json::array();
      for (uint32_t i = 0; i <= pic_size_in_map_units_minus1; ++i) {
        buffer->ReadBits(&slice_group_id, slice_group_id_bits);  // u(v)
        pps["slice_group_id"].push_back(slice_group_id);
      }
    }
  }
  buffer->ReadExponentialGolomb(
      &num_ref_idx_l0_default_active_minus1);  // ue(v)
  pps["num_ref_idx_l0_default_active_minus1"] =
      num_ref_idx_l0_default_active_minus1;
  buffer->ReadExponentialGolomb(
      &num_ref_idx_l1_default_active_minus1);  // ue(v)
  pps["num_ref_idx_l1_default_active_minus1"] =
      num_ref_idx_l1_default_active_minus1;
  buffer->ReadBits(&weighted_pred_flag, 1);  // u(1)
  pps["weighted_pred_flag"] = weighted_pred_flag;
  buffer->ReadBits(&weighted_bipred_idc, 2);  // u(2)
  pps["weighted_bipred_idc"] = weighted_bipred_idc;
  buffer->ReadExponentialGolomb(&pic_init_qp_minus26);  // se(v)
  pps["pic_init_qp_minus26"] = pic_init_qp_minus26;
  buffer->ReadExponentialGolomb(&pic_init_qs_minus26);  // se(v)
  pps["pic_init_qs_minus26"] = pic_init_qs_minus26;
  buffer->ReadExponentialGolomb(&chroma_qp_index_offset);  // se(v)
  pps["chroma_qp_index_offset"] = chroma_qp_index_offset;
  buffer->ReadBits(&deblocking_filter_control_present_flag, 1);  // u(1)
  pps["deblocking_filter_control_present_flag"] =
      deblocking_filter_control_present_flag;
  buffer->ReadBits(&constrained_intra_pred_flag, 1);  // u(1)
  pps["constrained_intra_pred_flag"] = constrained_intra_pred_flag;
  buffer->ReadBits(&redundant_pic_cnt_present_flag, 1);  // u(1)
  pps["redundant_pic_cnt_present_flag"] = redundant_pic_cnt_present_flag;

  this->bottom_field_pic_order_in_frame_present_flag =
      bottom_field_pic_order_in_frame_present_flag;
  this->redundant_pic_cnt_present_flag = redundant_pic_cnt_present_flag;
  this->entropy_coding_mode_flag = entropy_coding_mode_flag;
  this->deblocking_filter_control_present_flag =
      deblocking_filter_control_present_flag;
  this->num_slice_groups_minus1 = num_slice_groups_minus1;
  this->slice_group_map_type = slice_group_map_type;
  return pps;
}

nlohmann::json PayloadH264::parseSliceHeader(const uint8_t* buff,
                                             uint16_t length) {
  std::unique_ptr<rtc::BitBuffer> buffer(new rtc::BitBuffer(buff, length));

  uint32_t first_mb_in_slice{0};
  uint32_t slice_type{0};
  uint32_t pic_parameter_set_id{0};
  uint32_t colour_plane_id{0};
  // 解码帧号
  uint32_t frame_num{0};

  uint32_t field_pic_flag{0};
  uint32_t bottom_field_flag{0};
  uint32_t idr_pic_id{0};
  uint32_t pic_order_cnt_lsb{0};
  uint32_t delta_pic_order_cnt_bottom{0};
  uint32_t delta_pic_order_cnt{0};
  uint32_t redundant_pic_cnt{0};
  uint32_t direct_spatial_mv_pred_flag{0};
  uint32_t num_ref_idx_active_override_flag{0};
  uint32_t num_ref_idx_l0_active_minus1{0};
  uint32_t num_ref_idx_l1_active_minus1{0};
  uint32_t cabac_init_idc{0};
  uint32_t slice_qp_delta{0};
  uint32_t sp_for_switch_flag{0};
  uint32_t slice_qs_delta{0};
  // 区块滤波
  uint32_t disable_deblocking_filter_idc{0};

  uint32_t slice_alpha_c0_offset_div2{0};
  uint32_t slice_beta_offset_div2{0};
  uint32_t slice_group_change_cycle{0};

  std::ostringstream oss;
  nlohmann::json slice_header;

  buffer->ReadExponentialGolomb(&first_mb_in_slice);  // ue(v)
  slice_header["first_mb_in_slice"] = first_mb_in_slice;
  buffer->ReadExponentialGolomb(&slice_type);  // ue(v)
  oss << slice_type << "(" << sliceType2String[slice_type] << ")";
  slice_header["slice_type"] = oss.str();
  buffer->ReadExponentialGolomb(&pic_parameter_set_id);  // ue(v)
  slice_header["pic_parameter_set_id"] = pic_parameter_set_id;
  if (this->separate_colour_plane_flag == 1) {
    buffer->ReadBits(&colour_plane_id, 2);  // u(2)
    slice_header["colour_plane_id"] = colour_plane_id;
  }
  buffer->ReadBits(&frame_num, this->log2_max_frame_num);  // u(v)
  slice_header["frame_num"] = frame_num;
  if (!this->frame_mbs_only_flag) {
    buffer->ReadBits(&field_pic_flag, 1);  // u(1)
    slice_header["field_pic_flag"] = field_pic_flag;
    if (field_pic_flag) {
      buffer->ReadBits(&bottom_field_flag, 1);  // u(1)
      slice_header["bottom_field_flag"] = bottom_field_flag;
    }
  }
  bool is_idr = (buff[0] & 0x0F) == webrtc::H264::NaluType::kIdr;
  if (is_idr) {
    buffer->ReadExponentialGolomb(&idr_pic_id);  // ue(v)
    slice_header["idr_pic_id"] = idr_pic_id;
  }
  if (this->pic_order_cnt_type == 0) {
    buffer->ReadBits(&pic_order_cnt_lsb,
                     this->log2_max_pic_order_cnt_lsb);  // u(v)
    slice_header["pic_order_cnt_lsb"] = pic_order_cnt_lsb;
    if (bottom_field_pic_order_in_frame_present_flag && !field_pic_flag) {
      buffer->ReadExponentialGolomb(&delta_pic_order_cnt_bottom);  // se(v)
      slice_header["delta_pic_order_cnt_bottom"] = delta_pic_order_cnt_bottom;
    }
  }
  if (this->pic_order_cnt_type == 1 &&
      !this->delta_pic_order_always_zero_flag) {
    slice_header["delta_pic_order_cnt"] = nlohmann::json::array();
    buffer->ReadExponentialGolomb(&delta_pic_order_cnt);  // se(v)
    slice_header["delta_pic_order_cnt"].push_back(delta_pic_order_cnt);
    if (bottom_field_pic_order_in_frame_present_flag && !field_pic_flag) {
      buffer->ReadExponentialGolomb(&delta_pic_order_cnt);  // se(v)
      slice_header["delta_pic_order_cnt"].push_back(delta_pic_order_cnt);
    }
  }
  if (this->redundant_pic_cnt_present_flag) {
    buffer->ReadExponentialGolomb(&redundant_pic_cnt);  // ue(v)
    slice_header["redundant_pic_cnt"] = redundant_pic_cnt;
  }
  if (slice_type == webrtc::H264::SliceType::kB) {
    buffer->ReadBits(&direct_spatial_mv_pred_flag, 1);  // u(1)
    slice_header["direct_spatial_mv_pred_flag"] = direct_spatial_mv_pred_flag;
  }
  if (slice_type == webrtc::H264::SliceType::kP ||
      slice_type == webrtc::H264::SliceType::kSp ||
      slice_type == webrtc::H264::SliceType::kB) {
    buffer->ReadBits(&num_ref_idx_active_override_flag, 1);  // u(1)
    slice_header["num_ref_idx_active_override_flag"] =
        num_ref_idx_active_override_flag;
    if (num_ref_idx_active_override_flag) {
      buffer->ReadExponentialGolomb(&num_ref_idx_l0_active_minus1);  // ue(v)
      slice_header["num_ref_idx_l0_active_minus1"] =
          num_ref_idx_l0_active_minus1;
      if (slice_type == webrtc::H264::SliceType::kB) {
        buffer->ReadExponentialGolomb(&num_ref_idx_l1_active_minus1);  // ue(v)
        slice_header["num_ref_idx_l1_active_minus1"] =
            num_ref_idx_l1_active_minus1;
      }
    }
  }
  if (this->entropy_coding_mode_flag &&
      slice_type != webrtc::H264::SliceType::kI &&
      slice_type != webrtc::H264::SliceType::kSi) {
    buffer->ReadExponentialGolomb(&cabac_init_idc);  // ue(v)
    slice_header["cabac_init_idc"] = cabac_init_idc;
  }
  buffer->ReadExponentialGolomb(&slice_qp_delta);  // se(v)
  slice_header["slice_qp_delta"] = slice_qp_delta;
  if (slice_type == webrtc::H264::SliceType::kSp ||
      slice_type == webrtc::H264::SliceType::kSi) {
    if (slice_type == webrtc::H264::SliceType::kSp) {
      buffer->ReadBits(&sp_for_switch_flag, 1);  // u(1)
      slice_header["sp_for_switch_flag"] = sp_for_switch_flag;
    }
    buffer->ReadExponentialGolomb(&slice_qs_delta);  // se(v)
    slice_header["slice_qs_delta"] = slice_qs_delta;
  }
  if (this->deblocking_filter_control_present_flag) {
    buffer->ReadExponentialGolomb(&disable_deblocking_filter_idc);  // ue(v)
    slice_header["disable_deblocking_filter_idc"] =
        disable_deblocking_filter_idc;
    if (disable_deblocking_filter_idc != 1) {
      buffer->ReadExponentialGolomb(&slice_alpha_c0_offset_div2);  // se(v)
      slice_header["slice_alpha_c0_offset_div2"] = slice_alpha_c0_offset_div2;
      buffer->ReadExponentialGolomb(&slice_beta_offset_div2);  // se(v)
      slice_header["slice_beta_offset_div2"] = slice_beta_offset_div2;
    }
  }
  if (this->num_slice_groups_minus1 > 0 && this->slice_group_map_type >= 3 &&
      this->slice_group_map_type <= 5) {
    // buffer->ReadBits(&slice_group_change_cycle, v); // u(v)
    slice_header["slice_group_change_cycle"] = "null";
  }
  return slice_header;
}

PayloadAV1::PayloadAV1() {}

nlohmann::json PayloadAV1::parse(const uint8_t* buff, uint16_t length) {
  /*
     0 1 2 3 4 5 6 7
    +-+-+-+-+-+-+-+-+
    |Z|Y| W |N|-|-|-|
    +-+-+-+-+-+-+-+-+
  */
  uint8_t aggregation_header = buff[0];
  uint16_t continues_obu =
      (aggregation_header & 0b1000'0000u) >> 7;  // 非第一个OBU
  uint16_t expect_continues_obu =
      (aggregation_header & 0b0100'0000u) >> 6;  // 非最后一个OBU
  uint16_t num_expected_obus =
      (aggregation_header & 0b0011'0000u) >> 4;  // OBU元素个数
  uint16_t frame_key = (aggregation_header & 0b0000'1000u) >> 3;  // 关键帧
    
  payloads_.push_back({buff, length});
  if (expect_continues_obu == 1) {
    return WEBRTC_VIDEO_CODEC_OK;
  }

  webrtc::VideoRtpDepacketizerAv1 depacketizer;
  
  std::vector<rtc::ArrayView<const uint8_t>> rtp_payloads(payloads_.begin(), payloads_.end());
  rtc::scoped_refptr<webrtc::EncodedImageBuffer> bitstream =
      depacketizer.AssembleFrame(rtp_payloads);
  payloads_.clear();

  if (!context_) {
    context_.reset(new aom_codec_ctx_t);

    aom_codec_dec_cfg_t config = {8u,  // Max # of threads.
                                  0,   // Frame width set after decode.
                                  0,   // Frame height set after decode.
                                  1};  // Enable low-bit-depth code path.
    aom_codec_err_t ret =
        aom_codec_dec_init(context_.get(), aom_codec_av1_dx(), &config, 0);

    if (ret != AOM_CODEC_OK) {
      RTC_LOG(LS_WARNING) << "LibaomAv1Decoder::InitDecode returned " << ret
                          << " on aom_codec_dec_init.";
      context_.reset();
      return WEBRTC_VIDEO_CODEC_OK;
    }
  }

  aom_codec_err_t ret =
      aom_codec_decode(context_.get(), bitstream->data(), bitstream->size(),
                       /*user_priv=*/nullptr);
  if (ret != AOM_CODEC_OK) {
    RTC_LOG(LS_WARNING) << "LibaomAv1Decoder::Decode returned " << ret
                        << " on aom_codec_decode.";
    return nlohmann::json();
  }

  // Get decoded frame data.
  int corrupted_frame = 0;
  aom_codec_iter_t iter = nullptr;
  while (aom_image_t* decoded_image =
             aom_codec_get_frame(context_.get(), &iter)) {
    if (aom_codec_control(context_.get(), AOMD_GET_FRAME_CORRUPTED,
                          &corrupted_frame)) {
      RTC_LOG(LS_WARNING) << "LibaomAv1Decoder::Decode "
                             "AOM_GET_FRAME_CORRUPTED.";
    }
    // Check that decoded image format is I420 and has 8-bit depth.
    if (decoded_image->fmt != AOM_IMG_FMT_I420) {
      RTC_LOG(LS_WARNING) << "LibaomAv1Decoder::Decode invalid image format";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // Return decoded frame data.
    int qp;
    ret = aom_codec_control(context_.get(), AOMD_GET_LAST_QUANTIZER, &qp);
    if (ret != AOM_CODEC_OK) {
      RTC_LOG(LS_WARNING) << "LibaomAv1Decoder::Decode returned " << ret
                          << " on control AOME_GET_LAST_QUANTIZER.";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // Allocate memory for decoded frame.
    rtc::scoped_refptr<webrtc::I420Buffer> buffer =
        buffer_pool_.CreateI420Buffer(decoded_image->d_w, decoded_image->d_h);
    if (!buffer.get()) {
      // Pool has too many pending frames.
      RTC_LOG(LS_WARNING) << "LibaomAv1Decoder::Decode returned due to lack of"
                             " space in decoded frame buffer pool.";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // Copy decoded_image to decoded_frame.
    libyuv::I420Copy(
        decoded_image->planes[AOM_PLANE_Y], decoded_image->stride[AOM_PLANE_Y],
        decoded_image->planes[AOM_PLANE_U], decoded_image->stride[AOM_PLANE_U],
        decoded_image->planes[AOM_PLANE_V], decoded_image->stride[AOM_PLANE_V],
        buffer->MutableDataY(), buffer->StrideY(), buffer->MutableDataU(),
        buffer->StrideU(), buffer->MutableDataV(), buffer->StrideV(),
        decoded_image->d_w, decoded_image->d_h);

    //static std::fstream f("a.yuv", std::ios::binary | std::ios::out);
    //f << std::string((char*)buffer->DataY(),
    //                 buffer->width() * buffer->height() * 3 / 2);
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

// https://aomediacodec.github.io/av1-spec/av1-spec.pdf 5.3.1  Last modified: 2019-01-08 11:48 PT
int PayloadAV1::open_bitstream_unit(const uint8_t* buff,
                                               uint16_t length) {
  //if (obu_type == OBU_SEQUENCE_HEADER) {
  //  //json["sequence_header_obu"] = sequence_header_obu();
  //} else if (obu_type == OBU_TEMPORAL_DELIMITER) {
  //  //temporal_delimiter_obu();
  //} else if (obu_type == OBU_FRAME_HEADER) {
  //  //frame_header_obu();
  //} else if (obu_type == OBU_REDUNDANT_FRAME_HEADER) {
  //  //frame_header_obu();
  //} else if (obu_type == OBU_TILE_GROUP) {
  //  //tile_group_obu(obu_size);
  //} else if (obu_type == OBU_METADATA) {
  //  //metadata_obu();
  //} else if (obu_type == OBU_FRAME) {
  //  //frame_obu(obu_size);
  //} else if (obu_type == OBU_TILE_LIST) {
  //  //tile_list_obu();
  //} else if (obu_type == OBU_PADDING) {
  //  //padding_obu();
  //} else {
  //  //reserved_obu();
  //}
  return 0;
}

//nlohmann::json PayloadAV1::obu_header() {
//  std::ostringstream oss;
//
//  uint32_t obu_forbidden_bit{0};            // 为0
//  obu_type = 0;                   // 取值范围1-8,15；其他值保留
//  obu_extension_flag = 0;  // 扩展字段
//  obu_has_size_field = 0; // 有obu size字段
//  uint32_t obu_reserved_1bit = 0;  //为0，保留字段
//  buffer_->ReadBits(&obu_forbidden_bit, 1); // f(1)  保留字段
//  buffer_->ReadBits(&obu_type, 4); // f(4)
//  buffer_->ReadBits(&obu_extension_flag, 1); // f(1)
//  buffer_->ReadBits(&obu_has_size_field, 1); // f(1)
//  buffer_->ReadBits(&obu_reserved_1bit, 1); // f(1)  保留字段
//
//  oss << obu_type << "(" << obuType2String[(OBU_TYPE)obu_type] << ")";
//  nlohmann::json json = {
//      {"obu_forbidden_bit", obu_forbidden_bit},
//      {"obu_type", oss.str()},
//      {"obu_extension_flag", obu_extension_flag},
//      {"obu_has_size_field", obu_has_size_field},
//      {"obu_reserved_1bit", obu_reserved_1bit},
//  };
//  if (obu_extension_flag == 1) {
//    json["obu_extension_header"] = obu_extension_header();
//  } else {
//    temporal_id = 0;
//    spatial_id = 0;
//  }
//  return json;
//}
//
//nlohmann::json PayloadAV1::obu_extension_header() {
//  uint32_t extension_header_reserved_3bits{0};  // 为0，保留字段
//  buffer_->ReadBits(&temporal_id, 3); // f(3)
//  buffer_->ReadBits(&spatial_id, 2);            // f(2)
//  buffer_->ReadBits(&extension_header_reserved_3bits, 3);  // f(3)
//  return {
//      {"temporal_id", temporal_id},
//      {"spatial_id", spatial_id},
//      {"extension_header_reserved_3bits", extension_header_reserved_3bits},
//  };
//}

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
  webrtc::RtpPacket rtpPacket;
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
  if (rtpPacket.payload_size()) {
    json["payload"] =
        this->parsePayload(rtpPacket.PayloadBuffer(), rtpPacket.PayloadType());
  }

  if (rtpPacket.PayloadType() == 124) {
    flexfec_->insertMediaPacket(rtpPacket);
  } else if (rtpPacket.PayloadType() == 115) {
    flexfec_->insertFecPacket(rtpPacket);
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
nlohmann::json RtpPacket::parseHeader(const webrtc::RtpPacket& rtpPacket) {
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
nlohmann::json RtpPacket::parseExtension(const webrtc::RtpPacket& rtpPacket) {
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

nlohmann::json RtpPacket::parsePayload(const rtc::CopyOnWriteBuffer& payload,
                                       uint8_t payloadType) {
  const uint8_t* ptr = payload.data();
  uint16_t len = payload.size();
  if (payloadType == 111) {
    if (!audio_) {
      audio_.reset(new PayloadOpus);
    }
    return audio_->parse(ptr, len);
  } else if (payloadType == 124) {
    if (!video_) {
      video_.reset(new PayloadH264);
    }
    return video_->parse(ptr, len);
  } else if (payloadType == 35) {
    if (!video_) {
      video_.reset(new PayloadAV1);
    }
    return video_->parse(ptr, len);
  } else if (payloadType == 115) {
    return flexfec_->parse(ptr, len);
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
  webrtc::RtpPacket rtpPacket;
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
