#pragma once

#include <string>
#include "core_types.h"
#include "side_aware_logger.h"

class SideAware {
 public:
  SideAware(const Side& side) : side_(side) {}
  virtual ~SideAware() = default;

  Side get_side() const { return side_; }

  void log_info(const std::string& message) const { sa_log_info(side_, message); }
  void log_warning(const std::string& message) const { sa_log_warning(side_, message); }
  void log_error(const std::string& message) const { sa_log_error(side_, message); }

 private:
  const Side side_;
};
