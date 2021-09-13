#ifndef CHAI_RTP_PACKET_H
#define CHAI_RTP_PACKET_H

#include <pc/peer_connection.h>
#include <rtc_base/bit_buffer.h>

#include <json.hpp>
//#include "Encode.h"

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
  nlohmann::json parse(const uint8_t* buff, uint16_t length) override;

 protected:
  // 5.3
  nlohmann::json open_bitstream_unit(uint64_t sz);
  nlohmann::json obu_header();
  nlohmann::json obu_extension_header();
  //nlohmann::json trailing_bits();
  //nlohmann::json byte_alignment();
  //// 5.4
  //nlohmann::json reserved_obu();
  //// 5.5
  nlohmann::json sequence_header_obu();
  nlohmann::json color_config();
  nlohmann::json timing_info();
  nlohmann::json decoder_model_info();
  nlohmann::json operating_parameters_info(uint16_t op);
  //// 5.6
  //nlohmann::json temporal_delimiter_obu();
  //// 5.7
  //nlohmann::json padding_obu();
  //// 5.8
  //nlohmann::json metadata_obu();
  //nlohmann::json metadata_itut_t35();
  //nlohmann::json metadata_hdr_cll();
  //nlohmann::json metadata_hdr_mdcv();
  //nlohmann::json metadata_scalability();
  //nlohmann::json scalability_structure();
  //nlohmann::json metadata_timecode();
  //// 5.9
  //nlohmann::json frame_header_obu();
  //nlohmann::json uncompressed_header();
  //nlohmann::json get_relative_dist();
  //nlohmann::json mark_ref_frames();
  //nlohmann::json frame_size();
  //nlohmann::json render_size();
  //nlohmann::json frame_size_with_refs();
  //nlohmann::json superres_params();
  //nlohmann::json compute_image_size();
  //nlohmann::json read_interpolation_filter();
  //nlohmann::json loop_filter_params();
  //nlohmann::json quantization_params();
  //nlohmann::json read_delta_q();
  //nlohmann::json segmentation_params();
  //nlohmann::json tile_info();
  //nlohmann::json tile_log2();
  //nlohmann::json delta_q_params();
  //nlohmann::json delta_lf_params();
  //nlohmann::json cdef_params();
  //nlohmann::json lr_params();
  //nlohmann::json read_tx_mode();
  //nlohmann::json skip_mode_params();
  //nlohmann::json frame_reference_mode();
  //nlohmann::json global_motion_params();
  //nlohmann::json read_global_param();
  //nlohmann::json decode_signed_subexp_with_ref();
  //nlohmann::json decode_unsigned_subexp_with_ref();
  //nlohmann::json decode_subexp();
  //nlohmann::json inverse_recenter();
  //nlohmann::json film_grain_params();
  //nlohmann::json temporal_point_info();
  //// 5.10
  //nlohmann::json frame_obu(uint64_t obu_size);
  //// 5.11
  //nlohmann::json tile_group_obu(uint64_t obu_size);
  //nlohmann::json decode_tile();
  //nlohmann::json clear_block_decoded_flags();
  //nlohmann::json decode_partition();
  //nlohmann::json decode_block();
  //nlohmann::json mode_info();
  //nlohmann::json intra_frame_mode_info();
  //nlohmann::json intra_segment_id();
  //nlohmann::json read_segment_id();
  //nlohmann::json read_skip_mode();
  //nlohmann::json read_skip();
  //nlohmann::json read_delta_qindex();
  //nlohmann::json read_delta_lf();
  //nlohmann::json seg_feature_active_idx();
  //nlohmann::json seg_feature_active();
  //nlohmann::json read_tx_size();
  //nlohmann::json read_block_tx_size();
  //nlohmann::json read_var_tx_size();
  //nlohmann::json inter_frame_mode_info();
  //nlohmann::json inter_segment_id();
  //nlohmann::json read_is_inter();
  //nlohmann::json get_segment_id();
  //nlohmann::json intra_block_mode_info();
  //nlohmann::json inter_block_mode_info();
  //nlohmann::json filter_intra_mode_info();
  //nlohmann::json read_ref_frames();
  //nlohmann::json assign_mv();
  //nlohmann::json read_motion_mode();
  //nlohmann::json read_interintra_mode();
  //nlohmann::json read_compound_type();
  //nlohmann::json get_mode();
  //nlohmann::json read_mv();
  //nlohmann::json read_mv_component();
  //nlohmann::json compute_prediction();
  //nlohmann::json residual();
  //nlohmann::json transform_block();
  //nlohmann::json transform_tree();
  //nlohmann::json get_tx_size();
  //nlohmann::json get_plane_residual_size();
  //nlohmann::json coeffs();
  //nlohmann::json compute_tx_type();
  //nlohmann::json get_mrow_scan();
  //nlohmann::json intra_angle_info_y();
  //nlohmann::json intra_angle_info_uv();
  //nlohmann::json is_directional_mode();
  //nlohmann::json read_cfl_alphas();
  //nlohmann::json palette_mode_info();
  //nlohmann::json get_palette_cache();
  //nlohmann::json transform_type();
  //nlohmann::json get_tx_set();
  //nlohmann::json palette_tokens();
  //nlohmann::json get_palette_color_context();
  //nlohmann::json is_inside();
  //nlohmann::json is_inside_filter_region();
  //nlohmann::json clamp_mv_row();
  //nlohmann::json clamp_mv_col();
  //nlohmann::json clear_cdef();
  //nlohmann::json read_cdef();
  //nlohmann::json read_lr();
  //nlohmann::json read_lr_unit();
  //nlohmann::json decode_signed_subexp_with_ref_bool();
  //nlohmann::json decode_unsigned_subexp_with_ref_bool();
  //nlohmann::json decode_subexp_bool();
  //// 5.12
  //nlohmann::json tile_list_obu();
  //nlohmann::json tile_list_entry();

protected:
  uint64_t leb128();
 uint32_t uvlc();

 private:
  std::unique_ptr<rtc::BitBuffer> buffer_;

  uint32_t seq_profile{0}; 
  uint32_t obu_type{0};
  uint32_t obu_extension_flag{0};
  uint32_t obu_has_size_field{0};
  uint32_t operating_point_idc[32];
  uint32_t buffer_delay_length_minus_1{0};
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
