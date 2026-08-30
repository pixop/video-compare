#pragma once
#include <string>
#include <utility>
#include <vector>

struct ControlEntry {
  std::string key;
  std::string description;
};

struct ControlSection {
  std::string title;
  std::vector<ControlEntry> entries;
};

const std::vector<ControlSection>& get_control_sections();
