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

#ifndef FRAMESCONV_UTILS_H_
#define FRAMESCONV_UTILS_H_

struct FdCloser {
  class pointer {
   public:
    pointer(int fd = -1) : fd_{fd} {}
    pointer(decltype(nullptr)) {}
    operator int() const { return fd_; }
    bool operator!=(const pointer& other) const { return fd_ != other.fd_; }

   private:
    int fd_{-1};
  };

  void operator()(const pointer& ptr) const noexcept;
};

template <class T>
class Defer {
 public:
  Defer(T&& impl) : impl_{impl} {}
  ~Defer() { impl_(); }

 private:
  T impl_;
};

template <class T>
Defer(T&&) -> Defer<T>;

#endif  // FRAMESCONV_UTILS_H_
