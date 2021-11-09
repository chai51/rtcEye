#ifndef CHAI_PAYLOAD_AV1_H
#define CHAI_PAYLOAD_AV1_H

#include <av1/common/obu_util.h>
#include "RtpPakcet.h"

namespace chai {
class PayloadAV1 : public PayloadBase {
 public:
  PayloadAV1();
  nlohmann::json parse(const uint8_t* buff, uint16_t length) override;

 protected:
  nlohmann::json open_bitstream_unit(uint64_t sz);
  uint32_t leb128(const uint8_t* buff, uint64_t& value);

 protected:
  nlohmann::json obu_header();

  nlohmann::json obu_sequence_header();
  nlohmann::json timing_info(aom_timing_info_t& timing_info);
  nlohmann::json decoder_model_info(aom_dec_model_info_t& decoder_model_info);
  nlohmann::json operating_parameters_info(
      aom_dec_model_op_parameters_t& op_params);
  nlohmann::json color_config(SequenceHeader* const seq_params);

  nlohmann::json obu_frame(uint64_t sz);
  nlohmann::json frame_header_obu();
  nlohmann::json uncompressed_header();
  nlohmann::json temporal_point_info(AV1_COMMON* const cm);
  nlohmann::json frame_size(AV1_COMMON* cm);
  nlohmann::json render_size(AV1_COMMON* cm);
  nlohmann::json tile_info(AV1Decoder* const pbi);
  nlohmann::json quantization_params(AV1_COMMON* cm);
  nlohmann::json segmentation_params(AV1_COMMON* const cm);
  nlohmann::json delta_q_params(AV1_COMMON* const cm);
  nlohmann::json delta_lf_params(AV1_COMMON* const cm);
  nlohmann::json loop_filter_params();
  nlohmann::json cdef_params();
  nlohmann::json lr_params();
  nlohmann::json read_tx_mode();
  nlohmann::json frame_reference_mode();
  nlohmann::json skip_mode_params();
  nlohmann::json global_motion_params();
  nlohmann::json film_grain_params();
 private:
  const uint8_t* buff_{nullptr};

  std::unique_ptr<aom_codec_ctx_t> context_;
  ObuHeader header;
};
}  // namespace chai
#endif  // CHAI_AV1_H