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

#ifndef FRAMESCONV_FRAMESCONV_H_
#define FRAMESCONV_FRAMESCONV_H_

#include <GLES3/gl3.h>

#include <cstddef>
#include <memory>

struct Framesconv {
  virtual void Convert(GLuint texture_rgbx, std::size_t width,
                       std::size_t height, GLuint texture_nv12) const = 0;
  virtual ~Framesconv() = default;
};

std::unique_ptr<Framesconv> CreateFramesconvES31();
std::unique_ptr<Framesconv> CreateFramesconvES20();

#endif  // FRAMESCONV_FRAMESCONV_H_
