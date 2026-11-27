#include "hdr_metadata.h"
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int fail(const std::string& msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return EXIT_FAILURE;
}

int expect_throw(const std::function<void()>& fn) {
  try {
    fn();
    return fail("expected exception");
  } catch (const std::exception&) {
    return EXIT_SUCCESS;
  }
}

}  // namespace

int main() {
  InputVideo video;
  video.side_description = "Test";

  video.color_trc = "bt2020-10";
  if (expect_throw([&] { hdr_metadata::validate_hdr10_cli_color_overrides(video); }) != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }

  video.color_trc = "smpte2084";
  video.color_space = "bt2020nc";
  video.color_primaries = "bt2020";
  hdr_metadata::validate_hdr10_cli_color_overrides(video);

  hdr_metadata::EffectiveHdr10Tags good;
  good.trc = AVCOL_TRC_SMPTE2084;
  good.primaries = AVCOL_PRI_BT2020;
  good.matrix = AVCOL_SPC_BT2020_NCL;
  good.pixel_format = AV_PIX_FMT_YUV420P10LE;
  hdr_metadata::validate_effective_native_hdr_tags("Left", good);

  hdr_metadata::EffectiveHdr10Tags hlg = good;
  hlg.trc = AVCOL_TRC_ARIB_STD_B67;
  hdr_metadata::validate_effective_native_hdr_tags("Left", hlg);

  hdr_metadata::EffectiveHdr10Tags cl = good;
  cl.matrix = AVCOL_SPC_BT2020_CL;
  if (expect_throw([&] { hdr_metadata::validate_effective_native_hdr_tags("Left", cl); }) != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }

  video.color_space = "bt2020c";
  if (expect_throw([&] { hdr_metadata::validate_hdr10_cli_color_overrides(video); }) != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }

  std::cout << "test_hdr_metadata: OK\n";
  return EXIT_SUCCESS;
}