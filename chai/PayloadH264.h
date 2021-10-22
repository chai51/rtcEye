#ifndef CHAI_PAYLOAD_H264_H
#define CHAI_PAYLOAD_H264_H

#include <api/video/video_frame.h>
#include <api/video/video_source_interface.h>
#include <media/base/video_adapter.h>
#include <media/base/video_broadcaster.h>
#include <api/video/i420_buffer.h>
#include <rtc_base/task_queue.h>
#include <modules/desktop_capture/desktop_capturer_wrapper.h>
#include "RtpPakcet.h"

namespace chai {
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
}  // namespace chai
#endif  // CHAI_AV1_H