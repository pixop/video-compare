#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int fail(const std::string& msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return EXIT_FAILURE;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

// Static source audit for SDL 3.4.20 native HDR backends (hardware unverified).
int main() {
  const char* root_env = std::getenv("SDL34_SOURCE");
  if (root_env == nullptr || *root_env == '\0') {
    std::cout << "SKIP: set SDL34_SOURCE to audit an SDL 3.4.20 source checkout\n";
    return EXIT_SUCCESS;
  }
  const std::string root = root_env;

  const std::string d3d11_shader = root + "/src/render/direct3d11/D3D11_PixelShader_Common.hlsli";
  const std::string d3d11_backend = root + "/src/render/direct3d11/SDL_render_d3d11.c";
  const std::string metal_shader = root + "/src/render/metal/SDL_shaders_metal.metal";
  const std::string metal_backend = root + "/src/render/metal/SDL_render_metal.m";

  const std::string d3d11_src = read_file(d3d11_shader);
  if (d3d11_src.empty()) {
    return fail("missing SDL D3D11 shader sources at " + d3d11_shader);
  }
  if (!contains(d3d11_src, "mat2020to709") || !contains(d3d11_src, "PQtoLinear")) {
    return fail("D3D11 HDR shader missing PQ decode or BT.2020->709 matrix");
  }
  if (!contains(d3d11_src, "sdr_white_point")) {
    return fail("D3D11 shader must reference sdr_white_point texture property");
  }

  const std::string d3d11_back_src = read_file(d3d11_backend);
  if (d3d11_back_src.empty() || !contains(d3d11_back_src, "SDL_PIXELFORMAT_RGBA64_FLOAT")) {
    return fail("D3D11 backend must advertise RGBA64_FLOAT textures");
  }

  const std::string metal_src = read_file(metal_shader);
  if (metal_src.empty()) {
    return fail("missing SDL Metal shader sources at " + metal_shader);
  }
  if (!contains(metal_src, "mat2020to709") || !contains(metal_src, "PQtoLinear")) {
    return fail("Metal HDR shader missing PQ decode or BT.2020->709 matrix");
  }

  const std::string metal_back_src = read_file(metal_backend);
  if (metal_back_src.empty() || !contains(metal_back_src, "SDL_PIXELFORMAT_RGBA64_FLOAT")) {
    return fail("Metal backend must advertise RGBA64_FLOAT textures");
  }

  std::cout << "test_hdr_sdl_audit: OK (SDL sources at " << root << "; Win/mac HDR hardware unverified on Linux)\n";
  return EXIT_SUCCESS;
}