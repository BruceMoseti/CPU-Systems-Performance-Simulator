#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_util.hpp"
#include "trace.hpp"

using namespace perfsim;

namespace {

std::string temp_path(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

std::vector<TraceRecord> read_all(const std::string& path) {
  TraceReader reader(path);
  std::vector<TraceRecord> records;
  TraceRecord record;
  while (reader.next(record)) records.push_back(record);
  return records;
}

}  // namespace

TEST(trace_round_trips_through_the_writer) {
  const std::string path = temp_path("perfsim_round_trip.trace");
  {
    TraceWriter writer(path, "workload: unit test");
    writer.read(0x1000);
    writer.write(0x2000);
    writer.dependent_read(0xDEADBEEF);
    writer.compute(7);
    writer.compute(0);  // ignored: a zero-instruction record carries no meaning
    writer.close();
  }

  const std::vector<TraceRecord> records = read_all(path);
  CHECK_EQ(records.size(), 4u);
  CHECK(records[0].type == RecordType::Read);
  CHECK_EQ(records[0].value, 0x1000u);
  CHECK(records[1].type == RecordType::Write);
  CHECK_EQ(records[1].value, 0x2000u);
  CHECK(records[2].type == RecordType::DependentRead);
  CHECK_EQ(records[2].value, 0xDEADBEEFu);
  CHECK(records[3].type == RecordType::Compute);
  CHECK_EQ(records[3].value, 7u);
  std::filesystem::remove(path);
}

TEST(trace_reader_skips_comments_and_blank_lines) {
  const std::string path = temp_path("perfsim_comments.trace");
  write_text(path,
             "# perfsim-trace v1\n"
             "# workload: something\n"
             "\n"
             "R 0x40\n"
             "\n"
             "   N 2\n"
             "W 0x80\n"
             "# trailing comment\n");

  const std::vector<TraceRecord> records = read_all(path);
  CHECK_EQ(records.size(), 3u);
  CHECK_EQ(records[0].value, 0x40u);
  CHECK(records[1].type == RecordType::Compute);
  CHECK_EQ(records[1].value, 2u);
  CHECK_EQ(records[2].value, 0x80u);
  std::filesystem::remove(path);
}

TEST(trace_reader_accepts_hex_and_decimal_addresses) {
  const std::string path = temp_path("perfsim_radix.trace");
  write_text(path, "R 0x10\nR 16\nR 0\nR 0X20\nN 0\n");

  const std::vector<TraceRecord> records = read_all(path);
  CHECK_EQ(records.size(), 5u);
  CHECK_EQ(records[0].value, 16u);
  CHECK_EQ(records[1].value, 16u);
  CHECK_EQ(records[2].value, 0u);
  CHECK_EQ(records[3].value, 32u);
  CHECK_EQ(records[4].value, 0u);
  std::filesystem::remove(path);
}

TEST(trace_reader_handles_a_missing_final_newline) {
  const std::string path = temp_path("perfsim_no_newline.trace");
  write_text(path, "R 0x40\nR 0x80");
  const std::vector<TraceRecord> records = read_all(path);
  CHECK_EQ(records.size(), 2u);
  CHECK_EQ(records[1].value, 0x80u);
  std::filesystem::remove(path);
}

TEST(trace_reader_rejects_malformed_records) {
  const std::string bad_type = temp_path("perfsim_bad_type.trace");
  write_text(bad_type, "R 0x40\nX 0x80\n");
  CHECK_THROWS(read_all(bad_type));
  std::filesystem::remove(bad_type);

  const std::string no_operand = temp_path("perfsim_no_operand.trace");
  write_text(no_operand, "R\n");
  CHECK_THROWS(read_all(no_operand));
  std::filesystem::remove(no_operand);

  // A 64-bit address is valid and must survive the parser intact.
  const std::string big_address = temp_path("perfsim_big_address.trace");
  write_text(big_address, "R 0xFFFFFFFFFFFFFF00\n");
  const std::vector<TraceRecord> records = read_all(big_address);
  CHECK_EQ(records.size(), 1u);
  CHECK_EQ(records[0].value, 0xFFFFFFFFFFFFFF00ULL);
  std::filesystem::remove(big_address);

  CHECK_THROWS(TraceReader("/nonexistent/perfsim/trace.txt"));
}

// Traces are large enough that the reader has to work across buffer refills.
TEST(trace_reader_crosses_buffer_boundaries) {
  const std::string path = temp_path("perfsim_large.trace");
  const uint64_t count = 300000;
  {
    TraceWriter writer(path, "workload: buffer boundary");
    for (uint64_t i = 0; i < count; ++i) {
      writer.read(i * 64);
      writer.compute(2);
    }
    writer.close();
  }

  TraceReader reader(path);
  TraceRecord record;
  uint64_t reads = 0;
  uint64_t computes = 0;
  uint64_t last_address = 0;
  while (reader.next(record)) {
    if (record.type == RecordType::Read) {
      last_address = record.value;
      ++reads;
    } else {
      ++computes;
    }
  }
  CHECK_EQ(reads, count);
  CHECK_EQ(computes, count);
  CHECK_EQ(last_address, (count - 1) * 64);
  std::filesystem::remove(path);
}
