#include "deltadct.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "phase1_dct_codec.hpp"
#include "phase2_similarity.hpp"
#include "phase3_delta.hpp"
#include "phase4_storage.hpp"
#include "phase5_restore.hpp"

namespace deltadct {

namespace {

bool ReadWholeFile(
    const std::string &path,
    std::vector<uint8_t> *out_data) {
    if (out_data == nullptr) {
        return false;
    }
    std::ifstream input(path, std::ios::binary | std::ios::in);
    if (!input.is_open()) {
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return false;
    }
    input.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0) {
        input.read(reinterpret_cast<char *>(data.data()), size);
        if (!input.good()) {
            return false;
        }
    }

    *out_data = std::move(data);
    return true;
}

phase3::RouteMode ToPhase3RouteMode(const RoutingMode mode) {
    switch (mode) {
        case RoutingMode::kFpmOnly:
            return phase3::RouteMode::kForceFpm;
        case RoutingMode::kDCHashOnly:
            return phase3::RouteMode::kForceDCHash;
        case RoutingMode::kAuto:
        default:
            return phase3::RouteMode::kAuto;
    }
}

std::string RouteToString(const phase3::MatchRoute route) {
    switch (route) {
        case phase3::MatchRoute::kFpm:
            return "FPM";
        case phase3::MatchRoute::kDCHash:
            return "DCHash";
        default:
            return "Unknown";
    }
}

phase3::CoordinateHint DeriveCoordinateHint(
    const phase2::Signature &base_signature,
    const phase2::Signature &target_signature) {
    phase3::CoordinateHint hint;
    if (base_signature.features.empty() || target_signature.features.empty()) {
        return hint;
    }

    std::size_t best_idx = 0;
    const std::size_t min_size = std::min(base_signature.features.size(), target_signature.features.size());
    for (std::size_t i = 0; i < min_size; ++i) {
        if (base_signature.features[i].transform_index ==
            target_signature.features[i].transform_index) {
            best_idx = i;
            break;
        }
    }

    hint.valid = true;
    hint.base_row = base_signature.features[best_idx].row;
    hint.base_col = base_signature.features[best_idx].col;
    hint.target_row = target_signature.features[best_idx].row;
    hint.target_col = target_signature.features[best_idx].col;
    return hint;
}

size_t CountVarintBytes(uint32_t value) {
    size_t count = 1;
    while (value >= 0x80u) {
        value >>= 7;
        ++count;
    }
    return count;
}

size_t CountCopyMetadataBytes(const phase3::DeltaResult &delta) {
    std::vector<phase3::Instruction> merged;
    merged.reserve(delta.instructions.size());
    for (const phase3::Instruction &instr : delta.instructions) {
        if (instr.kind == phase3::InstructionKind::kCopy && !merged.empty() &&
            merged.back().kind == phase3::InstructionKind::kCopy) {
            phase3::CopyInstruction &prev = merged.back().copy;
            const phase3::CopyInstruction &cur = instr.copy;
            if (prev.base_row == cur.base_row &&
                prev.base_col + prev.length == cur.base_col &&
                prev.target_row == cur.target_row &&
                prev.target_col + prev.length == cur.target_col) {
                prev.length += cur.length;
                continue;
            }
        }
        merged.push_back(instr);
    }

    size_t bytes = 0;
    for (const phase3::Instruction &instr : merged) {
        if (instr.kind != phase3::InstructionKind::kCopy) {
            continue;
        }

        if (instr.copy.target_row < 0 || instr.copy.target_col < 0 ||
            instr.copy.base_row < 0 || instr.copy.base_col < 0 || instr.copy.length <= 0) {
            continue;
        }

        const uint32_t tr = static_cast<uint32_t>(instr.copy.target_row);
        const uint32_t tc = static_cast<uint32_t>(instr.copy.target_col);
        const uint32_t br = static_cast<uint32_t>(instr.copy.base_row);
        const uint32_t bc = static_cast<uint32_t>(instr.copy.base_col);
        const uint32_t len = static_cast<uint32_t>(instr.copy.length);

        bytes += CountVarintBytes(0u);
        if (tr <= 0xFFFFu && tc <= 0xFFFFu && br <= 0xFFFFu && bc <= 0xFFFFu && len <= 0xFFFFu) {
            bytes += 1u + 2u * 5u;
        } else {
            bytes += 1u;
            bytes += CountVarintBytes(tr) + CountVarintBytes(tc) + CountVarintBytes(br) +
                     CountVarintBytes(bc) + CountVarintBytes(len);
        }
    }
    return bytes;
}

size_t CountInsertRawBytes(const phase3::DeltaResult &delta, int num_components) {
    size_t insert_count = 0;
    for (const phase3::Instruction &instruction : delta.instructions) {
        if (instruction.kind == phase3::InstructionKind::kInsert) {
            ++insert_count;
        }
    }
    return insert_count * static_cast<size_t>(phase1::kDctCoeffsPerBlock) * sizeof(int16_t) *
           static_cast<size_t>(std::max(1, num_components));
}

}  // namespace

DedupResult compress_image(
    const std::string &target_jpg,
    const std::string &base_jpg,
    const std::string &output_ddct,
    const CompressOptions &options) {
    DedupResult result;

    const auto begin = std::chrono::high_resolution_clock::now();

    phase1::JpegDctImage base_image;
    phase1::JpegDctImage target_image;
    std::string error_message;
    if (!phase1::ReadJpegDctImage(base_jpg, &base_image, &error_message) ||
        !phase1::ReadJpegDctImage(target_jpg, &target_image, &error_message)) {
        return result;
    }

    phase2::Phase2Config phase2_config;
    phase2_config.window_size = std::max(1, options.window_size);
    phase2_config.num_transforms = std::max(1, options.num_features);
    phase2::Signature base_signature;
    phase2::Signature target_signature;
    const auto similarity_begin = std::chrono::high_resolution_clock::now();
    if (!phase2::BuildSignature(base_image, phase2_config, &base_signature, &error_message) ||
        !phase2::BuildSignature(target_image, phase2_config, &target_signature, &error_message)) {
        return result;
    }
    const auto similarity_end = std::chrono::high_resolution_clock::now();

    const phase3::CoordinateHint hint = DeriveCoordinateHint(base_signature, target_signature);

    phase3::BlockMatrix base_matrix;
    phase3::BlockMatrix target_matrix;
    if (!phase3::BuildBlockMatrixFromLuma(base_image, &base_matrix, &error_message) ||
        !phase3::BuildBlockMatrixFromLuma(target_image, &target_matrix, &error_message)) {
        return result;
    }

    phase3::Phase3Config phase3_config;
    phase3_config.route_mode = ToPhase3RouteMode(options.mode);
    phase3_config.strict_lossless = options.strict_lossless;
    phase3::DeltaResult delta;
    const auto delta_begin = std::chrono::high_resolution_clock::now();
    if (!phase3::GenerateDeltaInstructions(
            base_matrix,
            target_matrix,
            hint,
            phase3_config,
            &delta,
            &error_message)) {
        return result;
    }
    const auto delta_end = std::chrono::high_resolution_clock::now();

    phase4::DeltaDctPackage package;
    if (!phase4::BuildPackageFromDelta(base_jpg, delta, &package, &error_message)) {
        return result;
    }

    {
        std::vector<uint8_t> header_prefix;
        if (!phase1::ExtractJpegHeaderPrefixBeforeSos(target_jpg, &header_prefix, &error_message)) {
            return result;
        }
        package.header_prefix = std::move(header_prefix);
    }

    // record number of components so serializer knows how many channels per INSERT
    package.num_components = target_image.num_components;

    if (!phase4::WritePackageToFile(output_ddct, package, &error_message)) {
        return result;
    }

    std::error_code ec_target;
    std::error_code ec_imd;
    const auto original_size_u64 = std::filesystem::file_size(target_jpg, ec_target);
    const auto compressed_size_u64 = std::filesystem::file_size(output_ddct, ec_imd);
    if (ec_target || ec_imd) {
        return result;
    }

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - begin);

    result.success = true;
    result.original_size = static_cast<std::size_t>(original_size_u64);
    result.compressed_size = static_cast<std::size_t>(compressed_size_u64);
    result.compression_ratio =
        (result.compressed_size == 0) ? 0.0
                                      : (static_cast<double>(result.original_size) /
                                         static_cast<double>(result.compressed_size));
    result.time_ms = elapsed.count();
    result.similarity_time_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(similarity_end - similarity_begin)
            .count();
    result.delta_time_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(delta_end - delta_begin).count();
    result.route_used = RouteToString(delta.route);
    result.blocks_total = delta.blocks_total;
    result.blocks_exact = delta.blocks_exact;
    result.blocks_similar = delta.blocks_similar;
    result.blocks_different = delta.blocks_different;
    result.blocks_width = delta.width_in_blocks;
    result.blocks_height = delta.height_in_blocks;
    result.block_map = delta.block_map;
    result.header_bytes = package.header_prefix.size();
    result.copy_meta_bytes = CountCopyMetadataBytes(delta);
    result.insert_raw_bytes = CountInsertRawBytes(delta, target_image.num_components);

    const double algo_seconds = (result.similarity_time_ms + result.delta_time_ms) / 1000.0;
    const double original_mb = static_cast<double>(result.original_size) / (1024.0 * 1024.0);
    result.throughput_mb_s = (algo_seconds > 0.0) ? (original_mb / algo_seconds) : 0.0;
    return result;
}

bool decompress_image(
    const std::string &input_ddct,
    const std::string &base_jpg,
    const std::string &output_jpg) {
    phase5::RestoreReport report;
    std::string error_message;
    return phase5::RestoreTargetJpegFromPackage(
        base_jpg,
        input_ddct,
        output_jpg,
        "",
        &report,
        &error_message);
}

std::vector<uint64_t> extract_features(
    const std::string &img_path,
    int window_size,
    int num_features) {
    phase1::JpegDctImage image;
    std::string error_message;
    if (!phase1::ReadJpegDctImage(img_path, &image, &error_message)) {
        throw std::runtime_error("Failed to decode JPEG DCT: " + img_path);
    }

    int luma_index = -1;
    const phase1::DctComponent *luma = phase1::GetLumaComponent(image, &luma_index, &error_message);
    if (luma == nullptr) {
        throw std::runtime_error("Failed to decode JPEG DCT: " + img_path);
    }
    (void)img_path;

    phase2::Phase2Config config;
    config.window_size = std::max(1, window_size);
    config.num_transforms = std::max(1, num_features);

    phase2::Signature signature;
    if (!phase2::BuildSignature(image, config, &signature, &error_message)) {
        throw std::runtime_error("BuildSignature failed: " + error_message);
    }

    std::vector<uint64_t> hashes;
    hashes.reserve(signature.features.size());
    for (const phase2::MinHashFeature &feature : signature.features) {
        hashes.push_back(feature.hash_value);
    }
    return hashes;
}

}  // namespace deltadct