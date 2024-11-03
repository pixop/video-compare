#include "vmaf_calculator.h"
#include <iostream>
#include "filtered_logger.h"
#include "string_utils.h"
extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

static const std::string VMAF_SCORE_STRING("VMAF score:");
static const std::regex VMAF_REGEX(VMAF_SCORE_STRING + "\\s(\\d+\\.\\d+)");

VMAFCalculator& VMAFCalculator::instance() {
  static VMAFCalculator instance;

  return instance;
}

VMAFCalculator::VMAFCalculator() {
  FilteredLogger::instance().install(VMAF_SCORE_STRING);
}

void VMAFCalculator::set_libvmaf_options(const std::string& options) {
  libvmaf_options_ = options;
}

std::string VMAFCalculator::compute(const AVFrame* distorted_frame, const AVFrame* reference_frame) {
  std::string result = "n/a";

  if (!disabled_) {
    try {
      FilteredLogger::instance().reset();

      run_libvmaf_filter(distorted_frame, reference_frame);

      std::vector<std::string> vmaf_scores;

      std::string buffered_logs = FilteredLogger::instance().get_buffered_logs();

      std::string::const_iterator search_start(buffered_logs.cbegin());
      std::smatch match;

      while (std::regex_search(search_start, buffered_logs.cend(), match, VMAF_REGEX)) {
        vmaf_scores.push_back(match[1].str());

        search_start = match.suffix().first;
      }

      if (!vmaf_scores.empty()) {
        result = string_join(vmaf_scores, "|");
      } else {
        std::cerr << "Failed to extract at least one VMAF score, disabling VMAF computation." << std::endl;
        disabled_ = true;
      }
    } catch (const std::exception& e) {
      std::cerr << "Failed to run libvmaf FFmpeg filter, disabling VMAF computation." << std::endl;
      disabled_ = true;
    }
  }

  return result;
}

void VMAFCalculator::run_libvmaf_filter(const AVFrame* distorted_frame, const AVFrame* reference_frame) {
  if (!avfilter_get_by_name("libvmaf")) {
    throw std::runtime_error("libvmaf filter not found");
  }

  auto format_filter_args = [](const AVFrame* frame) {
    return
#if (LIBAVFILTER_VERSION_INT < AV_VERSION_INT(10, 1, 100))
        string_sprintf("video_size=%dx%d:pix_fmt=%d:time_base=1/25:pixel_aspect=0/1", frame->width, frame->height, frame->format);
#else
        string_sprintf("video_size=%dx%d:pix_fmt=%d:time_base=1/25:pixel_aspect=0/1:colorspace=%d:range=%d", frame->width, frame->height, frame->format, frame->colorspace, frame->color_range);
#endif
  };

  const AVFilter* buffersrc = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");

  AVFilterGraphRAII filter_graph;
  AVFilterContext* buffersrc_ctx_dist;

  if (avfilter_graph_create_filter(&buffersrc_ctx_dist, buffersrc, "in_dist", format_filter_args(distorted_frame).c_str(), nullptr, filter_graph.get()) < 0) {
    throw std::runtime_error("Cannot create buffer source for distorted frame");
  }

  AVFilterContext* buffersrc_ctx_ref;

  if (avfilter_graph_create_filter(&buffersrc_ctx_ref, buffersrc, "in_ref", format_filter_args(reference_frame).c_str(), nullptr, filter_graph.get()) < 0) {
    throw std::runtime_error("Cannot create buffer source for reference frame");
  }

  AVFilterContext* buffersink_ctx;

  if (avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", nullptr, nullptr, filter_graph.get()) < 0) {
    throw std::runtime_error("Cannot create buffer sink");
  }

  std::string yuv_pixel_format = distorted_frame->format == AV_PIX_FMT_RGB24 ? "yuv444p" : "yuv444p16le";
  std::string libvmaf_filter_options = libvmaf_options_.empty() ? "" : string_sprintf("=%s", libvmaf_options_.c_str());

  std::string filter_description =
      string_sprintf("[in_dist]setparams=colorspace=%d:range=%d,format=%s[in_dist_yuv],[in_ref]setparams=colorspace=%d:range=%d,format=%s[in_ref_yuv],[in_dist_yuv][in_ref_yuv]libvmaf%s[out]", distorted_frame->colorspace,
                     distorted_frame->color_range, yuv_pixel_format.c_str(), reference_frame->colorspace, reference_frame->color_range, yuv_pixel_format.c_str(), libvmaf_filter_options.c_str());

  AVFilterInOutRAII outputs_ref(av_strdup("in_ref"), buffersrc_ctx_ref, nullptr, false);
  AVFilterInOutRAII outputs_dist(av_strdup("in_dist"), buffersrc_ctx_dist, outputs_ref.get());
  AVFilterInOutRAII inputs(av_strdup("out"), buffersink_ctx, nullptr);

  if (avfilter_graph_parse_ptr(filter_graph.get(), filter_description.c_str(), inputs.get_pointer(), outputs_dist.get_pointer(), nullptr) < 0) {
    throw std::runtime_error("Error parsing graph");
  }

  if (avfilter_graph_config(filter_graph.get(), nullptr) < 0) {
    throw std::runtime_error("Error configuring graph");
  }

  if (av_buffersrc_add_frame(buffersrc_ctx_dist, const_cast<AVFrame*>(distorted_frame)) < 0) {
    throw std::runtime_error("Error feeding distorted frame");
  }

  if (av_buffersrc_add_frame(buffersrc_ctx_ref, const_cast<AVFrame*>(reference_frame)) < 0) {
    throw std::runtime_error("Error feeding reference frame");
  }

  if (av_buffersrc_close(buffersrc_ctx_dist, 0, AV_BUFFERSRC_FLAG_PUSH) < 0) {
    throw std::runtime_error("Error closing distorted buffer source");
  }

  if (av_buffersrc_close(buffersrc_ctx_ref, 0, AV_BUFFERSRC_FLAG_PUSH) < 0) {
    throw std::runtime_error("Error closing reference buffer source");
  }

  AVFrameRAII filtered_frame;

  if (av_buffersink_get_frame(buffersink_ctx, filtered_frame.get()) < 0) {
    throw std::runtime_error("Error getting filtered frame");
  }
}
