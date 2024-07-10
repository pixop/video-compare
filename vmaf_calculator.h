#pragma once
#include <iostream>
#include <stdexcept>
#include <string>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavutil/frame.h>
}

class VMAFCalculator {
 private:
  bool disabled_{false};
  std::string libvmaf_options_;

 public:
  VMAFCalculator(const VMAFCalculator&) = delete;
  VMAFCalculator& operator=(const VMAFCalculator&) = delete;

  static VMAFCalculator& instance();

  void set_libvmaf_options(const std::string& options);

  std::string compute(const AVFrame* distorted_frame, const AVFrame* reference_frame);

 private:
  VMAFCalculator();

  void run_libvmaf_filter(const AVFrame* distorted_frame, const AVFrame* reference_frame);

 private:
  class AVFilterGraphRAII {
   public:
    AVFilterGraphRAII() : graph_(avfilter_graph_alloc()) {
      if (!graph_) {
        throw std::runtime_error("Failed to allocate filter graph");
      }
    }
    ~AVFilterGraphRAII() { avfilter_graph_free(&graph_); }
    AVFilterGraph* get() { return graph_; }

   private:
    AVFilterGraph* graph_;
  };

  class AVFilterInOutRAII {
   public:
    AVFilterInOutRAII(char* name, AVFilterContext* filter_ctx, AVFilterInOut* next, const bool root = true) : inout_(avfilter_inout_alloc()), root_(root) {
      if (!inout_) {
        throw std::runtime_error("Failed to allocate filter inout");
      }

      inout_->name = name;
      inout_->pad_idx = 0;
      inout_->filter_ctx = filter_ctx;
      inout_->next = next;
    }
    ~AVFilterInOutRAII() {
      if (root_) {
        avfilter_inout_free(&inout_);
      }
    }
    AVFilterInOut** get_pointer() { return &inout_; }
    AVFilterInOut* get() { return inout_; }

   private:
    AVFilterInOut* inout_;
    bool root_;
  };

  class AVFrameRAII {
   public:
    AVFrameRAII() : frame_(av_frame_alloc()) {
      if (!frame_) {
        throw std::runtime_error("Failed to allocate frame");
      }
    }
    ~AVFrameRAII() { av_frame_free(&frame_); }
    AVFrame* get() { return frame_; }

   private:
    AVFrame* frame_;
  };
};