#include "phase3_delta.hpp"
#include "phase4_storage.hpp"
#include "phase5_restore.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

using deltadct::phase1::DctBlock;
using deltadct::phase3::BlockMatrix;
using deltadct::phase3::CoordinateHint;
using deltadct::phase3::DeltaResult;
using deltadct::phase3::GenerateDeltaInstructions;
using deltadct::phase3::Instruction;
using deltadct::phase3::InstructionKind;
using deltadct::phase3::MatchRoute;
using deltadct::phase3::Phase3Config;
using deltadct::phase3::ValidateInstructionsBounds;
using deltadct::phase4::BuildPackageFromDelta;
using deltadct::phase4::DeserializePackage;
using deltadct::phase4::DeltaDctPackage;
using deltadct::phase4::IsZstdEnabledAtBuild;
using deltadct::phase4::ReadPackageFromFile;
using deltadct::phase4::SerializePackage;
using deltadct::phase4::StorageOptions;
using deltadct::phase4::BlockCodecPreference;
using deltadct::phase4::WritePackageToFile;
using deltadct::phase5::RestoreTargetMatrixFromPackage;

namespace {

void Expect(bool cond, const std::string &message) {
    if (!cond) {
        std::cerr << "[FAIL] " << message << "\n";
        std::exit(1);
    }
}

BlockMatrix MakePatternMatrix(int width, int height, int seed) {
    BlockMatrix matrix;
    matrix.width_in_blocks = width;
    matrix.height_in_blocks = height;
    matrix.blocks.resize(static_cast<size_t>(width * height));

    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            auto *mb = matrix.GetBlock(row, col);
            mb->components.resize(1);
            DctBlock &block = mb->components[0];
            const int base = seed + row * 211 + col * 37;
            for (int k = 0; k < 64; ++k) {
                const int value = (base + k * 13) % 4096 - 2048;
                block.coeff[static_cast<size_t>(k)] = static_cast<int16_t>(value);
            }
        }
    }
    return matrix;
}

void MutateBlock(BlockMatrix *matrix, int row, int col, int delta) {
    auto *mb = matrix->GetBlock(row, col);
    Expect(mb != nullptr, "MutateBlock got invalid coordinates.");
    Expect(!mb->components.empty(), "MutateBlock missing component.");
    DctBlock &block = mb->components[0];
    block.coeff[0] = static_cast<int16_t>(block.coeff[0] + delta);
    block.coeff[1] = static_cast<int16_t>(block.coeff[1] - delta);
}

bool BlocksSimilar(const DctBlock &lhs, const DctBlock &rhs) {
    int total_diff = 0;
    for (int i = 0; i < deltadct::phase1::kDctCoeffsPerBlock; ++i) {
        const int diff = std::abs(static_cast<int>(lhs.coeff[i]) - static_cast<int>(rhs.coeff[i]));
        total_diff += diff;
        if (total_diff > deltadct::phase3::kBlockDiffThreshold) {
            return false;
        }
    }
    return true;
}

bool MatricesSimilar(const BlockMatrix &lhs, const BlockMatrix &rhs) {
    if (lhs.width_in_blocks != rhs.width_in_blocks ||
        lhs.height_in_blocks != rhs.height_in_blocks ||
        lhs.blocks.size() != rhs.blocks.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.blocks.size(); ++i) {
        const DctBlock &L = lhs.blocks[i].components[0];
        const DctBlock &R = rhs.blocks[i].components[0];
        if (!BlocksSimilar(L, R)) {
            return false;
        }
    }
    return true;
}

void TestPhase45FpmRoundtrip() {
    BlockMatrix base = MakePatternMatrix(8, 4, 1000);
    BlockMatrix target = base;
    MutateBlock(&target, 0, 2, 3);
    MutateBlock(&target, 1, 5, 4);
    MutateBlock(&target, 3, 1, 5);

    CoordinateHint hint;
    hint.valid = true;
    hint.target_row = 1;
    hint.target_col = 1;
    hint.base_row = 1;
    hint.base_col = 1;

    Phase3Config config;
    DeltaResult delta;
    std::string error;
    const bool gen_ok = GenerateDeltaInstructions(base, target, hint, config, &delta, &error);
    Expect(gen_ok, "GenerateDeltaInstructions(FPM) failed: " + error);
    Expect(delta.route == MatchRoute::kFpm, "Expected FPM route.");

    DeltaDctPackage package;
    const bool pkg_ok = BuildPackageFromDelta("base-fpm", delta, &package, &error);
    Expect(pkg_ok, "BuildPackageFromDelta(FPM) failed: " + error);

    std::vector<uint8_t> binary;
    const bool ser_ok = SerializePackage(package, &binary, &error);
    Expect(ser_ok, "SerializePackage(FPM) failed: " + error);

    DeltaDctPackage decoded;
    const bool de_ok = DeserializePackage(binary, &decoded, &error);
    Expect(de_ok, "DeserializePackage(FPM) failed: " + error);
    Expect(decoded.base_id == "base-fpm", "Decoded base_id mismatch for FPM case.");
    Expect(
        decoded.block_codec == deltadct::phase4::BlockCodec::kRaw ||
            decoded.block_codec == deltadct::phase4::BlockCodec::kZstd,
        "Decoded block_codec must be raw or zstd.");

    BlockMatrix restored;
    const bool restore_ok = RestoreTargetMatrixFromPackage(base, decoded, &restored, &error);
    Expect(restore_ok, "RestoreTargetMatrixFromPackage(FPM) failed: " + error);
    Expect(MatricesSimilar(restored, target), "FPM restored matrix mismatch.");

    int merged_copy_count = 0;
    for (const Instruction &instruction : decoded.instructions) {
        if (instruction.kind == InstructionKind::kCopy && instruction.copy.length > 1) {
            ++merged_copy_count;
        }
    }
    Expect(merged_copy_count > 0, "FPM package should preserve merged COPY instructions.");

    std::cout << "[PASS] Phase4/5 FPM package roundtrip\n";
}

void TestPhase45DCHashRoundtrip() {
    const int width = 12;
    const int height = 4;

    BlockMatrix base = MakePatternMatrix(width, height, 3000);
    BlockMatrix target;
    target.width_in_blocks = width;
    target.height_in_blocks = height;
    target.blocks.resize(static_cast<size_t>(width * height));

    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            auto *dst_block = target.GetBlock(row, col);
            dst_block->components.resize(1);
            if (col < 2) {
                for (int k = 0; k < 64; ++k) {
                    dst_block->components[0].coeff[static_cast<size_t>(k)] =
                        static_cast<int16_t>(15000 + row * 31 + col * 7 + k);
                }
            } else {
                const auto *src_block = base.GetBlock(row, col - 2);
                dst_block->components[0] = src_block->components[0];
            }
        }
    }

    CoordinateHint hint;
    hint.valid = true;
    hint.target_row = 0;
    hint.target_col = 2;
    hint.base_row = 0;
    hint.base_col = 0;

    Phase3Config config;
    DeltaResult delta;
    std::string error;
    const bool gen_ok = GenerateDeltaInstructions(base, target, hint, config, &delta, &error);
    Expect(gen_ok, "GenerateDeltaInstructions(DCHash) failed: " + error);
    Expect(delta.route == MatchRoute::kDCHash, "Expected DCHash route.");

    const bool bounds_ok = ValidateInstructionsBounds(delta, width, height, &error);
    Expect(bounds_ok, "DCHash bounds validation failed: " + error);

    DeltaDctPackage package;
    const bool pkg_ok = BuildPackageFromDelta("base-shift2", delta, &package, &error);
    Expect(pkg_ok, "BuildPackageFromDelta(DCHash) failed: " + error);

    const std::string temp_path = "/tmp/phase45_shift2.ddct";
    StorageOptions options;
    options.block_codec_preference =
        IsZstdEnabledAtBuild() ? BlockCodecPreference::kZstd : BlockCodecPreference::kRaw;
    const bool write_ok = WritePackageToFile(temp_path, package, &error, options);
    Expect(write_ok, "WritePackageToFile(DCHash) failed: " + error);

    DeltaDctPackage decoded;
    const bool read_ok = ReadPackageFromFile(temp_path, &decoded, &error);
    Expect(read_ok, "ReadPackageFromFile(DCHash) failed: " + error);
    if (IsZstdEnabledAtBuild()) {
        Expect(
            decoded.block_codec == deltadct::phase4::BlockCodec::kZstd,
            "Expected zstd block codec when forcing zstd and build supports it.");
    } else {
        Expect(
            decoded.block_codec == deltadct::phase4::BlockCodec::kRaw,
            "Expected raw block codec when zstd build support is unavailable.");
    }

    BlockMatrix restored;
    const bool restore_ok = RestoreTargetMatrixFromPackage(base, decoded, &restored, &error);
    Expect(restore_ok, "RestoreTargetMatrixFromPackage(DCHash) failed: " + error);
    Expect(MatricesSimilar(restored, target), "DCHash restored matrix mismatch.");

    int merged_copy_count = 0;
    int shifted_copy_count = 0;
    for (const Instruction &instruction : decoded.instructions) {
        if (instruction.kind != InstructionKind::kCopy) {
            continue;
        }
        if (instruction.copy.length > 1) {
            ++merged_copy_count;
        }
        if (instruction.copy.base_col - instruction.copy.target_col == -2) {
            ++shifted_copy_count;
        }
    }
    Expect(merged_copy_count > 0, "DCHash package should preserve merged COPY instructions.");
    Expect(shifted_copy_count > 0, "DCHash package should preserve shifted COPY coordinates.");

    std::remove(temp_path.c_str());
    std::cout << "[PASS] Phase4/5 DCHash package roundtrip\n";
}

}  // namespace

int main() {
    TestPhase45FpmRoundtrip();
    TestPhase45DCHashRoundtrip();
    std::cout << "[PASS] All Phase 4/5 tests\n";
    return 0;
}
