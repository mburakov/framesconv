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

#ifndef FRAMESCONV_GPU_H_
#define FRAMESCONV_GPU_H_

#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <gbm.h>

#include <cstddef>
#include <istream>
#include <memory>
#include <ostream>
#include <string>

#include "utils.h"

class GbmBuffer {
 public:
  GbmBuffer(gbm_device* device, std::size_t width, std::size_t height);

  void FillFrom(std::istream& stream) const;
  void DrainTo(std::ostream& stream) const;
  EGLImage CreateEglImage(EGLDisplay display) const;

 private:
  std::size_t width_{};
  std::size_t height_{};
  std::unique_ptr<gbm_bo, decltype(&gbm_bo_destroy)> bo_{nullptr,
                                                         &gbm_bo_destroy};
  std::unique_ptr<std::nullptr_t, FdCloser> fd_;
};

class GbmDevice {
 public:
  explicit GbmDevice(const char* render_node);

  GbmBuffer CreateGbmBuffer(std::size_t width, std::size_t height) const;

 private:
  std::unique_ptr<std::nullptr_t, FdCloser> fd_;
  std::unique_ptr<gbm_device, decltype(&gbm_device_destroy)> device_{
      nullptr, &gbm_device_destroy};
};

class EglContext {
 public:
  EglContext();
  ~EglContext();

  EglContext(const EglContext&) = delete;
  EglContext(EglContext&& other) = delete;
  EglContext& operator=(const EglContext&) = delete;
  EglContext& operator=(EglContext&& other) = delete;

  EGLDisplay GetDisplay() const { return display_; }
  void MakeCurrent() const;
  void ResetCurrent() const;
  void Sync() const;

 private:
  EGLDisplay display_;
  EGLContext context_;
};

std::string WrapEglError(const std::string& message,
                         EGLint error = eglGetError());
std::string WrapGlError(const std::string& message,
                        GLenum error = glGetError());
GLuint CreateGlTexture(GLenum target, EGLImage image);
GLuint CreateGlProgram(const char* source);

#endif  // FRAMESCONV_GPU_H_
