#include "phase2_similarity.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace deltadct {
namespace phase2 {

namespace {

struct LinearHashParam {
    uint64_t a = 0;
    uint64_t b = 0;
};

constexpr uint64_t kDefaultA[] = {
    0x76931fac9dab2b36ULL,
    0xc248b87d6ae33f9aULL,
    0x62d7183a5d5789e4ULL,
    0xb2d6b441e2411dc7ULL,
    0x09e111c7e1e7acb6ULL,
    0xf8cac0bb2fc4c8bcULL,
    0x2ae3baaab9165cc4ULL,
    0x58e199cb89f51b13ULL,
    0x5f7091a5abb0874dULL,
    0xf3e8cb4543a5eb93ULL,
    0xb0441e9ca4c2b0fbULL,
    0x3d30875cbf29abd5ULL,
    0xb1acf38984b35ae8ULL,
    0x82809dd4cfe7abc5ULL,
    0xc61baa52e053b4c3ULL,
    0x643f204ef259d2e9ULL,
    0x8042a948aac5e884ULL,
    0xcb3ec7db925643fdULL,
    0x34fdd467e2cca406ULL,
    0x035cb2744cb90a63ULL,
    0xe51c973790334394ULL,
    0x7e02086541e4c48aULL,
    0x99630aa9aece1538ULL,
    0x43a4b190274ebc95ULL,
    0x5f8592e30a2205a4ULL,
    0x85846248987550aaULL,
    0xf2094ec59e7931dcULL,
    0x650c7451cc61c0cbULL,
    0x2c46a1b3f2c349faULL,
    0xff763c7f8d14ddffULL,
    0x946351744378d62cULL,
    0x59285a8d7915614fULL,
};

constexpr uint64_t kDefaultB[] = {
    0x38667b6ed2b2fcabULL,
    0x04abae8676e318b4ULL,
    0x02a7d15b30d2d7ddULL,
    0xb78650cc6af82bc3ULL,
    0xd7aa805b02dd9aa5ULL,
    0x23b7374a1323ee6bULL,
    0x516d1b81e5f709c2ULL,
    0xc790edaf1c3fa9b0ULL,
    0xa1dbc6dabc2b5ed2ULL,
    0x67244c458752002bULL,
    0x106d6381fad58a7eULL,
    0x193657bde0fe0291ULL,
    0x20f8379316891f82ULL,
    0x8b8d24a049e5b86dULL,
    0x855bcfed56765f9dULL,
    0xa1ac54caeaf9257aULL,
    0xbc67b451bc70b0e5ULL,
    0x2817dd1b704a6b41ULL,
    0x8a83fd4a9ca4c89eULL,
    0x1a6e779f8d9e9df1ULL,
    0x8747591e5b314c05ULL,
    0x763edcd59632423cULL,
    0xa83f14d6f073d784ULL,
    0xdb2b7001643a6760ULL,
    0xf9f0dd6ddd0a59e2ULL,
    0x41dc1ed720287896ULL,
    0x286f5cc3addf6c1aULL,
    0xdf6ed35f477b0022ULL,
    0x981e5e1fbfe1bfb8ULL,
    0xe26b5ba93253275bULL,
    0xf6a44b3fa1051cdfULL,
    0xe3b3f5d2725a9a58ULL,
};

void SetError(std::string *error_message, const std::string &msg) {
    if (error_message != nullptr) {
        *error_message = msg;
    }
}

uint64_t SplitMix64(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

uint64_t MixHash(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

std::vector<LinearHashParam> BuildLinearHashParams(int count, uint64_t seed) {
    std::vector<LinearHashParam> params;
    params.reserve(static_cast<size_t>(count));

    const int default_count = static_cast<int>(sizeof(kDefaultA) / sizeof(kDefaultA[0]));
    const int use_default = std::min(count, default_count);
    for (int i = 0; i < use_default; ++i) {
        params.push_back({kDefaultA[i], kDefaultB[i]});
    }

    uint64_t state = seed;
    for (int i = use_default; i < count; ++i) {
        uint64_t a = SplitMix64(&state);
        if ((a & 1ULL) == 0ULL) {
            ++a;
        }
        uint64_t b = SplitMix64(&state);
        params.push_back({a, b});
    }

    return params;
}

}  // namespace

uint8_t FeatureBitmap::Get(int row, int col) const {
    if (row < 0 || col < 0 || row >= height_in_blocks || col >= width_in_blocks) {
        return 0;
    }
    return bits[static_cast<size_t>(row * width_in_blocks + col)];
}

int16_t FeatureBitmap::GetDc(int row, int col) const {
    if (row < 0 || col < 0 || row >= height_in_blocks || col >= width_in_blocks) {
        return 0;
    }
    return dc_values[static_cast<size_t>(row * width_in_blocks + col)];
}

bool BuildFeatureBitmap(
    const phase1::JpegDctImage &image,
    FeatureBitmap *out_bitmap,
    int *out_luma_component_index,
    std::string *error_message) {
    if (out_bitmap == nullptr) {
        SetError(error_message, "out_bitmap is null.");
        return false;
    }

    int luma_component_index = -1;
    const phase1::DctComponent *luma =
        phase1::GetLumaComponent(image, &luma_component_index, error_message);
    if (luma == nullptr) {
        return false;
    }

    if (luma->width_in_blocks <= 0 || luma->height_in_blocks <= 0) {
        SetError(error_message, "Invalid luma dimensions.");
        return false;
    }

    FeatureBitmap bitmap;
    bitmap.width_in_blocks = luma->width_in_blocks;
    bitmap.height_in_blocks = luma->height_in_blocks;
    bitmap.bits.resize(static_cast<size_t>(bitmap.width_in_blocks * bitmap.height_in_blocks));
    bitmap.dc_values.resize(static_cast<size_t>(bitmap.width_in_blocks * bitmap.height_in_blocks));
    bool has_one = false;
    int64_t dc_sum = 0;
    for (const phase1::DctBlock &block : luma->blocks) {
        dc_sum += static_cast<int64_t>(block.coeff[0]);
    }
    const int64_t dc_mean = dc_sum /
        static_cast<int64_t>(std::max<size_t>(1, luma->blocks.size()));

    for (int row = 0; row < luma->height_in_blocks; ++row) {
        for (int col = 0; col < luma->width_in_blocks; ++col) {
            const phase1::DctBlock *block = luma->GetBlock(row, col);
            if (block == nullptr) {
                SetError(error_message, "Internal error: failed to access luma block.");
                return false;
            }
            const int16_t dc = block->coeff[0];
            const uint8_t bit = static_cast<uint8_t>(dc >= dc_mean ? 1 : 0);
            const size_t idx = static_cast<size_t>(row * bitmap.width_in_blocks + col);
            bitmap.bits[idx] = bit;
            bitmap.dc_values[idx] = dc;
            if (bit != 0) {
                has_one = true;
            }
        }
    }

    if (!has_one) {
        SetError(error_message, "Feature bitmap is all zeros; DCT decode likely failed.");
        return false;
    }

    if (out_luma_component_index != nullptr) {
        *out_luma_component_index = luma_component_index;
    }
    *out_bitmap = std::move(bitmap);
    return true;
}

bool ExtractWindowHashes(
    const FeatureBitmap &bitmap,
    int window_size,
    std::vector<WindowHash> *out_hashes,
    std::string *error_message) {
    if (out_hashes == nullptr) {
        SetError(error_message, "out_hashes is null.");
        return false;
    }
    if (window_size <= 0) {
        SetError(error_message, "window_size must be positive.");
        return false;
    }
    if (window_size > 8) {
        SetError(error_message, "window_size too large. window_size*window_size must be <= 64.");
        return false;
    }
    if (bitmap.width_in_blocks < window_size || bitmap.height_in_blocks < window_size) {
        SetError(error_message, "window_size exceeds bitmap dimensions.");
        return false;
    }

    std::vector<WindowHash> hashes;
    const int max_row = bitmap.height_in_blocks - window_size;
    const int max_col = bitmap.width_in_blocks - window_size;
    hashes.reserve(static_cast<size_t>((max_row + 1) * (max_col + 1)));
    const int window_bits = window_size * window_size;
    const uint64_t all_ones = (window_bits >= 64) ? ~0ULL : ((1ULL << window_bits) - 1ULL);

    for (int row = 0; row <= max_row; ++row) {
        for (int col = 0; col <= max_col; ++col) {
            uint64_t bit_window = 0;
            uint64_t raw_value = 1469598103934665603ULL;
            for (int wr = 0; wr < window_size; ++wr) {
                for (int wc = 0; wc < window_size; ++wc) {
                    const uint8_t bit = bitmap.Get(row + wr, col + wc);
                    bit_window = (bit_window << 1) | static_cast<uint64_t>(bit);
                    const uint16_t dc = static_cast<uint16_t>(bitmap.GetDc(row + wr, col + wc));
                    raw_value ^= (static_cast<uint64_t>(dc) | (static_cast<uint64_t>(bit) << 16));
                    raw_value *= 1099511628211ULL;
                }
            }
            if (bit_window == 0 || bit_window == all_ones) {
                continue;
            }
            const uint64_t value = MixHash(raw_value ^ (bit_window * 0x9e3779b97f4a7c15ULL));
            hashes.push_back(WindowHash{value, row, col});
        }
    }

    *out_hashes = std::move(hashes);
    return true;
}

bool ComputeNTransformMinHash(
    const std::vector<WindowHash> &window_hashes,
    const Phase2Config &config,
    std::vector<MinHashFeature> *out_features,
    std::string *error_message) {
    if (out_features == nullptr) {
        SetError(error_message, "out_features is null.");
        return false;
    }
    if (config.num_transforms <= 0) {
        SetError(error_message, "num_transforms must be positive.");
        return false;
    }
    if (window_hashes.empty()) {
        SetError(error_message, "window_hashes is empty.");
        return false;
    }

    const std::vector<LinearHashParam> params =
        BuildLinearHashParams(config.num_transforms, config.hash_seed);
    for (int i = 0; i < config.num_transforms; ++i) {
        if (params[static_cast<size_t>(i)].a == 0 || params[static_cast<size_t>(i)].b == 0) {
            SetError(error_message, "N-Transform parameters must be non-zero.");
            return false;
        }
    }

    std::vector<MinHashFeature> features;
    features.reserve(static_cast<size_t>(config.num_transforms));

    for (int i = 0; i < config.num_transforms; ++i) {
        MinHashFeature best;
        best.transform_index = static_cast<uint32_t>(i);
        best.hash_value = std::numeric_limits<uint64_t>::max();
        best.window_hash = 0;
        best.row = -1;
        best.col = -1;

        for (const WindowHash &window_hash : window_hashes) {
            const uint64_t transformed = params[static_cast<size_t>(i)].a * window_hash.value +
                                         params[static_cast<size_t>(i)].b;
            const bool better =
                (transformed < best.hash_value) ||
                (transformed == best.hash_value &&
                 (window_hash.row < best.row ||
                  (window_hash.row == best.row && window_hash.col < best.col)));
            if (better) {
                best.hash_value = transformed;
                best.window_hash = window_hash.value;
                best.row = window_hash.row;
                best.col = window_hash.col;
            }
        }

        features.push_back(best);
    }

    *out_features = std::move(features);
    return true;
}

bool BuildSignature(
    const phase1::JpegDctImage &image,
    const Phase2Config &config,
    Signature *out_signature,
    std::string *error_message) {
    if (out_signature == nullptr) {
        SetError(error_message, "out_signature is null.");
        return false;
    }

    FeatureBitmap bitmap;
    int luma_component_index = -1;
    if (!BuildFeatureBitmap(image, &bitmap, &luma_component_index, error_message)) {
        return false;
    }

    std::vector<WindowHash> window_hashes;
    if (!ExtractWindowHashes(bitmap, config.window_size, &window_hashes, error_message)) {
        return false;
    }

    std::vector<MinHashFeature> features;
    if (!ComputeNTransformMinHash(window_hashes, config, &features, error_message)) {
        return false;
    }

    Signature signature;
    signature.luma_component_index = luma_component_index;
    signature.window_size = config.window_size;
    signature.bitmap_width_in_blocks = bitmap.width_in_blocks;
    signature.bitmap_height_in_blocks = bitmap.height_in_blocks;
    signature.features = std::move(features);

    *out_signature = std::move(signature);
    return true;
}

bool DumpSignatureToText(
    const Signature &signature,
    const std::string &output_text_path,
    std::string *error_message) {
    std::ofstream output(output_text_path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        SetError(error_message, "Failed to open output text file: " + output_text_path);
        return false;
    }

    output << signature.luma_component_index << " "
           << signature.window_size << " "
           << signature.bitmap_width_in_blocks << " "
           << signature.bitmap_height_in_blocks << " "
           << signature.features.size() << "\n";

    for (const MinHashFeature &feature : signature.features) {
        output << feature.transform_index << " "
               << feature.hash_value << " "
               << feature.window_hash << " "
               << feature.row << " "
               << feature.col << "\n";
    }
    return true;
}

void InMemoryInvertedIndex::AddImageSignature(
    const std::string &image_id,
    const Signature &signature) {
    std::unordered_set<uint64_t> unique_features;
    for (const MinHashFeature &feature : signature.features) {
        unique_features.insert(feature.hash_value);
    }

    for (uint64_t feature_value : unique_features) {
        std::vector<std::string> &posting = postings_[feature_value];
        if (std::find(posting.begin(), posting.end(), image_id) == posting.end()) {
            posting.push_back(image_id);
        }
    }
}

MatchResult InMemoryInvertedIndex::QueryBestMatch(
    const Signature &target_signature,
    const std::string &exclude_image_id) const {
    MatchResult result;

    std::unordered_set<uint64_t> unique_target_features;
    for (const MinHashFeature &feature : target_signature.features) {
        unique_target_features.insert(feature.hash_value);
    }

    for (uint64_t feature_value : unique_target_features) {
        const auto it = postings_.find(feature_value);
        if (it == postings_.end()) {
            continue;
        }
        for (const std::string &image_id : it->second) {
            if (!exclude_image_id.empty() && image_id == exclude_image_id) {
                continue;
            }
            result.candidate_scores[image_id] += 1;
        }
    }

    for (const auto &kv : result.candidate_scores) {
        if (kv.second > result.best_score ||
            (kv.second == result.best_score &&
             (!kv.first.empty() && (result.best_image_id.empty() || kv.first < result.best_image_id)))) {
            result.best_score = kv.second;
            result.best_image_id = kv.first;
        }
    }

    return result;
}

}  // namespace phase2
}  // namespace deltadct
