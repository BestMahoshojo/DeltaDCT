#ifndef DELTADCT_PHASE3_DELTA_HPP
#define DELTADCT_PHASE3_DELTA_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "phase1_dct_codec.hpp"

namespace deltadct {
namespace phase3 {

struct MultiDctBlock {
    std::vector<phase1::DctBlock> components; // per-component DCT blocks, in component order
};

struct BlockMatrix {
    int width_in_blocks = 0;
    int height_in_blocks = 0;
    std::vector<MultiDctBlock> blocks;

    const MultiDctBlock *GetBlock(int row, int col) const;
    MultiDctBlock *GetBlock(int row, int col);
};

enum class InstructionKind {
    kCopy,
    kInsert,
};

enum class MatchRoute {
    kFpm,
    kDCHash,
};

constexpr int kBlockDiffThreshold = 64;

enum class RouteMode {
    kAuto,
    kForceFpm,
    kForceDCHash,
};

struct CopyInstruction {
    int target_row = 0;
    int target_col = 0;
    int base_row = 0;
    int base_col = 0;
    int length = 0;
};

struct InsertInstruction {
    int target_row = 0;
    int target_col = 0;
    MultiDctBlock block;
};

struct Instruction {
    InstructionKind kind = InstructionKind::kInsert;
    CopyInstruction copy;
    InsertInstruction insert;
};

struct CoordinateHint {
    bool valid = false;
    int target_row = 0;
    int target_col = 0;
    int base_row = 0;
    int base_col = 0;
};

struct Phase3Config {
    double fpm_same_position_threshold = 0.6;
    int min_copy_length = 1;
    RouteMode route_mode = RouteMode::kAuto;
    bool strict_lossless = false;
};

struct DeltaResult {
    MatchRoute route = MatchRoute::kFpm;
    int width_in_blocks = 0;
    int height_in_blocks = 0;
    std::vector<Instruction> instructions;
    int blocks_total = 0;
    int blocks_exact = 0;
    int blocks_similar = 0;
    int blocks_different = 0;
    std::vector<uint8_t> block_map;
};

bool BuildBlockMatrixFromLuma(
    const phase1::JpegDctImage &image,
    BlockMatrix *out_matrix,
    std::string *error_message);

bool GenerateDeltaInstructions(
    const BlockMatrix &base,
    const BlockMatrix &target,
    const CoordinateHint &hint,
    const Phase3Config &config,
    DeltaResult *out_result,
    std::string *error_message);

bool ReconstructTargetFromInstructions(
    const BlockMatrix &base,
    const DeltaResult &delta,
    BlockMatrix *out_target,
    std::string *error_message);

bool ValidateInstructionsBounds(
    const DeltaResult &delta,
    int width_in_blocks,
    int height_in_blocks,
    std::string *error_message);

}  // namespace phase3
}  // namespace deltadct

#endif
