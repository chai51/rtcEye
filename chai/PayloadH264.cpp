#include "ScreenCapturer.h"

#include <rtc_base/logging.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <libyuv/convert.h>
#include <libyuv/video_common.h>
#include <common_video/h264/h264_common.h>
#include <modules/rtp_rtcp/source/byte_io.h>

#include "PayloadH264.h"

namespace {
const uint16_t kNalHeaderSize{1};
const uint16_t kLengthFieldSize{2};
const uint16_t kStapAHeaderSize = kNalHeaderSize + kLengthFieldSize;
// Bit masks for FU (A and B) indicators.
enum NalDefs : uint8_t { kFBit = 0x80, kNriMask = 0x60, kTypeMask = 0x1F };
// Bit masks for FU (A and B) headers.
enum FuDefs : uint8_t { kSBit = 0x80, kEBit = 0x40, kRBit = 0x20 };

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

std::map<uint8_t, std::string> chromaFormatIdc2String = {
    {0, "4:0:0"},
    {1, "4:2:0"},
    {2, "4:2:2"},
    {3, "4:4:4"},
};
}

namespace chai {
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
}  // namespace chai