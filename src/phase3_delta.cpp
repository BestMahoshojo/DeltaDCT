#include "phase3_delta.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace deltadct {
namespace phase3 {

namespace {

struct Position {
    int row = 0;
    int col = 0;
};

void SetError(std::string *error_message, const std::string &message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

bool MatrixShapeValid(const BlockMatrix &matrix) {
    if (matrix.width_in_blocks <= 0 || matrix.height_in_blocks <= 0) {
        return false;
    }
    return static_cast<int>(matrix.blocks.size()) ==
           matrix.width_in_blocks * matrix.height_in_blocks;
}


bool BlocksEqual(
    const MultiDctBlock &lhs,
    const MultiDctBlock &rhs,
    bool strict_lossless) {
    if (lhs.components.size() != rhs.components.size()) {
        return false;
    }
    for (size_t c = 0; c < lhs.components.size(); ++c) {
        const phase1::DctBlock &L = lhs.components[c];
        const phase1::DctBlock &R = rhs.components[c];
        if (strict_lossless) {
            if (std::memcmp(L.coeff.data(), R.coeff.data(),
                           sizeof(int16_t) * phase1::kDctCoeffsPerBlock) != 0) {
                return false;
            }
            continue;
        }
        int total_diff = 0;
        for (int i = 0; i < phase1::kDctCoeffsPerBlock; ++i) {
            const int diff = std::abs(static_cast<int>(L.coeff[i]) - static_cast<int>(R.coeff[i]));
            total_diff += diff;
            if (total_diff > kBlockDiffThreshold) {
                return false;
            }
        }
    }
    return true;
}

enum class BlockMatchType {
    kExact,
    kSimilar,
    kDifferent,
};

BlockMatchType CompareBlocks(
    const MultiDctBlock &lhs,
    const MultiDctBlock &rhs,
    bool strict_lossless) {
    if (lhs.components.size() != rhs.components.size()) {
        return BlockMatchType::kDifferent;
    }
    bool overall_exact = true;
    for (size_t c = 0; c < lhs.components.size(); ++c) {
        const phase1::DctBlock &L = lhs.components[c];
        const phase1::DctBlock &R = rhs.components[c];
        if (strict_lossless) {
            if (std::memcmp(L.coeff.data(), R.coeff.data(),
                           sizeof(int16_t) * phase1::kDctCoeffsPerBlock) != 0) {
                return BlockMatchType::kDifferent;
            }
            continue;
        }
        int total_diff = 0;
        bool exact = true;
        for (int i = 0; i < phase1::kDctCoeffsPerBlock; ++i) {
            const int diff = std::abs(static_cast<int>(L.coeff[i]) - static_cast<int>(R.coeff[i]));
            if (diff != 0) {
                exact = false;
            }
            total_diff += diff;
            if (total_diff > kBlockDiffThreshold) {
                return BlockMatchType::kDifferent;
            }
        }
        if (!exact) overall_exact = false;
    }
    return overall_exact ? BlockMatchType::kExact : BlockMatchType::kSimilar;
}

uint64_t BuildDCHash(const MultiDctBlock &block) {
    // Build hash from DCs of first component
    if (block.components.empty()) return 0;
    const phase1::DctBlock &b0 = block.components[0];
    const uint32_t c00 = static_cast<uint32_t>((b0.coeff[0] >> 1) & 0xFF);
    const uint32_t c01 = static_cast<uint32_t>((b0.coeff[1] >> 1) & 0xFF);
    const uint32_t c10 = static_cast<uint32_t>((b0.coeff[8] >> 1) & 0xFF);
    const uint32_t c11 = static_cast<uint32_t>((b0.coeff[9] >> 1) & 0xFF);

    const uint32_t hash = (c00 << 24) | (c01 << 16) | (c10 << 8) | c11;
    return static_cast<uint64_t>(hash);
}

void FillSimilarityStats(
    const BlockMatrix &base,
    const BlockMatrix &target,
    const Phase3Config &config,
    DeltaResult *out_result) {
    if (out_result == nullptr) {
        return;
    }

    const int width = target.width_in_blocks;
    const int height = target.height_in_blocks;
    if (width <= 0 || height <= 0) {
        return;
    }

    const int total = width * height;
    out_result->blocks_total = total;
    out_result->blocks_exact = 0;
    out_result->blocks_similar = 0;
    out_result->blocks_different = 0;
    out_result->block_map.assign(static_cast<size_t>(total), 0);

    for (const Instruction &instruction : out_result->instructions) {
        if (instruction.kind == InstructionKind::kCopy) {
            for (int offset = 0; offset < instruction.copy.length; ++offset) {
                const int target_row = instruction.copy.target_row;
                const int target_col = instruction.copy.target_col + offset;
                const int base_row = instruction.copy.base_row;
                const int base_col = instruction.copy.base_col + offset;
                const int index = target_row * width + target_col;
                if (index < 0 || index >= total) {
                    continue;
                }

                out_result->block_map[static_cast<size_t>(index)] = 1;
                const MultiDctBlock *target_block = target.GetBlock(target_row, target_col);
                const MultiDctBlock *base_block = base.GetBlock(base_row, base_col);
                if (target_block == nullptr || base_block == nullptr) {
                    ++out_result->blocks_different;
                    continue;
                }

                const BlockMatchType match = CompareBlocks(
                    *target_block,
                    *base_block,
                    config.strict_lossless);
                if (match == BlockMatchType::kExact) {
                    ++out_result->blocks_exact;
                } else if (match == BlockMatchType::kSimilar) {
                    ++out_result->blocks_similar;
                } else {
                    ++out_result->blocks_different;
                }
            }
        } else {
            const int target_row = instruction.insert.target_row;
            const int target_col = instruction.insert.target_col;
            const int index = target_row * width + target_col;
            if (index >= 0 && index < total) {
                out_result->block_map[static_cast<size_t>(index)] = 0;
            }
            ++out_result->blocks_different;
        }
    }

    const int counted = out_result->blocks_exact +
                        out_result->blocks_similar +
                        out_result->blocks_different;
    if (counted < total) {
        out_result->blocks_different += (total - counted);
    }
}

bool AppendCopyInstruction(
    std::vector<Instruction> *instructions,
    const CopyInstruction &copy) {
    if (instructions == nullptr || copy.length <= 0) {
        return false;
    }

    if (!instructions->empty()) {
        Instruction &tail = instructions->back();
        if (tail.kind == InstructionKind::kCopy) {
            const bool same_row_pair =
                tail.copy.target_row == copy.target_row &&
                tail.copy.base_row == copy.base_row;
            const bool adjacent_target =
                tail.copy.target_col + tail.copy.length == copy.target_col;
            const bool adjacent_base =
                tail.copy.base_col + tail.copy.length == copy.base_col;
            if (same_row_pair && adjacent_target && adjacent_base) {
                tail.copy.length += copy.length;
                return true;
            }
        }
    }

    Instruction instruction;
    instruction.kind = InstructionKind::kCopy;
    instruction.copy = copy;
    instructions->push_back(instruction);
    return true;
}

void AppendInsertInstruction(
    std::vector<Instruction> *instructions,
    int target_row,
    int target_col,
    const MultiDctBlock &block) {
    if (instructions == nullptr) {
        return;
    }
    Instruction instruction;
    instruction.kind = InstructionKind::kInsert;
    instruction.insert.target_row = target_row;
    instruction.insert.target_col = target_col;
    instruction.insert.block = block;
    instructions->push_back(instruction);
}

MatchRoute SelectRoute(
    const BlockMatrix &base,
    const BlockMatrix &target,
    const CoordinateHint &hint,
    const Phase3Config &config) {
    if (config.route_mode == RouteMode::kForceFpm) {
        return MatchRoute::kFpm;
    }
    if (config.route_mode == RouteMode::kForceDCHash) {
        return MatchRoute::kDCHash;
    }

    if (hint.valid) {
        const bool aligned =
            (hint.target_row == hint.base_row) && (hint.target_col == hint.base_col);
        return aligned ? MatchRoute::kFpm : MatchRoute::kDCHash;
    }

    const int total = base.width_in_blocks * base.height_in_blocks;
    if (total <= 0) {
        return MatchRoute::kDCHash;
    }

    int same_position_matches = 0;
    for (int row = 0; row < base.height_in_blocks; ++row) {
        for (int col = 0; col < base.width_in_blocks; ++col) {
            const MultiDctBlock *base_block = base.GetBlock(row, col);
            const MultiDctBlock *target_block = target.GetBlock(row, col);
            if (base_block != nullptr && target_block != nullptr &&
                BlocksEqual(*base_block, *target_block, config.strict_lossless)) {
                ++same_position_matches;
            }
        }
    }

    const double ratio = static_cast<double>(same_position_matches) / static_cast<double>(total);
    return (ratio >= config.fpm_same_position_threshold) ? MatchRoute::kFpm : MatchRoute::kDCHash;
}

void GenerateFpmInstructions(
    const BlockMatrix &base,
    const BlockMatrix &target,
    const Phase3Config &config,
    std::vector<Instruction> *instructions) {
    for (int row = 0; row < target.height_in_blocks; ++row) {
        int col = 0;
        while (col < target.width_in_blocks) {
            const MultiDctBlock *target_block = target.GetBlock(row, col);
            const MultiDctBlock *base_block = base.GetBlock(row, col);

            if (target_block != nullptr && base_block != nullptr &&
                BlocksEqual(*target_block, *base_block, config.strict_lossless)) {
                int run = 1;
                while (col + run < target.width_in_blocks) {
                    const MultiDctBlock *next_target = target.GetBlock(row, col + run);
                    const MultiDctBlock *next_base = base.GetBlock(row, col + run);
                    if (next_target == nullptr || next_base == nullptr ||
                        !BlocksEqual(*next_target, *next_base, config.strict_lossless)) {
                        break;
                    }
                    ++run;
                }

                if (run >= config.min_copy_length) {
                    CopyInstruction copy;
                    copy.target_row = row;
                    copy.target_col = col;
                    copy.base_row = row;
                    copy.base_col = col;
                    copy.length = run;
                    AppendCopyInstruction(instructions, copy);
                    col += run;
                    continue;
                }
            }

            if (target_block != nullptr) {
                AppendInsertInstruction(instructions, row, col, *target_block);
            }
            ++col;
        }
    }
}

void GenerateDCHashInstructions(
    const BlockMatrix &base,
    const BlockMatrix &target,
    const Phase3Config &config,
    std::vector<Instruction> *instructions) {
    std::unordered_map<uint64_t, std::vector<Position>> index;
    index.reserve(static_cast<size_t>(base.width_in_blocks * base.height_in_blocks));

    for (int row = 0; row < base.height_in_blocks; ++row) {
        for (int col = 0; col < base.width_in_blocks; ++col) {
            const MultiDctBlock *base_block = base.GetBlock(row, col);
            if (base_block == nullptr) {
                continue;
            }
            index[BuildDCHash(*base_block)].push_back(Position{row, col});
        }
    }

    for (int target_row = 0; target_row < target.height_in_blocks; ++target_row) {
        int target_col = 0;
        while (target_col < target.width_in_blocks) {
            const MultiDctBlock *target_block = target.GetBlock(target_row, target_col);
            if (target_block == nullptr) {
                ++target_col;
                continue;
            }

            int best_length = 0;
            Position best_pos{0, 0};
            const auto it = index.find(BuildDCHash(*target_block));
            if (it != index.end()) {
                for (const Position &candidate : it->second) {
                    const int max_target_len = target.width_in_blocks - target_col;
                    const int max_base_len = base.width_in_blocks - candidate.col;
                    const int max_len = std::min(max_target_len, max_base_len);

                    int length = 0;
                    while (length < max_len) {
                        const MultiDctBlock *target_next =
                            target.GetBlock(target_row, target_col + length);
                        const MultiDctBlock *base_next =
                            base.GetBlock(candidate.row, candidate.col + length);
                        if (target_next == nullptr || base_next == nullptr ||
                            !BlocksEqual(*target_next, *base_next, config.strict_lossless)) {
                            break;
                        }
                        ++length;
                    }

                    if (length > best_length) {
                        best_length = length;
                        best_pos = candidate;
                    }
                }
            }

            if (best_length >= config.min_copy_length && best_length > 0) {
                CopyInstruction copy;
                copy.target_row = target_row;
                copy.target_col = target_col;
                copy.base_row = best_pos.row;
                copy.base_col = best_pos.col;
                copy.length = best_length;
                AppendCopyInstruction(instructions, copy);
                target_col += best_length;
                continue;
            }

            AppendInsertInstruction(instructions, target_row, target_col, *target_block);
            ++target_col;
        }
    }
}

}  // namespace

const MultiDctBlock *BlockMatrix::GetBlock(int row, int col) const {
    if (row < 0 || col < 0 || row >= height_in_blocks || col >= width_in_blocks) {
        return nullptr;
    }
    const int index = row * width_in_blocks + col;
    return &blocks[static_cast<size_t>(index)];
}

MultiDctBlock *BlockMatrix::GetBlock(int row, int col) {
    if (row < 0 || col < 0 || row >= height_in_blocks || col >= width_in_blocks) {
        return nullptr;
    }
    const int index = row * width_in_blocks + col;
    return &blocks[static_cast<size_t>(index)];
}

bool BuildBlockMatrixFromLuma(
    const phase1::JpegDctImage &image,
    BlockMatrix *out_matrix,
    std::string *error_message) {
    if (out_matrix == nullptr) {
        SetError(error_message, "out_matrix is null.");
        return false;
    }

    if (image.num_components <= 0) {
        SetError(error_message, "Invalid image: no components.");
        return false;
    }

    // enforce 4:4:4 sampling (all components sampling factors == 1)
    const phase1::DctComponent *luma = phase1::GetLumaComponent(image, nullptr, error_message);
    if (luma == nullptr) {
        return false;
    }

    for (const phase1::DctComponent &comp : image.components) {
        if (comp.h_samp_factor != 1 || comp.v_samp_factor != 1) {
            SetError(error_message, "Only 4:4:4 sampling (h_samp_factor==1 && v_samp_factor==1) is supported for multi-channel mode.");
            return false;
        }
        if (comp.width_in_blocks != luma->width_in_blocks || comp.height_in_blocks != luma->height_in_blocks) {
            SetError(error_message, "Component block geometry mismatch across components.");
            return false;
        }
    }

    BlockMatrix matrix;
    matrix.width_in_blocks = luma->width_in_blocks;
    matrix.height_in_blocks = luma->height_in_blocks;
    matrix.blocks.resize(static_cast<size_t>(matrix.width_in_blocks * matrix.height_in_blocks));

    for (int row = 0; row < matrix.height_in_blocks; ++row) {
        for (int col = 0; col < matrix.width_in_blocks; ++col) {
            MultiDctBlock mb;
            mb.components.reserve(static_cast<size_t>(image.num_components));
            for (int c = 0; c < image.num_components; ++c) {
                const phase1::DctComponent &comp = image.components[static_cast<size_t>(c)];
                const phase1::DctBlock *blk = comp.GetBlock(row, col);
                if (blk == nullptr) {
                    SetError(error_message, "Failed to access component block while building block matrix.");
                    return false;
                }
                mb.components.push_back(*blk);
            }
            matrix.blocks[static_cast<size_t>(row * matrix.width_in_blocks + col)] = std::move(mb);
        }
    }

    *out_matrix = std::move(matrix);
    return true;
}

bool GenerateDeltaInstructions(
    const BlockMatrix &base,
    const BlockMatrix &target,
    const CoordinateHint &hint,
    const Phase3Config &config,
    DeltaResult *out_result,
    std::string *error_message) {
    if (out_result == nullptr) {
        SetError(error_message, "out_result is null.");
        return false;
    }
    if (!MatrixShapeValid(base) || !MatrixShapeValid(target)) {
        SetError(error_message, "Invalid block matrix shape.");
        return false;
    }
    if (base.width_in_blocks != target.width_in_blocks ||
        base.height_in_blocks != target.height_in_blocks) {
        SetError(error_message, "Base/target matrix dimensions do not match.");
        return false;
    }
    if (config.min_copy_length <= 0) {
        SetError(error_message, "min_copy_length must be positive.");
        return false;
    }

    DeltaResult result;
    result.width_in_blocks = target.width_in_blocks;
    result.height_in_blocks = target.height_in_blocks;
    result.route = SelectRoute(base, target, hint, config);

    if (result.route == MatchRoute::kFpm) {
        GenerateFpmInstructions(base, target, config, &result.instructions);
    } else {
        GenerateDCHashInstructions(base, target, config, &result.instructions);
    }

    if (!ValidateInstructionsBounds(
            result,
            result.width_in_blocks,
            result.height_in_blocks,
            error_message)) {
        return false;
    }

    FillSimilarityStats(base, target, config, &result);

    *out_result = std::move(result);
    return true;
}

bool ReconstructTargetFromInstructions(
    const BlockMatrix &base,
    const DeltaResult &delta,
    BlockMatrix *out_target,
    std::string *error_message) {
    if (out_target == nullptr) {
        SetError(error_message, "out_target is null.");
        return false;
    }
    if (!MatrixShapeValid(base)) {
        SetError(error_message, "Invalid base matrix.");
        return false;
    }
    if (delta.width_in_blocks <= 0 || delta.height_in_blocks <= 0) {
        SetError(error_message, "Invalid delta dimensions.");
        return false;
    }

    if (!ValidateInstructionsBounds(delta, delta.width_in_blocks, delta.height_in_blocks, error_message)) {
        return false;
    }

    BlockMatrix target;
    target.width_in_blocks = delta.width_in_blocks;
    target.height_in_blocks = delta.height_in_blocks;
    target.blocks.resize(static_cast<size_t>(target.width_in_blocks * target.height_in_blocks));
    std::vector<uint8_t> filled(target.blocks.size(), 0);

    for (const Instruction &instruction : delta.instructions) {
        if (instruction.kind == InstructionKind::kCopy) {
            for (int i = 0; i < instruction.copy.length; ++i) {
                const int tr = instruction.copy.target_row;
                const int tc = instruction.copy.target_col + i;
                const int br = instruction.copy.base_row;
                const int bc = instruction.copy.base_col + i;
                const MultiDctBlock *src = base.GetBlock(br, bc);
                MultiDctBlock *dst = target.GetBlock(tr, tc);
                if (src == nullptr || dst == nullptr) {
                    SetError(error_message, "COPY instruction points out of matrix bounds.");
                    return false;
                }
                *dst = *src;
                filled[static_cast<size_t>(tr * target.width_in_blocks + tc)] = 1;
            }
        } else {
            const int tr = instruction.insert.target_row;
            const int tc = instruction.insert.target_col;
            MultiDctBlock *dst = target.GetBlock(tr, tc);
            if (dst == nullptr) {
                SetError(error_message, "INSERT instruction points out of matrix bounds.");
                return false;
            }
            *dst = instruction.insert.block;
            filled[static_cast<size_t>(tr * target.width_in_blocks + tc)] = 1;
        }
    }

    for (size_t i = 0; i < filled.size(); ++i) {
        if (filled[i] == 0) {
            SetError(error_message, "Delta stream does not fully cover the target matrix.");
            return false;
        }
    }

    *out_target = std::move(target);
    return true;
}

bool ValidateInstructionsBounds(
    const DeltaResult &delta,
    int width_in_blocks,
    int height_in_blocks,
    std::string *error_message) {
    if (width_in_blocks <= 0 || height_in_blocks <= 0) {
        SetError(error_message, "Invalid width/height for instruction validation.");
        return false;
    }

    for (const Instruction &instruction : delta.instructions) {
        if (instruction.kind == InstructionKind::kCopy) {
            const CopyInstruction &copy = instruction.copy;
            if (copy.length <= 0) {
                SetError(error_message, "COPY length must be positive.");
                return false;
            }
            if (copy.target_row < 0 || copy.target_row >= height_in_blocks ||
                copy.base_row < 0 || copy.base_row >= height_in_blocks ||
                copy.target_col < 0 || copy.base_col < 0 ||
                copy.target_col + copy.length > width_in_blocks ||
                copy.base_col + copy.length > width_in_blocks) {
                SetError(error_message, "COPY instruction exceeds matrix boundary.");
                return false;
            }
        } else {
            const InsertInstruction &insert = instruction.insert;
            if (insert.target_row < 0 || insert.target_row >= height_in_blocks ||
                insert.target_col < 0 || insert.target_col >= width_in_blocks) {
                SetError(error_message, "INSERT instruction exceeds matrix boundary.");
                return false;
            }
        }
    }

    return true;
}

}  // namespace phase3
} 
