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

#include "gpu.h"

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <sys/mman.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

void VerifyExtension(const std::string_view& haystack, const char* needle) {
  if (haystack.find(needle) == std::string_view::npos) {
    using namespace std::literals::string_literals;
    const auto& message =
        "Required extenstion "s + needle + " is not supported"s;
    throw std::runtime_error(message);
  }
}

const std::pair<EGLint, const char*> kEglErrors[] = {
    {EGL_SUCCESS, "EGL_SUCCESS"},
    {EGL_NOT_INITIALIZED, "EGL_NOT_INITIALIZED"},
    {EGL_BAD_ACCESS, "EGL_BAD_ACCESS"},
    {EGL_BAD_ALLOC, "EGL_BAD_ALLOC"},
    {EGL_BAD_ATTRIBUTE, "EGL_BAD_ATTRIBUTE"},
    {EGL_BAD_CONFIG, "EGL_BAD_CONFIG"},
    {EGL_BAD_CONTEXT, "EGL_BAD_CONTEXT"},
    {EGL_BAD_CURRENT_SURFACE, "EGL_BAD_CURRENT_SURFACE"},
    {EGL_BAD_DISPLAY, "EGL_BAD_DISPLAY"},
    {EGL_BAD_MATCH, "EGL_BAD_MATCH"},
    {EGL_BAD_NATIVE_PIXMAP, "EGL_BAD_NATIVE_PIXMAP"},
    {EGL_BAD_NATIVE_WINDOW, "EGL_BAD_NATIVE_WINDOW"},
    {EGL_BAD_PARAMETER, "EGL_BAD_PARAMETER"},
    {EGL_BAD_SURFACE, "EGL_BAD_SURFACE"},
    {EGL_CONTEXT_LOST, "EGL_CONTEXT_LOST"}};

const std::pair<GLenum, const char*> kGlErrors[] = {
    {GL_NO_ERROR, "GL_NO_ERROR"},
    {GL_INVALID_ENUM, "GL_INVALID_ENUM"},
    {GL_INVALID_VALUE, "GL_INVALID_VALUE"},
    {GL_INVALID_OPERATION, "GL_INVALID_OPERATION"},
    {GL_OUT_OF_MEMORY, "GL_OUT_OF_MEMORY"},
    {GL_INVALID_FRAMEBUFFER_OPERATION, "GL_INVALID_FRAMEBUFFER_OPERATION"},
};

template <class K, auto S>
const char* LookupError(const std::pair<K, const char*> (&list)[S], K key) {
  for (const auto& it : list) {
    if (it.first == key) return it.second;
  }
  return "???";
}

struct GlShaderTraits {
  static constexpr const auto message = "Failed to compile shader";
  static constexpr const auto getter = &glGetShaderiv;
  static constexpr const auto status = GL_COMPILE_STATUS;
  static constexpr const auto logger = &glGetShaderInfoLog;
};

struct GlProgramTraits {
  static constexpr const auto message = "Failed to link program";
  static constexpr const auto getter = &glGetProgramiv;
  static constexpr const auto status = GL_LINK_STATUS;
  static constexpr const auto logger = &glGetProgramInfoLog;
};

template <class T>
void CheckBuildable(GLuint buildable) {
  if (GLenum error = glGetError(); error != GL_NO_ERROR)
    throw std::runtime_error(WrapGlError(T::message, error));
  GLint status{};
  (T::getter)(buildable, T::status, &status);
  if (status != GL_TRUE) {
    GLint log_length{};
    (T::getter)(buildable, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<std::size_t>(log_length), 0);
    (T::logger)(buildable, log_length, nullptr, log.data());
    throw std::runtime_error(log);
  }
}

}  // namespace

GbmBuffer::GbmBuffer(gbm_device* device, std::size_t width, std::size_t height)
    : width_{width}, height_{height} {
  bo_.reset(gbm_bo_create(
      device, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
      static_cast<uint32_t>(GBM_BO_FORMAT_ARGB8888), GBM_BO_USE_LINEAR));
  if (!bo_) {
    throw std::system_error(errno, std::system_category(),
                            "Failed to create gbm buffer object");
  }
  fd_.reset(gbm_bo_get_fd(bo_.get()));
  if (!fd_) {
    throw std::system_error(errno, std::system_category(),
                            "Failed to get gbm buffer object fd");
  }
}

void GbmBuffer::FillFrom(std::istream& stream) const {
  std::size_t size = width_ * height_ * 4;
  void* data = mmap(nullptr, size, PROT_WRITE, MAP_SHARED, fd_.get(), 0);
  if (data == MAP_FAILED) {
    throw std::system_error(errno, std::system_category(),
                            "Failed to mmap gbm buffer object fd");
  }
  Defer deferred_munmap([data, size] { munmap(data, size); });
  stream.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
  if (!stream) throw std::runtime_error("Failed to read source");
}

void GbmBuffer::DrainTo(std::ostream& stream) const {
  std::size_t size = width_ * height_ * 4;
  void* data = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd_.get(), 0);
  if (data == MAP_FAILED) {
    throw std::system_error(errno, std::system_category(),
                            "Failed to mmap gbm buffer object fd");
  }
  Defer deferred_munmap([data, size] { munmap(data, size); });
  stream.write(static_cast<char*>(data), static_cast<std::streamsize>(size));
  if (!stream) throw std::runtime_error("Failed to write target");
}

EGLImage GbmBuffer::CreateEglImage(EGLDisplay display) const {
  const EGLAttrib attrib_list[] = {
#define _(...) __VA_ARGS__
      _(EGL_WIDTH, static_cast<EGLAttrib>(width_)),
      _(EGL_HEIGHT, static_cast<EGLAttrib>(height_)),
      _(EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888),
      _(EGL_DMA_BUF_PLANE0_FD_EXT, static_cast<EGLAttrib>(fd_.get())),
      _(EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0),
      _(EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLAttrib>(width_ * 4)),
      EGL_NONE,
#undef _
  };
  EGLImage result = eglCreateImage(display, EGL_NO_CONFIG_KHR,
                                   EGL_LINUX_DMA_BUF_EXT, nullptr, attrib_list);
  if (result == EGL_NO_IMAGE)
    throw std::runtime_error(WrapEglError("Failed to create egl image"));
  return result;
}

GbmDevice::GbmDevice(const char* render_node) {
  fd_.reset(open(render_node, O_RDWR));
  if (!fd_) {
    throw std::system_error(errno, std::system_category(),
                            "Failed to open redner node");
  }
  device_.reset(gbm_create_device(fd_.get()));
  if (!device_) {
    throw std::system_error(errno, std::system_category(),
                            "Failed to create gbm device");
  }
}

GbmBuffer GbmDevice::CreateGbmBuffer(std::size_t width,
                                     std::size_t height) const {
  return {device_.get(), width, height};
}

EglContext::EglContext() {
  const char* egl_ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  if (!egl_ext) {
    throw std::runtime_error(
        WrapEglError("Failed to query platformless egl extensions"));
  }
  VerifyExtension(egl_ext, "EGL_MESA_platform_surfaceless");
  EGLDisplay display =
      eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
  if (display == EGL_NO_DISPLAY)
    throw std::runtime_error(WrapEglError("Failed to get platform display"));
  Defer deferred_egl_terminate([&display] {
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
  });

  if (!eglInitialize(display, nullptr, nullptr))
    throw std::runtime_error(WrapEglError("Failed to initialize egl display"));
  egl_ext = eglQueryString(display, EGL_EXTENSIONS);
  if (!egl_ext) {
    throw std::runtime_error(
        WrapEglError("Failed to query platform egl extensions"));
  }
  VerifyExtension(egl_ext, "EGL_KHR_surfaceless_context");
  VerifyExtension(egl_ext, "EGL_KHR_no_config_context");
  VerifyExtension(egl_ext, "EGL_EXT_image_dma_buf_import");

  if (!eglBindAPI(EGL_OPENGL_ES_API))
    throw std::runtime_error(WrapEglError("Failed to bind egl api"));
  static const EGLint kContextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                           EGL_NONE};
  context_ = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
                              kContextAttribs);
  if (context_ == EGL_NO_CONTEXT)
    throw std::runtime_error(WrapEglError("Failed to create egl context"));
  display_ = std::exchange(display, EGL_NO_DISPLAY);
}

EglContext::~EglContext() {
  eglDestroyContext(display_, context_);
  eglTerminate(display_);
}

void EglContext::MakeCurrent() const {
  if (!eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_)) {
    throw std::runtime_error(
        WrapEglError("Failed to make EGL context current"));
  }
}

void EglContext::ResetCurrent() const {
  if (!eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      EGL_NO_CONTEXT)) {
    throw std::runtime_error(
        WrapEglError("Failed to reset current EGL context"));
  }
}

void EglContext::Sync() const {
  EGLSync sync = eglCreateSync(display_, EGL_SYNC_FENCE, nullptr);
  if (sync == EGL_NO_SYNC)
    throw std::runtime_error(WrapEglError("Failed to create egl fence sync"));
  eglClientWaitSync(display_, sync, 0, EGL_FOREVER);
  eglDestroySync(display_, sync);
}

std::string WrapEglError(const std::string& message, EGLint error) {
  return message + ": " + LookupError(kEglErrors, error);
}

std::string WrapGlError(const std::string& message, GLenum error) {
  return message + ": " + LookupError(kGlErrors, error);
}

GLuint CreateGlTexture(GLenum target, EGLImage image) {
  static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES{};
  if (glEGLImageTargetTexture2DOES == nullptr) {
    const GLubyte* gl_ext = glGetString(GL_EXTENSIONS);
    if (!gl_ext) throw std::runtime_error("Failed to get gl extensions");
    VerifyExtension(reinterpret_cast<const char*>(gl_ext), "GL_OES_EGL_image");
    glEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!glEGLImageTargetTexture2DOES)
      throw std::runtime_error("Failed to import glEGLImageTargetTexture2DOES");
  }

  GLuint result{};
  glGenTextures(1, &result);
  glBindTexture(target, result);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glEGLImageTargetTexture2DOES(target, image);
  glBindTexture(target, 0);

  if (GLenum error = glGetError(); error != GL_NO_ERROR) {
    glDeleteTextures(1, &result);
    throw std::runtime_error(
        WrapGlError("Failed to create image texture", error));
  }
  return result;
}

GLuint CreateGlProgram(const char* source) {
  GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
  if (!shader) throw std::runtime_error(WrapGlError("Failed to create shader"));
  Defer deferred_gl_delete_shader([shader] { glDeleteShader(shader); });
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  CheckBuildable<GlShaderTraits>(shader);

  GLuint program = glCreateProgram();
  if (!program)
    throw std::runtime_error(WrapGlError("Failed to create program"));
  Defer deferred_gl_delete_program([&program] {
    if (program) glDeleteProgram(program);
  });
  glAttachShader(program, shader);
  glLinkProgram(program);
  CheckBuildable<GlProgramTraits>(program);
  return std::exchange(program, 0);
}
