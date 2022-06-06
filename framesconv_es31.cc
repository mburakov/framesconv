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

#include <GLES3/gl3.h>

#include <cstddef>
#include <memory>
#include <stdexcept>

#include "framesconv.h"
#include "gpu.h"

namespace {

const auto kComputeShaderSource = R"(
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
//)";

class FramesconvES31 final : public Framesconv {
 public:
  FramesconvES31();
  ~FramesconvES31() override;

  // Framesconv
  void Convert(GLuint texture_rgbx, std::size_t width, std::size_t height,
               GLuint texture_nv12) const override;

 private:
  const GLuint program_;
};

FramesconvES31::FramesconvES31()
    : program_{CreateGlProgram(kComputeShaderSource)} {}

FramesconvES31::~FramesconvES31() { glDeleteProgram(program_); }

void FramesconvES31::Convert(GLuint texture_rgbx, std::size_t width,
                             std::size_t height, GLuint texture_nv12) const {
  glUseProgram(program_);
  glBindImageTexture(0, texture_rgbx, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
  glBindImageTexture(1, texture_nv12, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
  glDispatchCompute(static_cast<GLuint>(width / 8),
                    static_cast<GLuint>(height / 4), 1);
  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
  if (GLenum error = glGetError(); error != GL_NO_ERROR)
    throw std::runtime_error(WrapGlError("Failed to dispatch compute", error));
}

}  // namespace

std::unique_ptr<Framesconv> CreateFramesconvES31() {
  return std::make_unique<FramesconvES31>();
}
