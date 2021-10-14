#include "ScreenCapturer.h"

#include <rtc_base/logging.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <libyuv/convert.h>
#include <libyuv/video_common.h>

namespace chai {
ScreenCapturer* ScreenCapturer::Create(size_t width,
                                       size_t height,
                                       size_t target_fps) {
  std::unique_ptr<ScreenCapturer> screenCapturer(new ScreenCapturer());
  if (!screenCapturer->Init(width, height, target_fps)) {
    RTC_LOG(LS_WARNING) << "Failed to create VcmCapturer(w = " << width
                        << ", h = " << height << ", fps = " << target_fps
                        << ")";
    return nullptr;
  }
  return screenCapturer.release();
}
ScreenCapturer::ScreenCapturer() {}

ScreenCapturer::~ScreenCapturer() {
  this->screenQueue_ = nullptr;
}

bool ScreenCapturer::Init(size_t width, size_t height, size_t target_fps) {
  this->fps_ = target_fps;
  frame_buffer_ = webrtc::I420Buffer::Create(width, height);

  webrtc::DesktopCaptureOptions options =
      webrtc::DesktopCaptureOptions::CreateDefault();
  options.set_allow_directx_capturer(true);

  auto screenCapturer = webrtc::DesktopCapturer::CreateScreenCapturer(options);
  if (!screenCapturer) {
    return false;
  }

  this->dc = std::move(screenCapturer);

  this->dc->Start(this);

  auto task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
  this->screenQueue_ = new rtc::TaskQueue(task_queue_factory->CreateTaskQueue(
      "screen", webrtc::TaskQueueFactory::Priority::HIGH));

  this->CaptureFrame();

  return true;
}

void ScreenCapturer::OnCaptureResult(
    webrtc::DesktopCapturer::Result result,
    std::unique_ptr<webrtc::DesktopFrame> frame) {
  if (result != webrtc::DesktopCapturer::Result::SUCCESS) {
    return;
  }

  int width = frame->size().width();
  int height = frame->size().height();
  if (frame_buffer_->width() * frame_buffer_->height() < width * height) {
    return;
  }

  libyuv::ConvertToI420(frame->data(), 0, frame_buffer_->MutableDataY(),
                        frame_buffer_->StrideY(), frame_buffer_->MutableDataU(),
                        frame_buffer_->StrideU(), frame_buffer_->MutableDataV(),
                        frame_buffer_->StrideV(), 0, 0, width, height,
                        frame_buffer_->width(), frame_buffer_->height(),
                        libyuv::kRotate0, libyuv::FOURCC_ARGB);

  broadcaster_.OnFrame(
      webrtc::VideoFrame(frame_buffer_, 0, 0, webrtc::kVideoRotation_0));
}
void ScreenCapturer::AddOrUpdateSink(
    rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
    const rtc::VideoSinkWants& wants) {
  broadcaster_.AddOrUpdateSink(sink, wants);
}
void ScreenCapturer::RemoveSink(
    rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) {
  broadcaster_.RemoveSink(sink);
}

void ScreenCapturer::CaptureFrame() {
  if (this->screenQueue_ == nullptr) {
    return;
  }

  this->dc->CaptureFrame();

  this->screenQueue_->PostDelayedTask(std::bind(&ScreenCapturer::CaptureFrame, this),
                                      1000 / this->fps_);
}
}  // namespace chai