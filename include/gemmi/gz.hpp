// Copyright 2017 Global Phasing Ltd.
//
// Functions for transparent reading of gzipped files. Uses zlib.

#ifndef GEMMI_GZ_HPP_
#define GEMMI_GZ_HPP_
#include "util.hpp"  // ends_with, MaybeStdin
#include <cstdio>    // fseek, ftell, fread
#include <cstring>   // strlen
#include <memory>
#include <string>
#include <zlib.h>

namespace gemmi {

// Throws if the size is not found or if it is suspicious.
// Anything outside of the arbitrary limits from 1 to 10x of the compressed
// size looks suspicious to us.
inline size_t estimate_uncompressed_size(const std::string& path) {
  fileptr_t f = file_open(path.c_str(), "rb");
  if (std::fseek(f.get(), -4, SEEK_END) != 0)
    fail("fseek() failed (empty file?): " + path);
  long pos = std::ftell(f.get());
  if (pos <= 0)
    fail("ftell() failed on " + path);
  size_t gzipped_size = pos + 4;
  unsigned char buf[4];
  if (std::fread(buf, 1, 4, f.get()) != 4)
    fail("Failed to read last 4 bytes of: " + path);
  size_t orig_size = (buf[3] << 24) | (buf[2] << 16) | (buf[1] << 8) | buf[0];
  if (orig_size < gzipped_size || orig_size > 10 * gzipped_size)
    fail("Cannot determine uncompressed size of " + path);
  return orig_size;
}


size_t copy_line_from_stream(char* line, int size, gzFile f) {
  if (!gzgets(f, line, size))
    return 0;
  size_t len = std::strlen(line);
  // If a line is longer than size we discard the rest of it.
  if (len > 0 && line[len-1] != '\n')
    for (int c = gzgetc(f); c != 0 && c != -1 && c != '\n'; c = gzgetc(f))
      continue;
  return len;
}


class MaybeGzipped : public MaybeStdin {
public:
  explicit MaybeGzipped(const std::string& path)
    : MaybeStdin(path), mem_size_(0), file_(nullptr) {}
  ~MaybeGzipped() {
    if (file_)
      gzclose(file_);
  }

  bool is_compressed() const { return ends_with(path(), ".gz"); };
  size_t mem_size() const { return mem_size_; };

  std::unique_ptr<char[]> memory() {
    if (!is_compressed())
      return MaybeStdin::memory();
    mem_size_ = estimate_uncompressed_size(path());
    open();
    if (mem_size_ > 500000000)
      fail("For now gz files above 500MB uncompressed are not supported.");
    std::unique_ptr<char[]> mem(new char[mem_size_]);
    int bytes_read = gzread(file_, mem.get(), mem_size_);
    if (bytes_read < (int) mem_size_ && !gzeof(file_)) {
      int errnum;
      std::string err_str = gzerror(file_, &errnum);
      if (errnum)
        fail("Error reading " + path() + ": " + err_str);
    }
    return mem;
  }

  gzFile prepare_lines() {
    if (!is_compressed())
      return nullptr;
    open();
#if ZLIB_VERNUM >= 0x1235
    gzbuffer(file_, 64*1024);
#endif
    return file_;
  }

private:
  size_t mem_size_;
  gzFile file_;

  void open() {
    file_ = gzopen(path().c_str(), "rb");
    if (!file_)
      fail("Failed to gzopen: " + path());
  }
};

} // namespace gemmi

#endif
// vim:sw=2:ts=2:et
