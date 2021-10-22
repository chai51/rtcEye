#include "PayloadAV1.h"

#include <rtc_base/logging.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <libyuv/convert.h>
#include <libyuv/video_common.h>

namespace chai {

PayloadAV1::PayloadAV1() {
  color1_ = VIDEO_L0T0_COLOR;
  color2_ = VIDEO_L0T0_COLOR;
}

nlohmann::json PayloadAV1::parse(const uint8_t* buff, uint16_t length) {
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

  aom_codec_err_t ret = aom_codec_decode(context_.get(), buff, length,
                                         /*user_priv=*/nullptr);
  if (ret != AOM_CODEC_OK) {
    RTC_LOG(LS_WARNING) << "LibaomAv1Decoder::Decode returned " << ret
                        << " on aom_codec_decode.";
    return nlohmann::json();
  }

  // Get decoded frame data.
  aom_codec_iter_t iter = nullptr;
  while (aom_image_t* decoded_image =
             aom_codec_get_frame(context_.get(), &iter)) {  }

  this->buff_ = buff;
  nlohmann::json obus = nlohmann::json::array();
  while (buff + length != this->buff_) {
    obus.push_back(open_bitstream_unit(buff - this->buff_));
  }
  return obus;
}

// https://aomediacodec.github.io/av1-spec/av1-spec.pdf 5.3.1  Last modified:
// 2019-01-08 11:48 PT
nlohmann::json PayloadAV1::open_bitstream_unit(uint64_t sz) {
  nlohmann::json obu;
  uint64_t obu_size{0};
  obu["header"] = obu_header();
  if (header.has_size_field) {
    uint32_t len = leb128(this->buff_ + header.size, obu_size);
    this->buff_ += header.size + len + obu_size;
  } else {
    obu_size = sz - header.size;
    this->buff_ += sz;
  }
  OBU_TYPE type = header.type;

  auto* ctx = (aom_codec_alg_priv_t*)context_->priv;
  if (type == OBU_SEQUENCE_HEADER) {
    obu["sequence_header"] = obu_sequence_header();
  } else if (type == OBU_TEMPORAL_DELIMITER) {
  } else if (type == OBU_FRAME_HEADER) {
  } else if (type == OBU_REDUNDANT_FRAME_HEADER) {
  } else if (type == OBU_TILE_GROUP) {
  } else if (type == OBU_METADATA) {
  } else if (type == OBU_FRAME) {
    obu["frame"] = obu_frame(obu_size);
  } else if (type == OBU_TILE_LIST) {
  } else if (type == OBU_PADDING) {
  } else {
  }
  return obu;
}

uint32_t PayloadAV1::leb128(const uint8_t* buff, uint64_t& value) {
  value = 0;
  uint16_t i{0};
  for (i = 0; i < 8; i++) {
    uint8_t leb128_byte = buff[i];
    value |= ((leb128_byte & 0x7f) << (i * 7));
    if (!(leb128_byte & 0x80)) {
      break;
    }
  }
  return i + 1;
}

nlohmann::json PayloadAV1::obu_header() {
  uint8_t byte1 = buff_[0];
  header.type = OBU_TYPE((byte1 & 0b0111'1000) >> 3);
  header.has_extension = (byte1 & 0b0000'0100) >> 2;
  header.has_size_field = (byte1 & 0b0000'0010) >> 1;
  header.size = 1;
  header.temporal_layer_id = 0;
  header.spatial_layer_id = 0;

  if (!header.has_extension) {
    color1_ = color2_ = VIDEO_L0T0_COLOR;
    return {{"obu_type", header.type},
            {"size", header.size}};
  }

  byte1 = this->buff_[1];
  header.temporal_layer_id = (byte1 & 0b1110'0000) >> 5;
  header.spatial_layer_id = (byte1 & 0b0001'1000) >> 3;
  header.size += 1;
  switch (header.temporal_layer_id) {
    case 1:
      color1_ = color2_ = VIDEO_L0T1_COLOR;
      break;
    case 2:
      color1_ = color2_ = VIDEO_L0T2_COLOR;
      break;
  }

  return {{"obu_type", header.type},
          {"size", header.size},
          {"temporal_id", header.temporal_layer_id},
          {"spatial_id", header.spatial_layer_id}};
}

nlohmann::json PayloadAV1::obu_sequence_header() {
  auto* ctx = (aom_codec_alg_priv_t*)context_->priv;
  AVxWorker* worker = get_worker(ctx);
  FrameWorkerData* const frame_worker_data = (FrameWorkerData*)worker->data1;
  AV1Decoder* pbi = frame_worker_data->pbi;

  SequenceHeader* const seq_params = pbi->common.seq_params;

  nlohmann::json obu = {
      {"seq_profile", seq_params->profile},
      {"still_picture", seq_params->still_picture},
      {"reduced_still_picture_header", seq_params->reduced_still_picture_hdr},
  };
  if (seq_params->reduced_still_picture_hdr) {
    obu["seq_level_idx"] = seq_params->seq_level_idx[0];
  } else {
    obu["timing_info_present_flag"] = seq_params->timing_info_present;
    if (seq_params->timing_info_present) {
      obu["timing_info"] = timing_info(seq_params->timing_info);
      obu["decoder_model_info_present_flag"] =
          seq_params->decoder_model_info_present_flag;
      if (seq_params->decoder_model_info_present_flag) {
        obu["decoder_model_info"] =
            decoder_model_info(seq_params->decoder_model_info);
      }
    } 
    obu["initial_display_delay_present_flag"] =
        seq_params->display_model_info_present_flag;
    obu["operating_points_cnt_minus_1"] =
        seq_params->operating_points_cnt_minus_1;
    obu["operating_points"] = nlohmann::json::array();
    for (int i = 0; i <= seq_params->operating_points_cnt_minus_1; ++i) {
      nlohmann::json operating_point = {
          {"operating_point_idc", seq_params->operating_point_idc[i]},
          {"seq_level_idx", seq_params->seq_level_idx[i]},
          {"seq_tier", seq_params->tier[i]},
      };

      if (seq_params->decoder_model_info_present_flag) {
        operating_point["decoder_model_present_for_this_op"] =
            seq_params->op_params[i].decoder_model_param_present_flag;
        if (seq_params->op_params[i].decoder_model_param_present_flag) {
          operating_point["operating_parameters_info"] =
              operating_parameters_info(seq_params->op_params[i]);
        }
      }
      if (seq_params->display_model_info_present_flag) {
        operating_point["initial_display_delay_present_for_this_op"] =
            seq_params->op_params[i].display_model_param_present_flag;
        if (seq_params->op_params[i].display_model_param_present_flag) {
          operating_point["initial_display_delay"] =
              seq_params->op_params[i].initial_display_delay;
        }
      }
      obu["operating_points"].push_back(operating_point);
    }
  }

  obu["frame_width_bits"] = seq_params->num_bits_width;
  obu["frame_height_bits"] = seq_params->num_bits_height;
  obu["max_frame_width"] = seq_params->max_frame_width;
  obu["max_frame_height"] = seq_params->max_frame_height;
  obu["frame_id_numbers_present_flag"] =
      seq_params->frame_id_numbers_present_flag;
  obu["frame_id_numbers_present_flag"] =
      seq_params->frame_id_numbers_present_flag;
  if (seq_params->frame_id_numbers_present_flag) {
    obu["delta_frame_id_length"] = seq_params->delta_frame_id_length;
    obu["additional_frame_id_length"] = seq_params->frame_id_length;
  }
  obu["sb_size"] = seq_params->sb_size; // 128X128->15  64X64:12
  obu["enable_filter_intra"] = seq_params->enable_filter_intra;
  obu["enable_intra_edge_filter"] = seq_params->enable_intra_edge_filter;

  obu["enable_interintra_compound"] = seq_params->enable_interintra_compound;
  obu["enable_masked_compound"] = seq_params->enable_masked_compound;
  obu["enable_warped_motion"] = seq_params->enable_warped_motion;
  obu["enable_dual_filter"] = seq_params->enable_dual_filter;
  obu["enable_order_hint"] = seq_params->order_hint_info.enable_order_hint;
  obu["enable_jnt_comp"] = obu["enable_ref_frame_mvs"] =
      seq_params->order_hint_info.enable_dist_wtd_comp;
  obu["enable_ref_frame_mvs"] =
      seq_params->order_hint_info.enable_ref_frame_mvs;
  obu["seq_force_screen_content_tools"] =
      seq_params->force_screen_content_tools;
  obu["seq_choose_integer_mv"] = seq_params->force_integer_mv;
  if (seq_params->order_hint_info.enable_order_hint) {
    obu["order_hint_bits_minus_1"] =
        seq_params->order_hint_info.order_hint_bits_minus_1;
  }

  obu["enable_superres"] = seq_params->enable_superres;
  obu["enable_cdef"] = seq_params->enable_cdef;
  obu["enable_restoration"] = seq_params->enable_restoration;
  obu["color_config"] = color_config(seq_params);
  obu["film_grain_params_present"] = seq_params->film_grain_params_present;

  return obu;
}

nlohmann::json PayloadAV1::timing_info(aom_timing_info_t& timing_info) {
  nlohmann::json json = {
      {"num_units_in_display_tick", timing_info.num_units_in_display_tick},
      {"time_scale", timing_info.time_scale},
      {"equal_picture_interval", timing_info.equal_picture_interval},
  };
  if (timing_info.equal_picture_interval) {
    json["num_ticks_per_picture"] = timing_info.num_ticks_per_picture;
  }
  return json;
}

nlohmann::json PayloadAV1::decoder_model_info(
    aom_dec_model_info_t& decoder_model_info) {
  return {
      {"buffer_delay_length",
       decoder_model_info.encoder_decoder_buffer_delay_length},
      {"num_units_in_decoding_tick",
       decoder_model_info.num_units_in_decoding_tick},
      {"buffer_removal_time_length",
       decoder_model_info.buffer_removal_time_length},
      {"frame_presentation_time_length",
       decoder_model_info.frame_presentation_time_length},
  };
}

nlohmann::json PayloadAV1::operating_parameters_info(
    aom_dec_model_op_parameters_t& op_params) {
  return {
      {"decoder_buffer_delay", op_params.decoder_buffer_delay},
      {"encoder_buffer_delay", op_params.encoder_buffer_delay},
      {"low_delay_mode_flag", op_params.low_delay_mode_flag},
  };
}

nlohmann::json PayloadAV1::color_config(SequenceHeader* const seq_params) {
  return {
      {"bit_depth", seq_params->bit_depth},
      {"mono_chrome", seq_params->monochrome},
      {"color_primaries", seq_params->color_primaries},
      {"transfer_characteristics", seq_params->transfer_characteristics},
      {"matrix_coefficients", seq_params->matrix_coefficients},
      {"color_range", seq_params->color_range},
      {"subsampling_x", seq_params->subsampling_x},
      {"subsampling_y", seq_params->subsampling_y},
      {"chroma_sample_position", seq_params->chroma_sample_position},
      {"separate_uv_delta_q", seq_params->separate_uv_delta_q},
  };
}

nlohmann::json PayloadAV1::obu_frame(uint64_t sz) {
  return {
      {"frame_header_obu", frame_header_obu()},
      //{"byte_alignment", byte_alignment()},
      //{"tile_group_obu", tile_group_obu()},
  };
}
nlohmann::json PayloadAV1::frame_header_obu() {
  return {
      {"uncompressed_header", uncompressed_header()},
      //{"decode_frame_wrapup", decode_frame_wrapup()},
  };
}
nlohmann::json PayloadAV1::uncompressed_header() {
  auto* ctx = (aom_codec_alg_priv_t*)context_->priv;
  AVxWorker* worker = get_worker(ctx);
  FrameWorkerData* const frame_worker_data = (FrameWorkerData*)worker->data1;
  AV1Decoder* pbi = frame_worker_data->pbi;
  AV1_COMMON* const cm = &pbi->common;
  const SequenceHeader* const seq_params = cm->seq_params;
  CurrentFrame* const current_frame = &cm->current_frame;
  FeatureFlags* const features = &cm->features;

  frame_type_ = current_frame->frame_type;
  nlohmann::json uncompressed;
  if (seq_params->reduced_still_picture_hdr) {
    uncompressed["show_existing_frame"] = cm->show_existing_frame;
    uncompressed["frame_type"] = current_frame->frame_type;
    uncompressed["show_frame"] = cm->show_frame;
    uncompressed["showable_frame"] = cm->showable_frame;
  } else {
    uncompressed["show_existing_frame"] = cm->show_existing_frame;
    if (cm->show_existing_frame) {
      uncompressed["show_existing_frame"] = cm->show_existing_frame;
      if (seq_params->decoder_model_info_present_flag &&
          seq_params->timing_info.equal_picture_interval == 0) {
        uncompressed["temporal_point_info"] = temporal_point_info(cm);
      }
      if (seq_params->frame_id_numbers_present_flag) {
      }
      return uncompressed;
    }
    uncompressed["frame_type"] = current_frame->frame_type;
    uncompressed["show_frame"] = cm->show_frame;
    if (cm->show_frame && seq_params->decoder_model_info_present_flag &&
        seq_params->timing_info.equal_picture_interval == 0) {
      uncompressed["temporal_point_info"] = temporal_point_info(cm);
    }
    uncompressed["showable_frame"] = cm->showable_frame;
    uncompressed["error_resilient_mode"] = features->error_resilient_mode;
  }
  uncompressed["disable_cdf_update"] = features->disable_cdf_update;
  uncompressed["allow_screen_content_tools"] =
      features->allow_screen_content_tools;
  uncompressed["force_integer_mv"] = features->allow_screen_content_tools;
  uncompressed["current_frame_id"] = cm->current_frame_id;
  uncompressed["order_hint"] = current_frame->order_hint;
  uncompressed["primary_ref_frame"] = features->primary_ref_frame;
  if (seq_params->decoder_model_info_present_flag) {
    uncompressed["buffer_removal_time_present_flag"] =
        pbi->buffer_removal_time_present;
    if (pbi->buffer_removal_time_present) {
      uncompressed["buffer_removal_time"] = nlohmann::json::array();
      for (int op_num = 0;
           op_num <= seq_params->operating_points_cnt_minus_1; ++op_num) {
        uncompressed["buffer_removal_time"].push_back(
            {{"buffer_removal_time", cm->buffer_removal_times[op_num]}});
      }
    }
  }
  uncompressed["refresh_frame_flags"] = current_frame->refresh_frame_flags;
  if (current_frame->frame_type == KEY_FRAME) {
    uncompressed["frame_size"] = frame_size();
    uncompressed["render_size"] = render_size();
  } else {
    uncompressed["frame_size_with_refs"] = frame_size_with_refs();
    uncompressed["frame_size"] = frame_size();
    uncompressed["render_size"] = render_size();
    uncompressed["allow_high_precision_mv"] = features->allow_high_precision_mv;
    uncompressed["read_interpolation_filter"] = read_interpolation_filter();
    uncompressed["is_motion_mode_switchable"] =
        features->switchable_motion_mode;
    uncompressed["use_ref_frame_mvs"] = features->allow_ref_frame_mvs;
  }
  uncompressed["refresh_frame_context"] = features->refresh_frame_context;
  uncompressed["tile_info"] = tile_info();
  uncompressed["quantization_params"] = quantization_params();
  uncompressed["segmentation_params"] = segmentation_params();
  uncompressed["delta_q_params"] = delta_q_params();
  uncompressed["delta_lf_params"] = delta_lf_params();
  uncompressed["loop_filter_params"] = loop_filter_params();
  uncompressed["cdef_params"] = cdef_params();
  uncompressed["lr_params"] = lr_params();
  uncompressed["read_tx_mode"] = read_tx_mode();
  uncompressed["frame_reference_mode"] = frame_reference_mode();
  uncompressed["skip_mode_params"] = skip_mode_params();
  uncompressed["allow_warped_motion"] = features->allow_warped_motion;
  uncompressed["reduced_tx_set"] = features->reduced_tx_set_used;
  uncompressed["global_motion_params"] = global_motion_params();
  uncompressed["film_grain_params"] = film_grain_params();

  if (current_frame->frame_type == KEY_FRAME) {
    color1_ = color2_ = VIDEO_KEY_COLOR;
  }
  return uncompressed;
}
nlohmann::json PayloadAV1::temporal_point_info(AV1_COMMON* const cm) {
  return {{"frame_presentation_time", cm->frame_presentation_time}};
}
nlohmann::json PayloadAV1::frame_size() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::render_size() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::tile_info() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::frame_size_with_refs() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::read_interpolation_filter() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::quantization_params() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::segmentation_params() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::delta_q_params() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::delta_lf_params() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::loop_filter_params() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::cdef_params() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::lr_params() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::read_tx_mode() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::frame_reference_mode() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::skip_mode_params() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::global_motion_params() {
  return nlohmann::json();
}
nlohmann::json PayloadAV1::film_grain_params() {
  return nlohmann::json();
}
}  // namespace chai