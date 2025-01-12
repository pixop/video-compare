#pragma once

constexpr unsigned UNSET_PEAK_LUMINANCE = 0;

constexpr unsigned DEFAULT_SDR_NITS = 100;
constexpr unsigned DEFAULT_HDR_NITS = 500;

enum Side { LEFT, RIGHT, Count };
enum ToneMapping { AUTO, OFF, FULLRANGE, RELATIVE };
enum DynamicRange { STANDARD, PQ, HLG };
