#pragma once
#include <array>
#include <stdexcept>
extern "C" {
#include <libavcodec/version.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

const static double AV_TIME_TO_SEC = av_q2d(AV_TIME_BASE_Q);
const static double SEC_TO_AV_TIME = AV_TIME_BASE;
const static double MILLISEC_TO_AV_TIME = SEC_TO_AV_TIME / 1000.0;
const static AVRational AV_R_MICROSECONDS = {1, AV_TIME_BASE};

namespace ffmpeg {
class Error : public std::runtime_error {
 public:
  explicit Error(const std::string& message);
  explicit Error(int status);
  Error(const std::string& file_name, int status);
};

std::string error_string(int error_code);

inline int check(const int status) {
  if (status < 0) {
    throw ffmpeg::Error{status};
  }
  return status;
}

inline int check(const std::string& file_name, const int status) {
  if (status < 0) {
    throw ffmpeg::Error{file_name, status};
  }
  return status;
}

inline float pts_in_secs(const AVFrame* frame) {
  return frame->pts * AV_TIME_TO_SEC;
}

inline float pkt_duration_in_secs(const AVFrame* frame) {
  return frame->pkt_duration * AV_TIME_TO_SEC;
}
}  // namespace ffmpeg
