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

#include <GLES2/gl2.h>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>

#include "framesconv.h"
#include "gpu.h"

namespace {

const auto kVertexShaderSource = R"(
attribute vec2 position;

varying vec2 dst_upper_left;

void main() {
  dst_upper_left = position;
  mat4 transform_matrix =
      mat4(vec4(2.0, 0.0, 0.0, 0.0), vec4(0.0, 2.0, 0.0, 0.0),
           vec4(0.0, 0.0, 2.0, 0.0), vec4(-1.0, -1.0, 0.0, 1.0));
  gl_Position = transform_matrix * vec4(position, 0.0, 1.0);
}
//)";

const auto kFragmentShaderSource = R"(
uniform sampler2D img_input;
uniform mediump vec2 img_input_size;

varying mediump vec2 dst_upper_left;

mediump vec3 rgb2yuv(in mediump vec4 rgb) {
  // mburakov: This hardcodes BT.709 full-range.
  mediump float y = rgb.r * 0.2126f + rgb.g * 0.7152f + rgb.b * 0.0722f;
  mediump float u = (rgb.b - y) / (2.f * (1.f - 0.0722f));
  mediump float v = (rgb.r - y) / (2.f * (1.f - 0.2126f));
  return vec3(y, u + 0.5f, v + 0.5f);
}

void main() {
  // mburakov: Upper left corner of 4x1 sampling rect.
  mediump vec2 src_upper_left = vec2(dst_upper_left.x * 4.f, dst_upper_left.y);

  // mburakov: Sampling offsets.
  mediump float pix_width = 1.f / img_input_size.x;
  mediump float pix_height = 1.f / img_input_size.y;
  mediump vec2 src_offset[4];
  src_offset[0] = vec2(0.f, 0.f);
  src_offset[1] = vec2(pix_width, 0.f);
  src_offset[2] = vec2(pix_width * 2.f, 0.f);
  src_offset[3] = vec2(pix_width * 3.f, 0.f);

  // mburakov: Colors of the 4x1 sampling rect.
  mediump vec4 rgb[4];
  rgb[0] = texture2D(img_input, src_upper_left + src_offset[0]);
  rgb[1] = texture2D(img_input, src_upper_left + src_offset[1]);
  rgb[2] = texture2D(img_input, src_upper_left + src_offset[2]);
  rgb[3] = texture2D(img_input, src_upper_left + src_offset[3]);

  // mburakov: Colors after colorspace conversion.
  mediump vec3 yuv[4];
  yuv[0] = rgb2yuv(rgb[0]);
  yuv[1] = rgb2yuv(rgb[1]);
  yuv[2] = rgb2yuv(rgb[2]);
  yuv[3] = rgb2yuv(rgb[3]);

  // mburakov: Writing luma plane with single store.
  // TODO(mburakov): Why such order? Is it little-endian ARGB?
  gl_FragColor = vec4(yuv[2].r, yuv[1].r, yuv[0].r, yuv[3].r);
}
//)";

class FramesconvES20 final : public Framesconv {
 public:
  FramesconvES20();
  ~FramesconvES20() override;

  // Framesconv
  void Convert(GLuint texture_rgbx, std::size_t width, std::size_t height,
               GLuint texture_nv12) const override;

 private:
  GLuint framebuffer_;
  GLuint buffer_object_;
  GLuint program_;
  GLint img_input_size_;
};

FramesconvES20::FramesconvES20() {
  // mburakov: Create framebuffer.
  GLuint framebuffer{};
  glGenFramebuffers(1, &framebuffer);
  if (!framebuffer) {
    throw std::runtime_error(
        WrapGlError("Failed to allocate framebuffer name"));
  }
  Defer deferred_gl_delete_framebuffers([&framebuffer] {
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
  });

  // mburakov: Create and initialize vertex buffer object.
  GLuint buffer_object{};
  glGenBuffers(1, &buffer_object);
  if (!buffer_object) {
    throw std::runtime_error(
        WrapGlError("Failed to allocate buffer object name"));
  }
  Defer deferred_gl_delete_buffers([&buffer_object] {
    if (buffer_object) glDeleteBuffers(1, &buffer_object);
  });
  glBindBuffer(GL_ARRAY_BUFFER, buffer_object);
  static const GLfloat kVertices[] = {0, 0, 1, 0, 1, 1, 0, 1};
  glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  if (GLenum error = glGetError(); error != GL_NO_ERROR)
    throw std::runtime_error(WrapGlError("Failed to initialize vbo", error));

  // mburakov: Colorspace conversion program.
  GLuint program = CreateGlProgram(kVertexShaderSource, kFragmentShaderSource);
  Defer deferred_gl_delete_program([&program] {
    if (program) glDeleteProgram(program);
  });

  // mburakov: Lookup and set input image uniform.
  GLint img_input = glGetUniformLocation(program, "img_input");
  if (img_input == -1)
    throw std::runtime_error(WrapGlError("Failed to get img_input location"));
  glUseProgram(program);
  glUniform1i(img_input, 0);
  glUseProgram(0);
  if (GLenum error = glGetError(); error != GL_NO_ERROR)
    throw std::runtime_error(WrapGlError("Failed to set img_input", error));

  // mburakov: Lookup input image size.
  img_input_size_ = glGetUniformLocation(program, "img_input_size");
  if (img_input_size_ == -1) {
    throw std::runtime_error(
        WrapGlError("Failed to get img_input_size location"));
  }

  // mburakov: So far so good.
  framebuffer_ = std::exchange(framebuffer, 0);
  buffer_object_ = std::exchange(buffer_object, 0);
  program_ = std::exchange(program, 0);
}

FramesconvES20::~FramesconvES20() {
  glDeleteProgram(program_);
  glDeleteBuffers(1, &buffer_object_);
  glDeleteFramebuffers(1, &framebuffer_);
}

void FramesconvES20::Convert(GLuint texture_rgbx, std::size_t width,
                             std::size_t height, GLuint texture_nv12) const {
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         texture_nv12, 0);
  GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
    glBindBuffer(GL_FRAMEBUFFER, 0);
    char message[64];
    std::snprintf(message, sizeof(message),
                  "Framebuffer is incomplete (0x%04x)", framebuffer_status);
    throw std::runtime_error(message);
  }

  glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));

  glUseProgram(program_);
  glUniform2f(img_input_size_, static_cast<GLfloat>(width),
              static_cast<GLfloat>(height));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture_rgbx);
  glBindBuffer(GL_ARRAY_BUFFER, buffer_object_);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  glDisableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (GLenum error = glGetError(); error != GL_NO_ERROR)
    throw std::runtime_error(WrapGlError("Conversion failed", error));
}

}  // namespace

std::unique_ptr<Framesconv> CreateFramesconvES20() {
  return std::make_unique<FramesconvES20>();
}
