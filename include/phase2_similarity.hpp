#ifndef DELTADCT_PHASE2_SIMILARITY_HPP
#define DELTADCT_PHASE2_SIMILARITY_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "phase1_dct_codec.hpp"

namespace deltadct {
namespace phase2 {

struct Phase2Config {
    int window_size = 2;
    int num_transforms = 10;
    uint64_t hash_seed = 0x7f4a7c159e3779b9ULL;
};

struct FeatureBitmap {
    int width_in_blocks = 0;
    int height_in_blocks = 0;
    std::vector<uint8_t> bits;
    std::vector<int16_t> dc_values;

    uint8_t Get(int row, int col) const;
    int16_t GetDc(int row, int col) const;
};

struct WindowHash {
    uint64_t value = 0;
    int row = 0;
    int col = 0;
};

struct MinHashFeature {
    uint32_t transform_index = 0;
    uint64_t hash_value = 0;
    uint64_t window_hash = 0;
    int row = 0;
    int col = 0;
};

struct Signature {
    int luma_component_index = -1;
    int window_size = 0;
    int bitmap_width_in_blocks = 0;
    int bitmap_height_in_blocks = 0;
    std::vector<MinHashFeature> features;
};

struct MatchResult {
    std::string best_image_id;
    int best_score = 0;
    std::unordered_map<std::string, int> candidate_scores;
};

bool BuildFeatureBitmap(
    const phase1::JpegDctImage &image,
    FeatureBitmap *out_bitmap,
    int *out_luma_component_index,
    std::string *error_message);

bool ExtractWindowHashes(
    const FeatureBitmap &bitmap,
    int window_size,
    std::vector<WindowHash> *out_hashes,
    std::string *error_message);

bool ComputeNTransformMinHash(
    const std::vector<WindowHash> &window_hashes,
    const Phase2Config &config,
    std::vector<MinHashFeature> *out_features,
    std::string *error_message);

bool BuildSignature(
    const phase1::JpegDctImage &image,
    const Phase2Config &config,
    Signature *out_signature,
    std::string *error_message);

bool DumpSignatureToText(
    const Signature &signature,
    const std::string &output_text_path,
    std::string *error_message);

class InMemoryInvertedIndex {
public:
    void AddImageSignature(const std::string &image_id, const Signature &signature);
    MatchResult QueryBestMatch(
        const Signature &target_signature,
        const std::string &exclude_image_id) const;

private:
    std::unordered_map<uint64_t, std::vector<std::string>> postings_;
};

}  // namespace phase2
}  // namespace deltadct

#endif
