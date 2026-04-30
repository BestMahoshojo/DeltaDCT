#include "phase3_delta.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

using deltadct::phase1::DctBlock;
using deltadct::phase3::BlockMatrix;
using deltadct::phase3::CoordinateHint;
using deltadct::phase3::DeltaResult;
using deltadct::phase3::GenerateDeltaInstructions;
using deltadct::phase3::Instruction;
using deltadct::phase3::InstructionKind;
using deltadct::phase3::MatchRoute;
using deltadct::phase3::Phase3Config;
using deltadct::phase3::ReconstructTargetFromInstructions;
using deltadct::phase3::ValidateInstructionsBounds;

namespace {

static_assert(
    std::is_same<decltype(std::declval<DctBlock>().coeff[0]), int16_t &>::value,
    "DCT coefficient type must be int16_t.");

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

void TestFpmWithSparseDifferences() {
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
    config.min_copy_length = 1;

    DeltaResult delta;
    std::string error;
    const bool ok = GenerateDeltaInstructions(base, target, hint, config, &delta, &error);
    Expect(ok, "GenerateDeltaInstructions(FPM) failed: " + error);
    Expect(delta.route == MatchRoute::kFpm, "Route should be FPM for aligned matrices.");

    const bool bounds_ok =
        ValidateInstructionsBounds(delta, target.width_in_blocks, target.height_in_blocks, &error);
    Expect(bounds_ok, "FPM bounds validation failed: " + error);

    int insert_count = 0;
    int copy_count = 0;
    int merged_copy_count = 0;
    for (const Instruction &instruction : delta.instructions) {
        if (instruction.kind == InstructionKind::kInsert) {
            ++insert_count;
        } else {
            ++copy_count;
            if (instruction.copy.length > 1) {
                ++merged_copy_count;
            }
        }
    }

    Expect(insert_count <= 3, "FPM should emit no more than 3 INSERT instructions.");
    Expect(copy_count > 0, "FPM should emit COPY instructions for identical regions.");
    Expect(merged_copy_count > 0, "FPM COPY instructions should merge contiguous blocks.");

    BlockMatrix restored;
    const bool recon_ok = ReconstructTargetFromInstructions(base, delta, &restored, &error);
    Expect(recon_ok, "ReconstructTargetFromInstructions(FPM) failed: " + error);
    Expect(MatricesSimilar(restored, target), "FPM reconstruction mismatch.");

    std::cout << "[PASS] FPM sparse-difference test\n";
}

void TestDCHashWithRightShift() {
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
    config.min_copy_length = 1;

    DeltaResult delta;
    std::string error;
    const bool ok = GenerateDeltaInstructions(base, target, hint, config, &delta, &error);
    Expect(ok, "GenerateDeltaInstructions(DCHash) failed: " + error);
    Expect(delta.route == MatchRoute::kDCHash, "Route should switch to DCHash on coordinate shift.");

    const bool bounds_ok = ValidateInstructionsBounds(delta, width, height, &error);
    Expect(bounds_ok, "DCHash bounds validation failed: " + error);

    int merged_copy_count = 0;
    int shifted_copy_count = 0;
    int max_copy_len = 0;
    for (const Instruction &instruction : delta.instructions) {
        if (instruction.kind != InstructionKind::kCopy) {
            continue;
        }
        max_copy_len = std::max(max_copy_len, instruction.copy.length);
        if (instruction.copy.length > 1) {
            ++merged_copy_count;
        }
        if (instruction.copy.base_col - instruction.copy.target_col == -2) {
            ++shifted_copy_count;
        }

        const int target_end = instruction.copy.target_col + instruction.copy.length;
        const int base_end = instruction.copy.base_col + instruction.copy.length;
        Expect(target_end <= width, "DCHash COPY crossed target row boundary.");
        Expect(base_end <= width, "DCHash COPY crossed base row boundary.");
    }

    Expect(merged_copy_count > 0, "DCHash COPY instructions should merge contiguous blocks.");
    Expect(shifted_copy_count > 0, "DCHash should emit COPY with displacement (base_col-target_col=-2).");
    Expect(max_copy_len >= width - 2, "DCHash should find long matches for shifted rows.");

    BlockMatrix restored;
    const bool recon_ok = ReconstructTargetFromInstructions(base, delta, &restored, &error);
    Expect(recon_ok, "ReconstructTargetFromInstructions(DCHash) failed: " + error);
    Expect(MatricesSimilar(restored, target), "DCHash reconstruction mismatch.");

    std::cout << "[PASS] DCHash right-shift test\n";
}

}  // namespace

int main() {
    TestFpmWithSparseDifferences();
    TestDCHashWithRightShift();
    std::cout << "[PASS] All Phase 3 tests\n";
    return 0;
}
