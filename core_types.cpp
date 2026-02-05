#include "core_types.h"

// Side class implementation

Side::Side() : type_(SideType::None), right_index_(0) {}

Side::Side(SideType type, size_t right_index) : type_(type), right_index_(right_index) {
  if (type != SideType::Right && right_index != 0) {
    right_index_ = 0;  // Only RIGHT can have non-zero index
  }
}

Side Side::Left() {
  return Side(SideType::Left);
}

Side Side::Right(size_t index) {
  return Side(SideType::Right, index);
}

Side Side::None() {
  return Side(SideType::None);
}

size_t Side::as_index() const {
  if (type_ == SideType::Left)
    return 0;
  if (type_ == SideType::Right)
    return 1;                      // For backward compat with existing arrays
  return static_cast<size_t>(-1);  // NONE
}

size_t Side::as_simple_index() const {
  if (type_ == SideType::Left)
    return 0;
  if (type_ == SideType::Right)
    return 1;
  return 0;  // Default to LEFT for NONE
}

bool Side::operator==(const Side& other) const {
  return type_ == other.type_ && right_index_ == other.right_index_;
}

bool Side::operator!=(const Side& other) const {
  return !(*this == other);
}

bool Side::operator<(const Side& other) const {
  if (type_ != other.type_) {
    return static_cast<int>(type_) < static_cast<int>(other.type_);
  }
  return right_index_ < other.right_index_;
}

size_t Side::hash() const {
  return std::hash<size_t>{}(static_cast<size_t>(type_) * 1000 + right_index_);
}

std::string Side::to_string() const {
  if (type_ == SideType::Left)
    return "LEFT";
  if (type_ == SideType::Right) {
    if (right_index_ == 0)
      return "RIGHT";
    return "RIGHT" + std::to_string(right_index_ + 1);
  }
  return "NONE";
}

// Constants for backward compatibility
const Side LEFT = Side::Left();
const Side RIGHT = Side::Right();
const Side NONE = Side::None();
