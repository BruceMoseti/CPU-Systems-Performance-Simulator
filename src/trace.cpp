#include "trace.hpp"

#include <cstring>
#include <stdexcept>

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

int TraceReader::refill() {
  if (pos_ < end_) return buffer_[pos_];
  end_ = std::fread(buffer_.data(), 1, buffer_.size(), file_);
  pos_ = 0;
  if (end_ == 0) return -1;
  return buffer_[pos_];
}

bool TraceReader::next(TraceRecord& record) {
  while (true) {
    int c = refill();
    if (c < 0) return false;

    // Skip blank lines, comments and leading whitespace.
    if (c == '\n') {
      ++pos_;
      ++line_;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      ++pos_;
      continue;
    }
    if (c == '#') {
      while (true) {
        int skip = refill();
        if (skip < 0) return false;
        ++pos_;
        if (skip == '\n') break;
      }
      ++line_;
      continue;
    }

    const char op = static_cast<char>(c);
    ++pos_;
    switch (op) {
      case 'R': record.type = RecordType::Read; break;
      case 'W': record.type = RecordType::Write; break;
      case 'L': record.type = RecordType::DependentRead; break;
      case 'N': record.type = RecordType::Compute; break;
      default:
        throw TraceError(path_ + ":" + std::to_string(line_) + ": unknown record type '" + op +
                         "'");
    }

    while (true) {
      int space = refill();
      if (space == ' ' || space == '\t') {
        ++pos_;
        continue;
      }
      break;
    }

    bool hex = false;
    int first = refill();
    if (first == '0') {
      ++pos_;
      int prefix = refill();
      if (prefix == 'x' || prefix == 'X') {
        ++pos_;
        hex = true;
      }
    } else if (first < 0) {
      throw TraceError(path_ + ":" + std::to_string(line_) + ": record is missing its operand");
    }

    uint64_t value = 0;
    bool any_digits = !hex && first == '0';
    while (true) {
      int digit = refill();
      if (digit < 0) break;
      uint64_t d;
      if (digit >= '0' && digit <= '9') {
        d = static_cast<uint64_t>(digit - '0');
      } else if (hex && digit >= 'a' && digit <= 'f') {
        d = static_cast<uint64_t>(digit - 'a' + 10);
      } else if (hex && digit >= 'A' && digit <= 'F') {
        d = static_cast<uint64_t>(digit - 'A' + 10);
      } else {
        break;
      }
      value = hex ? (value << 4) | d : value * 10 + d;
      any_digits = true;
      ++pos_;
    }
    if (!any_digits) {
      throw TraceError(path_ + ":" + std::to_string(line_) + ": record is missing its operand");
    }

    // Consume the rest of the line.
    while (true) {
      int tail = refill();
      if (tail < 0) break;
      ++pos_;
      if (tail == '\n') {
        ++line_;
        break;
      }
    }

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
