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
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "gpu.h"
#include "utils.h"

namespace {

const auto kShaderSource = R"(
#version 310 es

precision mediump image2D;

layout (local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
layout (rgba8, binding = 0) uniform restrict readonly image2D img_input;
layout (rgba8, binding = 1) uniform restrict writeonly image2D img_output;

void main(void) {
  ivec2 position = ivec2(gl_GlobalInvocationID.xy);
  vec4 color = imageLoad(img_input, position);
  imageStore(img_output, position, color);
}
)";

struct Options {
  std::size_t width;
  std::size_t height;
  const char* input;
  const char* output;
  const char* render_node;
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
  Options result{};
  result.render_node = "/dev/dri/renderD128";
  for (auto it = argv; it < argv + argc; it++) {
    if (*it == "-w"sv)
      result.width = check_size(*++it, 0xf);
    else if (*it == "-h"sv)
      result.height = check_size(*++it, 0xf);
    else if (*it == "-i"sv)
      result.input = check_fname(*++it);
    else if (*it == "-o"sv)
      result.output = check_fname(*++it);
    else if (*it == "-r"sv)
      result.render_node = *++it;
  }
  if (!result.width || !result.height) {
    throw std::runtime_error(
        "Usage: framesconv [-i input] -w width "
        "-h height [-o output] [-r render_node]");
  }
  return result;
}

}  // namespace

int main(int argc, char* argv[]) try {
  // Parse commandline
  const auto& options = ParseCommandline(argc, argv);

  // Create gbm device and buffers
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
      device.CreateGbmBuffer(options.width, options.height);
  // device.CreateGbmBuffer(options.width / 4, options.height * 3 / 2);

  // Createa and activate surfaceless egl context
  EglContext context;
  context.MakeCurrent();
  Defer deferred_reset_current([&context] { context.ResetCurrent(); });
  EGLDisplay display = context.GetDisplay();

  // Create compute program
  GLuint program = CreateGlProgram(kShaderSource);
  Defer deferred_gl_delete_program([program] { glDeleteProgram(program); });

  // Create source image
  EGLImage image_rgbx = buffer_rgbx.CreateEglImage(display);
  Defer deferred_egl_destroy_image_rgbx(
      [display, image_rgbx] { eglDestroyImage(display, image_rgbx); });
  GLuint texture_rgbx = CreateGlTexture(GL_TEXTURE_2D, image_rgbx);
  Defer deferred_gl_delete_textures_rgbx(
      [texture_rgbx] { glDeleteTextures(1, &texture_rgbx); });

  // Create destination image
  EGLImage image_nv12 = buffer_nv12.CreateEglImage(display);
  Defer deferred_egl_destroy_image_nv12(
      [display, image_nv12] { eglDestroyImage(display, image_nv12); });
  GLuint texture_nv12 = CreateGlTexture(GL_TEXTURE_2D, image_nv12);
  Defer deferred_gl_delete_textures_nv12(
      [texture_nv12] { glDeleteTextures(1, &texture_nv12); });

  using namespace std::chrono;
  auto before = steady_clock::now();

  // Do the colorspace conversion
  glUseProgram(program);
  glBindImageTexture(0, texture_rgbx, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
  glBindImageTexture(1, texture_nv12, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
  glDispatchCompute(static_cast<GLuint>(options.width / 16),
                    static_cast<GLuint>(options.height / 16), 1);
  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
  if (GLenum error = glGetError(); error != GL_NO_ERROR)
    throw std::runtime_error(WrapGlError("Failed to dispatch compute", error));
  context.Sync();

  auto after = steady_clock::now();
  auto millis = duration_cast<milliseconds>(after - before);
  std::cerr << "Colorspace conversion took " << millis.count()
            << " milliseconds" << std::endl;

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
