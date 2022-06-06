/*
 * Copyright (C) 2022 Mikhail Burakov. This file is part of framesconv.
 *
 * framesconv is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * framesconv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with framesconv.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "framesconv.h"
#include "gpu.h"
#include "utils.h"

namespace {

struct Options {
  std::size_t width;
  std::size_t height;
  const char* input;
  const char* output;
  const char* render_node;
  bool es20;
};

Options ParseCommandline(int argc, const char* const argv[]) {
  using namespace std::literals::string_view_literals;
  static const auto& check_size = [](const char* in, int mask) {
    if (!in) throw std::invalid_argument("Missing argument");
    auto value = std::atoi(in);
    if (value <= 0) throw std::invalid_argument("Size must be positive");
    if (value & mask) throw std::invalid_argument("Size must be aligned");
    return static_cast<std::size_t>(value);
  };
  static const auto& check_fname = [](const char* in) {
    return in == "-"sv ? nullptr : in;
  };
  static const auto& check_implementation = [](const char* in) {
    if (in == "31"sv) return false;
    if (in == "20"sv) return true;
    throw std::invalid_argument("Invalid implementation");
  };
  Options result{};
  result.render_node = "/dev/dri/renderD128";
  for (auto it = argv; it < argv + argc; it++) {
    if (*it == "-w"sv)
      result.width = check_size(*++it, 0x7);
    else if (*it == "-h"sv)
      result.height = check_size(*++it, 0x3);
    else if (*it == "-i"sv)
      result.input = check_fname(*++it);
    else if (*it == "-o"sv)
      result.output = check_fname(*++it);
    else if (*it == "-r"sv)
      result.render_node = *++it;
    else if (*it == "-es"sv)
      result.es20 = check_implementation(*++it);
  }
  if (!result.width || !result.height) {
    throw std::runtime_error(
        "Usage: framesconv [-i input] -w width -h height "
        "[-o output] [-r render_node] [-es implementation]");
  }
  return result;
}

}  // namespace

int main(int argc, char* argv[]) try {
  // mburakov: Parse commandline.
  const auto& options = ParseCommandline(argc, argv);

  // mburakov: Create gbm device and buffers.
  GbmDevice device(options.render_node);
  const auto& buffer_rgbx =
      device.CreateGbmBuffer(options.width, options.height);
  if (options.input) {
    std::ifstream source(options.input);
    buffer_rgbx.FillFrom(source);
  } else {
    buffer_rgbx.FillFrom(std::cin);
  }
  const auto& buffer_nv12 =
      device.CreateGbmBuffer(options.width / 4, options.height * 3 / 2);

  // mburakov: Createa and activate surfaceless egl context.
  const auto& context_version =
      options.es20 ? std::make_pair(2, 0) : std::make_pair(3, 1);
  EglContext context(context_version.first, context_version.second);
  context.MakeCurrent();
  Defer deferred_reset_current([&context] { context.ResetCurrent(); });
  EGLDisplay display = context.GetDisplay();

  // mburakov: Create source image
  EGLImage image_rgbx = buffer_rgbx.CreateEglImage(display);
  Defer deferred_egl_destroy_image_rgbx(
      [display, image_rgbx] { eglDestroyImage(display, image_rgbx); });
  GLuint texture_rgbx = CreateGlTexture(GL_TEXTURE_2D, image_rgbx);
  Defer deferred_gl_delete_textures_rgbx(
      [texture_rgbx] { glDeleteTextures(1, &texture_rgbx); });

  // mburakov: Create destination image.
  EGLImage image_nv12 = buffer_nv12.CreateEglImage(display);
  Defer deferred_egl_destroy_image_nv12(
      [display, image_nv12] { eglDestroyImage(display, image_nv12); });
  GLuint texture_nv12 = CreateGlTexture(GL_TEXTURE_2D, image_nv12);
  Defer deferred_gl_delete_textures_nv12(
      [texture_nv12] { glDeleteTextures(1, &texture_nv12); });

  // mburakov: Select framesconv implementation
  const auto& framesconv =
      options.es20 ? CreateFramesconvES20() : CreateFramesconvES31();

  // mburakov: Do the colorspace conversion.
  using namespace std::chrono;
  auto before = steady_clock::now();
  framesconv->Convert(texture_rgbx, options.width, options.height,
                      texture_nv12);
  context.Sync();
  auto after = steady_clock::now();
  auto millis = duration_cast<milliseconds>(after - before);
  std::cerr << "Colorspace conversion took " << millis.count()
            << " milliseconds" << std::endl;

  // mburakov: Drain conversion result.
  if (options.output) {
    std::ofstream stream(options.output);
    buffer_nv12.DrainTo(stream);
  } else {
    buffer_nv12.DrainTo(std::cout);
  }

  return EXIT_SUCCESS;
} catch (const std::exception& ex) {
  std::cerr << ex.what() << std::endl;
  return EXIT_FAILURE;
}
