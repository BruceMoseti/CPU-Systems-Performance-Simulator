// Trace format and streaming reader/writer.
//
// A trace is a line-oriented text file describing what the CPU executed:
//
//   # perfsim-trace v1
//   N 3          -- three instructions that touch no memory
//   R 0x1000     -- load
//   W 0x2000     -- store
//   L 0x3000     -- dependent load: this address came out of the previous load,
//                   so the CPU cannot overlap it with later work
//
// The distinction between R and L is what lets the simulator tell a
// latency-bound pointer chase apart from a random walk with the same miss rate.
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace perfsim {

enum class RecordType : uint8_t {
  Read,           // 'R'
  Write,          // 'W'
  DependentRead,  // 'L'
  Compute,        // 'N'
};

struct TraceRecord {
  RecordType type = RecordType::Compute;
  // Address for memory operations, instruction count for Compute.
  uint64_t value = 0;
};

class TraceError : public std::runtime_error {
 public:
  explicit TraceError(const std::string& what) : std::runtime_error(what) {}
};

// Streams records without holding the trace in memory; traces routinely reach
// hundreds of megabytes.
class TraceReader {
 public:
  // path of "-" reads standard input, which allows piping a generator directly
  // into the simulator instead of materialising the trace on disk.
  explicit TraceReader(const std::string& path);
  ~TraceReader();

  TraceReader(const TraceReader&) = delete;
  TraceReader& operator=(const TraceReader&) = delete;

  bool next(TraceRecord& record);

 private:
  // Guarantees that at least `wanted` bytes are buffered unless the file has
  // ended, and returns how many are actually available. Records are short, so
  // one call per record lets the parser work on raw pointers rather than
  // re-checking the buffer on every character.
  size_t ensure(size_t wanted);
  void skip_comment();
  [[noreturn]] void fail(const char* reason) const;

  std::string path_;
  std::FILE* file_ = nullptr;
  bool owns_file_ = false;
  std::string buffer_;
  size_t pos_ = 0;
  size_t end_ = 0;
  bool exhausted_ = false;
  uint64_t line_ = 1;
};

class TraceWriter {
 public:
  explicit TraceWriter(const std::string& path, const std::string& header_comment);
  ~TraceWriter();

  TraceWriter(const TraceWriter&) = delete;
  TraceWriter& operator=(const TraceWriter&) = delete;

  void read(uint64_t address);
  void write(uint64_t address);
  void dependent_read(uint64_t address);
  void compute(uint64_t instructions);
  void close();

 private:
  void emit(char op, uint64_t value);
  void flush();

  std::FILE* file_ = nullptr;
  bool owns_file_ = false;
  std::string buffer_;
};

}  // namespace perfsim
