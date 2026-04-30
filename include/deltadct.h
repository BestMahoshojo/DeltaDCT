#ifndef DELTADCT_API_HPP
#define DELTADCT_API_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace deltadct {

enum class RoutingMode {
    kAuto = 0,
    kFpmOnly = 1,
    kDCHashOnly = 2,
};

struct CompressOptions {
    RoutingMode mode = RoutingMode::kAuto;
    int window_size = 2;
    int num_features = 10;
    bool strict_lossless = true;
};

struct DedupResult {
    bool success = false;
    double compression_ratio = 0.0;
    std::size_t original_size = 0;
    std::size_t compressed_size = 0;
    std::size_t header_bytes = 0;
    std::size_t copy_meta_bytes = 0;
    std::size_t insert_raw_bytes = 0;
    double time_ms = 0.0;
    double similarity_time_ms = 0.0;
    double delta_time_ms = 0.0;
    double throughput_mb_s = 0.0;
    std::string route_used;
    int blocks_total = 0;
    int blocks_exact = 0;
    int blocks_similar = 0;
    int blocks_different = 0;
    int blocks_width = 0;
    int blocks_height = 0;
    std::vector<uint8_t> block_map;
};

DedupResult compress_image(
    const std::string &target_jpg,
    const std::string &base_jpg,
    const std::string &output_ddct,
    const CompressOptions &options = CompressOptions());

bool decompress_image(
    const std::string &input_ddct,
    const std::string &base_jpg,
    const std::string &output_jpg);

std::vector<uint64_t> extract_features(
    const std::string &img_path,
    int window_size = 2,
    int num_features = 10);

}  // namespace deltadct

#endif