#ifndef CHAI_SCREENCAPTURER_H
#define CHAI_SCREENCAPTURER_H

#include <api/video/video_frame.h>
#include <api/video/video_source_interface.h>
#include <media/base/video_adapter.h>
#include <media/base/video_broadcaster.h>
#include <api/video/i420_buffer.h>
#include <rtc_base/task_queue.h>
#include <modules/desktop_capture/desktop_capturer_wrapper.h>

namespace chai {

class ScreenCapturer : public webrtc::DesktopCapturer::Callback,
                       public rtc::VideoSourceInterface<webrtc::VideoFrame> {

 public:
  static ScreenCapturer* Create(size_t width,
                             size_t height,
                             size_t target_fps);

  ScreenCapturer();
  virtual ~ScreenCapturer();

  bool Init(size_t width, size_t height, size_t target_fps);

 public:
  virtual void OnCaptureResult(webrtc::DesktopCapturer::Result result,
      std::unique_ptr<webrtc::DesktopFrame> frame) override;

 public:
  virtual void AddOrUpdateSink(
      rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                               const rtc::VideoSinkWants& wants) override;
  virtual void RemoveSink(
      rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override;

 protected:
  void CaptureFrame();
 private:
  std::unique_ptr<webrtc::DesktopCapturer> dc{nullptr};
  rtc::VideoBroadcaster broadcaster_;
  rtc::scoped_refptr<webrtc::I420Buffer> frame_buffer_{nullptr};
  rtc::TaskQueue* screenQueue_{nullptr};
  uint32_t fps_{0};
};
}  // namespace chai
#endif  // CHAI_SCREENCAPTURER_H