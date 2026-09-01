#include "trace.hpp"

#include <cstring>

namespace perfsim {
namespace {

constexpr size_t kBufferSize = 1 << 20;

void append_hex(std::string& out, uint64_t value) {
  char digits[16];
  int count = 0;
  do {
    digits[count++] = "0123456789abcdef"[value & 0xF];
    value >>= 4;
  } while (value != 0);
  out += "0x";
  while (count > 0) out.push_back(digits[--count]);
}

void append_decimal(std::string& out, uint64_t value) {
  char digits[20];
  int count = 0;
  do {
    digits[count++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0);
  while (count > 0) out.push_back(digits[--count]);
}

}  // namespace

TraceReader::TraceReader(const std::string& path) : path_(path), buffer_(kBufferSize, '\0') {
  if (path == "-") {
    file_ = stdin;
    owns_file_ = false;
  } else {
    file_ = std::fopen(path.c_str(), "rb");
    owns_file_ = true;
    if (file_ == nullptr) throw TraceError("cannot open trace file: " + path);
  }
}

TraceReader::~TraceReader() {
  if (owns_file_ && file_ != nullptr) std::fclose(file_);
}

void TraceReader::fail(const char* reason) const {
  throw TraceError(path_ + ":" + std::to_string(line_) + ": " + reason);
}

size_t TraceReader::ensure(size_t wanted) {
  if (end_ - pos_ >= wanted || exhausted_) return end_ - pos_;

  const size_t remaining = end_ - pos_;
  if (remaining > 0) std::memmove(buffer_.data(), buffer_.data() + pos_, remaining);
  pos_ = 0;
  end_ = remaining;
  // Loops because a pipe can return a short read even when more is coming.
  while (end_ < wanted) {
    const size_t got = std::fread(buffer_.data() + end_, 1, buffer_.size() - end_, file_);
    if (got == 0) {
      exhausted_ = true;
      break;
    }
    end_ += got;
  }
  return end_ - pos_;
}

void TraceReader::skip_comment() {
  while (ensure(1) > 0) {
    const char* const begin = buffer_.data();
    const char* p = begin + pos_;
    const char* const limit = begin + end_;
    while (p < limit && *p != '\n') ++p;
    if (p < limit) {
      ++p;
      ++line_;
      pos_ = static_cast<size_t>(p - begin);
      return;
    }
    pos_ = end_;
  }
}

bool TraceReader::next(TraceRecord& record) {
  // Longer than any valid record, so buffering this much means a whole record is
  // present unless the file really has ended there.
  constexpr size_t kMaxRecordBytes = 40;

  while (true) {
    if (ensure(kMaxRecordBytes) == 0) return false;
    const char* const begin = buffer_.data();
    const char* const limit = begin + end_;
    const char* p = begin + pos_;

    while (p < limit) {
      const char c = *p;
      if (c == '\n') {
        ++line_;
      } else if (c != ' ' && c != '\t' && c != '\r') {
        break;
      }
      ++p;
    }
    pos_ = static_cast<size_t>(p - begin);
    if (p == limit) continue;  // only whitespace so far: refill or finish

    if (*p == '#') {
      skip_comment();
      continue;
    }

    RecordType type;
    switch (*p) {
      case 'R': type = RecordType::Read; break;
      case 'W': type = RecordType::Write; break;
      case 'L': type = RecordType::DependentRead; break;
      case 'N': type = RecordType::Compute; break;
      default: fail("unknown record type");
    }
    ++p;
    while (p < limit && (*p == ' ' || *p == '\t')) ++p;

    bool hex = false;
    if (limit - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
      hex = true;
      p += 2;
    }

    uint64_t value = 0;
    const char* const digits = p;
    if (hex) {
      while (p < limit) {
        const char c = *p;
        uint64_t digit;
        if (c >= '0' && c <= '9') {
          digit = static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
          digit = static_cast<uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
          digit = static_cast<uint64_t>(c - 'A' + 10);
        } else {
          break;
        }
        value = (value << 4) | digit;
        ++p;
      }
    } else {
      while (p < limit && *p >= '0' && *p <= '9') {
        value = value * 10 + static_cast<uint64_t>(*p - '0');
        ++p;
      }
    }
    if (p == digits) fail("record is missing its operand");

    while (p < limit && *p != '\n') ++p;
    if (p < limit) {
      ++p;
      ++line_;
    }
    pos_ = static_cast<size_t>(p - begin);

    record.type = type;
    record.value = value;
    return true;
  }
}

TraceWriter::TraceWriter(const std::string& path, const std::string& header_comment) {
  if (path == "-") {
    file_ = stdout;
    owns_file_ = false;
  } else {
    file_ = std::fopen(path.c_str(), "wb");
    owns_file_ = true;
    if (file_ == nullptr) throw TraceError("cannot open trace file for writing: " + path);
  }
  buffer_.reserve(kBufferSize + 64);
  buffer_ += "# perfsim-trace v1\n";
  if (!header_comment.empty()) {
    buffer_ += "# ";
    buffer_ += header_comment;
    buffer_.push_back('\n');
  }
}

TraceWriter::~TraceWriter() { close(); }

void TraceWriter::emit(char op, uint64_t value) {
  buffer_.push_back(op);
  buffer_.push_back(' ');
  if (op == 'N') {
    append_decimal(buffer_, value);
  } else {
    append_hex(buffer_, value);
  }
  buffer_.push_back('\n');
  if (buffer_.size() >= kBufferSize) flush();
}

void TraceWriter::read(uint64_t address) { emit('R', address); }
void TraceWriter::write(uint64_t address) { emit('W', address); }
void TraceWriter::dependent_read(uint64_t address) { emit('L', address); }

void TraceWriter::compute(uint64_t instructions) {
  if (instructions == 0) return;
  emit('N', instructions);
}

void TraceWriter::flush() {
  if (buffer_.empty() || file_ == nullptr) return;
  if (std::fwrite(buffer_.data(), 1, buffer_.size(), file_) != buffer_.size()) {
    throw TraceError("failed to write trace data");
  }
  buffer_.clear();
}

void TraceWriter::close() {
  if (file_ == nullptr) return;
  flush();
  if (owns_file_) {
    std::fclose(file_);
  } else {
    std::fflush(file_);
  }
  file_ = nullptr;
}

}  // namespace perfsim
