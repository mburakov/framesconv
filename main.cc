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

/**
 * mburakov: Following data layouts allow to sample and store data in 4-byte
 * groups. This allows to write nv12 data even though the underlying storage is
 * accessed as rgba.
 *
 *
 *     interleaved rgb plane
 *       4 bytes per pixel
 * +------+------++------+------+
 * | rgb0 | rgb1 || rgb2 | rgb3 | even line
 * +------+------++------+------+
 * | rgb4 | rgb5 || rgb6 | rgb7 | odd line
 * +------+------++------+------+
 *    left rect     right rect
 *
 *
 *   planar luma plane
 *    1 byte per pixel
 * +----+----++----+----+
 * | y0 | y1 || y2 | y3 | even line
 * +----+----++----+----+
 * | y4 | y5 || y6 | y7 | odd line
 * +----+----++----+----+
 *  left rect right rect
 *
 *
 * interleaved chroma plane
 *    2 bytes per pixel
 * +----------++----------+
 * |  uv0145  ||  uv2367  |
 * +----------++----------+
 *  left rect   right rect
 */

precision mediump image2D;

// mburakov: On *my* hardware workgroup size of 4 (2x2) provides the best
// performance for this particular compute shader. Note, that's it's unrelated
// to 4:2:0 chroma subsampling or any layouts mentioned above. When changing
// workgroup size, don't forget to update all the arguments of glDispatchCompute
// accordingly, and change the alignment checks in ParseCommandline.

layout(local_size_x = 2, local_size_y = 2) in;
layout(rgba8, binding = 0) uniform restrict readonly image2D img_input;
layout(rgba8, binding = 1) uniform restrict writeonly image2D img_output;

vec3 rgb2yuv(in vec4 rgb) {
  // mburakov: This hardcodes BT.709 full-range.
  float y = rgb.r * 0.2126f + rgb.g * 0.7152f + rgb.b * 0.0722f;
  float u = (rgb.b - y) / (2.f * (1.f - 0.0722f));
  float v = (rgb.r - y) / (2.f * (1.f - 0.2126f));
  return vec3(y, u + 0.5f, v + 0.5f);
}

void main(void) {
  // mburakov: Upper left corner of 4x2 sampling rect.
  ivec2 src_upper_left =
      ivec2(gl_GlobalInvocationID.x * 4u, gl_GlobalInvocationID.y * 2u);

  // mburakov: Sampling offsets.
  ivec2 src_offset[8] =
      ivec2[8](ivec2(0, 0), ivec2(1, 0), ivec2(2, 0), ivec2(3, 0), ivec2(0, 1),
               ivec2(1, 1), ivec2(2, 1), ivec2(3, 1));

  // mburakov: Colors of the 4x2 sampling rect.
  vec4 rgb[8] = vec4[8](imageLoad(img_input, src_upper_left + src_offset[0]),
                        imageLoad(img_input, src_upper_left + src_offset[1]),
                        imageLoad(img_input, src_upper_left + src_offset[2]),
                        imageLoad(img_input, src_upper_left + src_offset[3]),
                        imageLoad(img_input, src_upper_left + src_offset[4]),
                        imageLoad(img_input, src_upper_left + src_offset[5]),
                        imageLoad(img_input, src_upper_left + src_offset[6]),
                        imageLoad(img_input, src_upper_left + src_offset[7]));

  // mburakov: Colors after colorspace conversion.
  vec3 yuv[8] = vec3[8](rgb2yuv(rgb[0]), rgb2yuv(rgb[1]), rgb2yuv(rgb[2]),
                        rgb2yuv(rgb[3]), rgb2yuv(rgb[4]), rgb2yuv(rgb[5]),
                        rgb2yuv(rgb[6]), rgb2yuv(rgb[7]));

  // mburakov: Upper left corner of 4x2 storing rect for luma.
  ivec2 dst_upper_left_luma =
      ivec2(gl_GlobalInvocationID.x, gl_GlobalInvocationID.y * 2u);

  // mburakov: Writing luma plane with two stores.
  imageStore(img_output, dst_upper_left_luma + ivec2(0, 0),
             vec4(yuv[0].r, yuv[1].r, yuv[2].r, yuv[3].r));
  imageStore(img_output, dst_upper_left_luma + ivec2(0, 1),
             vec4(yuv[4].r, yuv[5].r, yuv[6].r, yuv[7].r));

  // mburakov: Upper left corner of 2x1 storing rect for chroma.
  ivec2 img_input_size = imageSize(img_input);
  ivec2 dst_upper_left_chroma = ivec2(
      gl_GlobalInvocationID.x, int(gl_GlobalInvocationID.y) + img_input_size.y);

  // mburakov: Writing chroma plane with single store.
  imageStore(img_output, dst_upper_left_chroma,
             vec4((yuv[0].gb + yuv[1].gb + yuv[4].gb + yuv[5].gb) / 4.f,
                  (yuv[2].gb + yuv[3].gb + yuv[6].gb + yuv[7].gb) / 4.f));
}
)";

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
    if (in == "es31"sv) return false;
    if (in == "es20"sv) return true;
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
    else if (*it == "-i"sv)
      result.es20 = check_implementation(*++it);
  }
  if (!result.width || !result.height) {
    throw std::runtime_error(
        "Usage: framesconv [-i input] -w width -h height "
        "[-o output] [-r render_node] [-i implementation]");
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

  // mburakov: Create compute program.
  GLuint program = CreateGlProgram(kShaderSource);
  Defer deferred_gl_delete_program([program] { glDeleteProgram(program); });

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

  using namespace std::chrono;
  auto before = steady_clock::now();

  // mburakov: Do the colorspace conversion.
  glUseProgram(program);
  glBindImageTexture(0, texture_rgbx, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
  glBindImageTexture(1, texture_nv12, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
  glDispatchCompute(static_cast<GLuint>(options.width / 8),
                    static_cast<GLuint>(options.height / 4), 1);
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
