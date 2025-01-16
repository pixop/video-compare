#pragma once

constexpr unsigned UNSET_PEAK_LUMINANCE = 0;

enum Side { NONE = -1, LEFT, RIGHT, Count };
enum ToneMapping { AUTO, OFF, FULLRANGE, RELATIVE };
enum DynamicRange { STANDARD, PQ, HLG };
