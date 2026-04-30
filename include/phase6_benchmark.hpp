#ifndef DELTADCT_PHASE6_BENCHMARK_HPP
#define DELTADCT_PHASE6_BENCHMARK_HPP

#include <cstdint>
#include <string>

#include "phase2_similarity.hpp"
#include "phase3_delta.hpp"

namespace deltadct {
namespace phase6 {

struct BenchmarkOptions {
    std::string base_jpeg_path;
    std::string target_jpeg_path;
    std::string output_dir;
    phase2::Phase2Config phase2_config;
    phase3::Phase3Config phase3_config;
    int zstd_level = 3;
};

struct CodecMetrics {
    bool generated = false;
    std::string package_path;
    uint64_t package_size_bytes = 0;
    double compression_ratio = 0.0;
};

struct BenchmarkResult {
    uint64_t original_size_bytes = 0;
    double similarity_seconds = 0.0;
    double idelta_seconds = 0.0;
    double total_seconds = 0.0;
    double throughput_mb_s = 0.0;
    phase3::MatchRoute route = phase3::MatchRoute::kFpm;

    CodecMetrics raw_metrics;
    CodecMetrics zstd_metrics;

    bool restored_luma_dct_equal = false;
    bool restored_bytewise_equal = false;
    uint64_t restored_reference_hash = 0;
    uint64_t restored_output_hash = 0;
    std::string restored_jpeg_path;
};

bool RunBenchmarkOnPair(
    const BenchmarkOptions &options,
    BenchmarkResult *out_result,
    std::string *error_message);

}  // namespace phase6
}  // namespace deltadct

#endif
