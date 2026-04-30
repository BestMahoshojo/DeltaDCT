#include "phase4_storage.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifdef DELTADCT_HAVE_ZSTD
#include <zstd.h>
#endif

namespace deltadct {
namespace phase4 {

namespace {

constexpr uint32_t kMagicNumber = 0x44444354u;  // "DDCT"
constexpr std::array<uint8_t, 8> kMagic = {
    static_cast<uint8_t>((kMagicNumber >> 24) & 0xFFu),
    static_cast<uint8_t>((kMagicNumber >> 16) & 0xFFu),
    static_cast<uint8_t>((kMagicNumber >> 8) & 0xFFu),
    static_cast<uint8_t>(kMagicNumber & 0xFFu),
    static_cast<uint8_t>((kMagicNumber >> 24) & 0xFFu),
    static_cast<uint8_t>((kMagicNumber >> 16) & 0xFFu),
    static_cast<uint8_t>((kMagicNumber >> 8) & 0xFFu),
    static_cast<uint8_t>(kMagicNumber & 0xFFu),
};
constexpr uint32_t kFormatVersion = 5;
constexpr uint8_t kMetadataCodecVarint = 1;
constexpr uint8_t kBlockCodecRaw = 0;
constexpr uint8_t kBlockCodecZstd = 1;
constexpr std::array<int, 64> kZigZag = {
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

void SetError(std::string *error_message, const std::string &message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

struct Header {
    std::array<uint8_t, 8> magic{};
    uint32_t version = 0;
    uint8_t route = 0;
    uint8_t metadata_codec = 0;
    uint8_t block_codec = 0;
    uint8_t reserved = 0;
    uint32_t width_in_blocks = 0;
    uint32_t height_in_blocks = 0;
    uint32_t instruction_count = 0;
    uint32_t insert_count = 0;
    uint32_t base_id_size = 0;
    uint32_t header_prefix_size = 0;
    uint32_t metadata_size = 0;
    uint32_t block_size = 0;
    uint32_t num_components = 1;
};

void AppendU32(std::vector<uint8_t> *buffer, uint32_t value) {
    buffer->push_back(static_cast<uint8_t>(value & 0xFFu));
    buffer->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    buffer->push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    buffer->push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

bool ReadU32(const std::vector<uint8_t> &buffer, size_t *offset, uint32_t *out_value) {
    if (offset == nullptr || out_value == nullptr) {
        return false;
    }
    if (*offset + 4 > buffer.size()) {
        return false;
    }
    const uint32_t value = static_cast<uint32_t>(buffer[*offset]) |
                           (static_cast<uint32_t>(buffer[*offset + 1]) << 8) |
                           (static_cast<uint32_t>(buffer[*offset + 2]) << 16) |
                           (static_cast<uint32_t>(buffer[*offset + 3]) << 24);
    *offset += 4;
    *out_value = value;
    return true;
}

void AppendVarint(std::vector<uint8_t> *buffer, uint32_t value) {
    while (value >= 0x80u) {
        buffer->push_back(static_cast<uint8_t>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    buffer->push_back(static_cast<uint8_t>(value));
}

void AppendU16(std::vector<uint8_t> *buffer, uint16_t value) {
    buffer->push_back(static_cast<uint8_t>(value & 0xFFu));
    buffer->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

bool ReadU16(const std::vector<uint8_t> &buffer, size_t *offset, uint16_t *out_value) {
    if (offset == nullptr || out_value == nullptr) return false;
    if (*offset + 2 > buffer.size()) return false;
    const uint16_t value = static_cast<uint16_t>(buffer[*offset]) | (static_cast<uint16_t>(buffer[*offset + 1]) << 8);
    *offset += 2;
    *out_value = value;
    return true;
}

void AppendI16(std::vector<uint8_t> *buffer, int16_t value) {
    AppendU16(buffer, static_cast<uint16_t>(value));
}

bool ReadVarint(const std::vector<uint8_t> &buffer, size_t *offset, uint32_t *out_value) {
    if (offset == nullptr || out_value == nullptr) {
        return false;
    }

    uint32_t value = 0;
    int shift = 0;
    while (*offset < buffer.size()) {
        const uint8_t byte = buffer[*offset];
        ++(*offset);

        value |= static_cast<uint32_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            *out_value = value;
            return true;
        }

        shift += 7;
        if (shift > 28) {
            return false;
        }
    }

    return false;
}

bool ToU32NonNegative(int value, uint32_t *out_value) {
    if (out_value == nullptr || value < 0) {
        return false;
    }
    *out_value = static_cast<uint32_t>(value);
    return true;
}

bool ToIntWithinRange(uint32_t value, int *out_value) {
    if (out_value == nullptr) {
        return false;
    }
    if (value > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    *out_value = static_cast<int>(value);
    return true;
}

#ifdef DELTADCT_HAVE_ZSTD
int ClampZstdLevel(int level) {
    if (level < ZSTD_minCLevel()) {
        return ZSTD_minCLevel();
    }
    if (level > ZSTD_maxCLevel()) {
        return ZSTD_maxCLevel();
    }
    return level;
}
#endif

bool EncodeBlockSection(
    const std::vector<uint8_t> &raw_block_section,
    const StorageOptions &options,
    const DeltaDctPackage &package,
    BlockCodec *out_codec,
    std::vector<uint8_t> *out_encoded,
    std::string *error_message) {
    if (out_codec == nullptr || out_encoded == nullptr) {
        SetError(error_message, "EncodeBlockSection output pointer is null.");
        return false;
    }

    BlockCodecPreference preference = options.block_codec_preference;
    if (preference == BlockCodecPreference::kAuto && package.block_codec == BlockCodec::kZstd) {
        preference = BlockCodecPreference::kZstd;
    }

    if (preference == BlockCodecPreference::kRaw || raw_block_section.empty()) {
        *out_codec = BlockCodec::kRaw;
        *out_encoded = raw_block_section;
        return true;
    }

#ifdef DELTADCT_HAVE_ZSTD
    std::vector<uint8_t> compressed;
    const size_t bound = ZSTD_compressBound(raw_block_section.size());
    compressed.resize(bound);
    const int zstd_level = ZSTD_maxCLevel();
    const size_t compressed_size =
        ZSTD_compress(
            compressed.data(),
            compressed.size(),
            raw_block_section.data(),
            raw_block_section.size(),
            zstd_level);
    if (ZSTD_isError(compressed_size) != 0) {
        if (preference == BlockCodecPreference::kZstd) {
            SetError(error_message, "ZSTD_compress failed for block section.");
            return false;
        }

        *out_codec = BlockCodec::kRaw;
        *out_encoded = raw_block_section;
        return true;
    }
    compressed.resize(compressed_size);

    if (preference == BlockCodecPreference::kZstd || compressed.size() < raw_block_section.size()) {
        *out_codec = BlockCodec::kZstd;
        *out_encoded = std::move(compressed);
        return true;
    }

    *out_codec = BlockCodec::kRaw;
    *out_encoded = raw_block_section;
    return true;
#else
    if (preference == BlockCodecPreference::kZstd) {
        SetError(error_message, "This build does not enable zstd codec.");
        return false;
    }

    *out_codec = BlockCodec::kRaw;
    *out_encoded = raw_block_section;
    return true;
#endif
}

bool DecodeBlockSection(
    BlockCodec codec,
    const std::vector<uint8_t> &encoded_block_section,
    size_t expected_raw_size,
    std::vector<uint8_t> *out_raw_block_section,
    std::string *error_message) {
    if (out_raw_block_section == nullptr) {
        SetError(error_message, "out_raw_block_section is null.");
        return false;
    }

    if (codec == BlockCodec::kRaw) {
        if (encoded_block_section.size() != expected_raw_size) {
            SetError(error_message, "RAW block section size mismatch.");
            return false;
        }
        *out_raw_block_section = encoded_block_section;
        return true;
    }

    if (codec == BlockCodec::kZstd) {
#ifdef DELTADCT_HAVE_ZSTD
        size_t decoded_size = expected_raw_size;
        if (decoded_size == 0) {
            decoded_size = ZSTD_getFrameContentSize(encoded_block_section.data(), encoded_block_section.size());
            if (decoded_size == ZSTD_CONTENTSIZE_ERROR || decoded_size == ZSTD_CONTENTSIZE_UNKNOWN) {
                SetError(error_message, "Failed to determine zstd block section size.");
                return false;
            }
        }
        std::vector<uint8_t> raw(decoded_size);
        const size_t decompressed_size =
            ZSTD_decompress(
                raw.data(),
                raw.size(),
                encoded_block_section.data(),
                encoded_block_section.size());
        if (ZSTD_isError(decompressed_size) != 0) {
            SetError(error_message, "ZSTD_decompress failed for block section.");
            return false;
        }
        if (decompressed_size != decoded_size) {
            SetError(error_message, "Decoded zstd block section size mismatch.");
            return false;
        }
        *out_raw_block_section = std::move(raw);
        return true;
#else
        SetError(error_message, "Package uses zstd block codec, but this build has no zstd support.");
        return false;
#endif
    }

    SetError(error_message, "Unsupported block codec in package header.");
    return false;
}

bool SerializeMetadata(
    const DeltaDctPackage &package,
    std::vector<uint8_t> *metadata,
    std::vector<uint8_t> *block_section,
    uint32_t *insert_count,
    size_t *copy_meta_bytes,
    size_t *insert_raw_bytes,
    std::string *error_message) {
    if (metadata == nullptr || block_section == nullptr || insert_count == nullptr) {
        SetError(error_message, "SerializeMetadata output pointer is null.");
        return false;
    }

    if (copy_meta_bytes != nullptr) {
        *copy_meta_bytes = 0;
    }
    if (insert_raw_bytes != nullptr) {
        *insert_raw_bytes = 0;
    }

    // First, merge consecutive COPY instructions when possible to reduce metadata
    std::vector<phase3::Instruction> merged;
    merged.reserve(package.instructions.size());
    for (const phase3::Instruction &instr : package.instructions) {
        if (instr.kind == phase3::InstructionKind::kCopy && !merged.empty() &&
            merged.back().kind == phase3::InstructionKind::kCopy) {
            phase3::CopyInstruction &prev = merged.back().copy;
            const phase3::CopyInstruction &cur = instr.copy;
            // merge when base row/col match and both target and base positions are contiguous
            if (prev.base_row == cur.base_row &&
                prev.base_col + prev.length == cur.base_col &&
                prev.target_row == cur.target_row &&
                prev.target_col + prev.length == cur.target_col) {
                prev.length += cur.length;
                continue; // merged
            }
        }
        merged.push_back(instr);
    }

    std::vector<int16_t> insert_buffer;
    uint32_t local_insert_count = 0;
    for (const phase3::Instruction &instruction : merged) {
        if (instruction.kind == phase3::InstructionKind::kCopy) {
            const size_t before = metadata->size();
            AppendVarint(metadata, 0u);

            uint32_t tr = 0;
            uint32_t tc = 0;
            uint32_t br = 0;
            uint32_t bc = 0;
            uint32_t len = 0;
            if (!ToU32NonNegative(instruction.copy.target_row, &tr) ||
                !ToU32NonNegative(instruction.copy.target_col, &tc) ||
                !ToU32NonNegative(instruction.copy.base_row, &br) ||
                !ToU32NonNegative(instruction.copy.base_col, &bc) ||
                !ToU32NonNegative(instruction.copy.length, &len) || len == 0) {
                SetError(error_message, "Invalid COPY instruction for serialization.");
                return false;
            }

            // If values fit in 16 bits, use compact 2-byte encoding for coordinates/length
            if (tr <= 0xFFFFu && tc <= 0xFFFFu && br <= 0xFFFFu && bc <= 0xFFFFu && len <= 0xFFFFu) {
                metadata->push_back(static_cast<uint8_t>(1u)); // subtype: u16 encoding
                AppendU16(metadata, static_cast<uint16_t>(tr));
                AppendU16(metadata, static_cast<uint16_t>(tc));
                AppendU16(metadata, static_cast<uint16_t>(br));
                AppendU16(metadata, static_cast<uint16_t>(bc));
                AppendU16(metadata, static_cast<uint16_t>(len));
            } else {
                metadata->push_back(static_cast<uint8_t>(0u)); // subtype: varint encoding
                AppendVarint(metadata, tr);
                AppendVarint(metadata, tc);
                AppendVarint(metadata, br);
                AppendVarint(metadata, bc);
                AppendVarint(metadata, len);
            }

            if (copy_meta_bytes != nullptr) {
                *copy_meta_bytes += metadata->size() - before;
            }
            continue;
        }

        uint32_t tr = 0;
        uint32_t tc = 0;
        if (!ToU32NonNegative(instruction.insert.target_row, &tr) ||
            !ToU32NonNegative(instruction.insert.target_col, &tc)) {
            SetError(error_message, "Invalid INSERT instruction for serialization.");
            return false;
        }

        AppendVarint(metadata, 1u);
        if (tr <= 0xFFFFu && tc <= 0xFFFFu) {
            metadata->push_back(static_cast<uint8_t>(1u));
            AppendU16(metadata, static_cast<uint16_t>(tr));
            AppendU16(metadata, static_cast<uint16_t>(tc));
        } else {
            metadata->push_back(static_cast<uint8_t>(0u));
            AppendVarint(metadata, tr);
            AppendVarint(metadata, tc);
        }

        // Write all components' coefficients (component order preserved)
        if (instruction.insert.block.components.empty()) {
            SetError(error_message, "INSERT instruction contains no components.");
            return false;
        }
        for (size_t comp_idx = 0; comp_idx < instruction.insert.block.components.size(); ++comp_idx) {
            const phase1::DctBlock &comp_block = instruction.insert.block.components[comp_idx];
            for (int zigzag_idx = 0; zigzag_idx < phase1::kDctCoeffsPerBlock; ++zigzag_idx) {
                insert_buffer.push_back(
                    comp_block.coeff[static_cast<size_t>(kZigZag[static_cast<size_t>(zigzag_idx)])]);
            }
        }
        ++local_insert_count;
    }

    block_section->reserve(insert_buffer.size() * 2);
    for (int16_t value : insert_buffer) {
        AppendI16(block_section, value);
    }
    if (insert_raw_bytes != nullptr) {
        *insert_raw_bytes = block_section->size();
    }

    *insert_count = local_insert_count;
    return true;
}

bool ParseMetadataAndBlock(
    const Header &header,
    const std::vector<uint8_t> &metadata,
    const std::vector<uint8_t> &block_section,
    std::vector<phase3::Instruction> *out_instructions,
    std::string *error_message) {
    if (out_instructions == nullptr) {
        SetError(error_message, "out_instructions is null.");
        return false;
    }

    std::vector<phase3::Instruction> instructions;
    instructions.reserve(static_cast<size_t>(header.instruction_count));
    std::vector<size_t> insert_positions;
    insert_positions.reserve(static_cast<size_t>(header.insert_count));

    size_t offset = 0;
    for (uint32_t i = 0; i < header.instruction_count; ++i) {
        uint32_t kind = 0;
        if (!ReadVarint(metadata, &offset, &kind)) {
            SetError(error_message, "Failed to decode instruction kind from metadata.");
            return false;
        }

        if (kind == 0u) {
            // Read subtype byte to know whether values are u16 or varint
            if (offset >= metadata.size()) {
                SetError(error_message, "Truncated metadata while reading COPY subtype.");
                return false;
            }
            const uint8_t subtype = metadata[offset++];

            uint32_t tr = 0;
            uint32_t tc = 0;
            uint32_t br = 0;
            uint32_t bc = 0;
            uint32_t len = 0;
            if (subtype == 1u) {
                uint16_t vtr, vtc, vbr, vbc, vlen;
                if (!ReadU16(metadata, &offset, &vtr) || !ReadU16(metadata, &offset, &vtc) ||
                    !ReadU16(metadata, &offset, &vbr) || !ReadU16(metadata, &offset, &vbc) ||
                    !ReadU16(metadata, &offset, &vlen) || vlen == 0) {
                    SetError(error_message, "Failed to decode compact COPY instruction from metadata.");
                    return false;
                }
                tr = vtr; tc = vtc; br = vbr; bc = vbc; len = vlen;
            } else {
                if (!ReadVarint(metadata, &offset, &tr) ||
                    !ReadVarint(metadata, &offset, &tc) ||
                    !ReadVarint(metadata, &offset, &br) ||
                    !ReadVarint(metadata, &offset, &bc) ||
                    !ReadVarint(metadata, &offset, &len) || len == 0) {
                    SetError(error_message, "Failed to decode COPY instruction from metadata.");
                    return false;
                }
            }

            phase3::Instruction instruction;
            instruction.kind = phase3::InstructionKind::kCopy;
            if (!ToIntWithinRange(tr, &instruction.copy.target_row) ||
                !ToIntWithinRange(tc, &instruction.copy.target_col) ||
                !ToIntWithinRange(br, &instruction.copy.base_row) ||
                !ToIntWithinRange(bc, &instruction.copy.base_col) ||
                !ToIntWithinRange(len, &instruction.copy.length)) {
                SetError(error_message, "COPY instruction value exceeds int range.");
                return false;
            }
            instructions.push_back(instruction);
            continue;
        }

        if (kind == 1u) {
            if (offset >= metadata.size()) {
                SetError(error_message, "Truncated metadata while reading INSERT subtype.");
                return false;
            }
            const uint8_t subtype = metadata[offset++];

            uint32_t tr = 0;
            uint32_t tc = 0;
            if (subtype == 1u) {
                uint16_t vtr = 0;
                uint16_t vtc = 0;
                if (!ReadU16(metadata, &offset, &vtr) ||
                    !ReadU16(metadata, &offset, &vtc)) {
                    SetError(error_message, "Failed to decode compact INSERT instruction from metadata.");
                    return false;
                }
                tr = vtr;
                tc = vtc;
            } else {
                if (!ReadVarint(metadata, &offset, &tr) ||
                    !ReadVarint(metadata, &offset, &tc)) {
                    SetError(error_message, "Failed to decode INSERT instruction from metadata.");
                    return false;
                }
            }

            phase3::Instruction instruction;
            instruction.kind = phase3::InstructionKind::kInsert;
            if (!ToIntWithinRange(tr, &instruction.insert.target_row) ||
                !ToIntWithinRange(tc, &instruction.insert.target_col)) {
                SetError(error_message, "INSERT instruction value exceeds int range.");
                return false;
            }

            instructions.push_back(instruction);
            insert_positions.push_back(instructions.size() - 1);
            continue;
        }

        SetError(error_message, "Unknown instruction kind in metadata.");
        return false;
    }

    if (offset != metadata.size()) {
        SetError(error_message, "Metadata contains trailing bytes.");
        return false;
    }
    if (insert_positions.size() != static_cast<size_t>(header.insert_count)) {
        SetError(error_message, "INSERT count mismatch between header and metadata.");
        return false;
    }

    size_t block_offset = 0;
    for (const size_t idx : insert_positions) {
        const size_t per_insert_bytes = static_cast<size_t>(phase1::kDctCoeffsPerBlock) * 2 *
            static_cast<size_t>(header.num_components);
        if (block_offset + per_insert_bytes > block_section.size()) {
            SetError(error_message, "INSERT block exceeds block section size.");
            return false;
        }

        phase3::Instruction &instruction = instructions[idx];
        instruction.insert.block.components.clear();
        instruction.insert.block.components.resize(static_cast<size_t>(header.num_components));
        for (uint32_t comp = 0; comp < header.num_components; ++comp) {
            phase1::DctBlock &dst_block = instruction.insert.block.components[static_cast<size_t>(comp)];
            for (int i = 0; i < phase1::kDctCoeffsPerBlock; ++i) dst_block.coeff[static_cast<size_t>(i)] = 0;
            for (int zigzag_idx = 0; zigzag_idx < phase1::kDctCoeffsPerBlock; ++zigzag_idx) {
                uint16_t raw_value = 0;
                if (!ReadU16(block_section, &block_offset, &raw_value)) {
                    SetError(error_message, "Failed to read INSERT block coefficients.");
                    return false;
                }
                dst_block.coeff[static_cast<size_t>(kZigZag[static_cast<size_t>(zigzag_idx)])] =
                    static_cast<int16_t>(raw_value);
            }
        }
    }

    if (block_offset != block_section.size()) {
        SetError(error_message, "Block section contains trailing bytes.");
        return false;
    }

    *out_instructions = std::move(instructions);
    return true;
}

}  // namespace

bool IsZstdEnabledAtBuild() {
#ifdef DELTADCT_HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

bool BuildPackageFromDelta(
    const std::string &base_id,
    const phase3::DeltaResult &delta,
    DeltaDctPackage *out_package,
    std::string *error_message) {
    if (out_package == nullptr) {
        SetError(error_message, "out_package is null.");
        return false;
    }
    if (delta.width_in_blocks <= 0 || delta.height_in_blocks <= 0) {
        SetError(error_message, "Delta dimensions must be positive.");
        return false;
    }

    DeltaDctPackage package;
    package.base_id = base_id;
    package.route = delta.route;
    package.block_codec = BlockCodec::kRaw;
    package.width_in_blocks = delta.width_in_blocks;
    package.height_in_blocks = delta.height_in_blocks;
    package.instructions = delta.instructions;
    *out_package = std::move(package);
    return true;
}

bool PackageToDelta(
    const DeltaDctPackage &package,
    phase3::DeltaResult *out_delta,
    std::string *error_message) {
    if (out_delta == nullptr) {
        SetError(error_message, "out_delta is null.");
        return false;
    }
    if (package.width_in_blocks <= 0 || package.height_in_blocks <= 0) {
        SetError(error_message, "Package dimensions must be positive.");
        return false;
    }

    phase3::DeltaResult delta;
    delta.route = package.route;
    delta.width_in_blocks = package.width_in_blocks;
    delta.height_in_blocks = package.height_in_blocks;
    delta.instructions = package.instructions;
    *out_delta = std::move(delta);
    return true;
}

bool SerializePackage(
    const DeltaDctPackage &package,
    std::vector<uint8_t> *out_binary,
    std::string *error_message,
    const StorageOptions &options) {
    if (out_binary == nullptr) {
        SetError(error_message, "out_binary is null.");
        return false;
    }
    if (package.width_in_blocks <= 0 || package.height_in_blocks <= 0) {
        SetError(error_message, "Package dimensions must be positive.");
        return false;
    }

        std::vector<uint8_t> metadata;
        std::vector<uint8_t> insert_block_section;
    uint32_t insert_count = 0;
        size_t copy_meta_bytes = 0;
        size_t insert_raw_bytes = 0;
    if (!SerializeMetadata(
            package,
            &metadata,
            &insert_block_section,
            &insert_count,
            &copy_meta_bytes,
            &insert_raw_bytes,
            error_message)) {
        return false;
    }

        std::vector<uint8_t> raw_payload;
        raw_payload.reserve(package.header_prefix.size() + metadata.size() + insert_block_section.size());
        raw_payload.insert(raw_payload.end(), package.header_prefix.begin(), package.header_prefix.end());
        raw_payload.insert(raw_payload.end(), metadata.begin(), metadata.end());
        raw_payload.insert(raw_payload.end(), insert_block_section.begin(), insert_block_section.end());

    BlockCodec chosen_codec = BlockCodec::kRaw;
    std::vector<uint8_t> encoded_block_section;
    if (!EncodeBlockSection(
            raw_payload,
            options,
            package,
            &chosen_codec,
            &encoded_block_section,
            error_message)) {
        return false;
    }

    Header header;
    header.magic = kMagic;
    header.version = kFormatVersion;
    header.route = static_cast<uint8_t>(package.route == phase3::MatchRoute::kDCHash ? 1 : 0);
    header.metadata_codec = kMetadataCodecVarint;
    header.block_codec = (chosen_codec == BlockCodec::kZstd) ? kBlockCodecZstd : kBlockCodecRaw;
    header.reserved = 0;

    if (!ToU32NonNegative(package.width_in_blocks, &header.width_in_blocks) ||
        !ToU32NonNegative(package.height_in_blocks, &header.height_in_blocks)) {
        SetError(error_message, "Package dimensions out of supported range.");
        return false;
    }

    if (package.instructions.size() > std::numeric_limits<uint32_t>::max() ||
        package.base_id.size() > std::numeric_limits<uint32_t>::max() ||
        package.header_prefix.size() > std::numeric_limits<uint32_t>::max() ||
        metadata.size() > std::numeric_limits<uint32_t>::max() ||
        encoded_block_section.size() > std::numeric_limits<uint32_t>::max()) {
        SetError(error_message, "Package size exceeds uint32 limits.");
        return false;
    }

    header.instruction_count = static_cast<uint32_t>(package.instructions.size());
    header.insert_count = insert_count;
    header.base_id_size = static_cast<uint32_t>(package.base_id.size());
    header.header_prefix_size = static_cast<uint32_t>(package.header_prefix.size());
    header.metadata_size = static_cast<uint32_t>(metadata.size());
    header.block_size = static_cast<uint32_t>(insert_raw_bytes);
    header.num_components = static_cast<uint32_t>(package.num_components);

    std::vector<uint8_t> binary;
    binary.reserve(
        8 + 4 + 4 + 4 * 8 +
        package.base_id.size() +
        encoded_block_section.size());

    binary.insert(binary.end(), header.magic.begin(), header.magic.end());
    AppendU32(&binary, header.version);
    binary.push_back(header.route);
    binary.push_back(header.metadata_codec);
    binary.push_back(header.block_codec);
    binary.push_back(header.reserved);
    AppendU32(&binary, header.num_components);
    AppendU32(&binary, header.width_in_blocks);
    AppendU32(&binary, header.height_in_blocks);
    AppendU32(&binary, header.instruction_count);
    AppendU32(&binary, header.insert_count);
    AppendU32(&binary, header.base_id_size);
    AppendU32(&binary, header.header_prefix_size);
    AppendU32(&binary, header.metadata_size);
    AppendU32(&binary, header.block_size);

    binary.insert(binary.end(), package.base_id.begin(), package.base_id.end());
    binary.insert(binary.end(), encoded_block_section.begin(), encoded_block_section.end());

    *out_binary = std::move(binary);
    return true;
}

bool DeserializePackage(
    const std::vector<uint8_t> &binary,
    DeltaDctPackage *out_package,
    std::string *error_message) {
    if (out_package == nullptr) {
        SetError(error_message, "out_package is null.");
        return false;
    }

    size_t offset = 0;
    Header header;

    if (binary.size() < 8 + 4 + 4 + 4 * 8) {
        SetError(error_message, "Binary package is too small.");
        return false;
    }

    std::copy(binary.begin(), binary.begin() + 8, header.magic.begin());
    offset += 8;

    if (header.magic != kMagic) {
        SetError(error_message, "Invalid package magic for .ddct file.");
        return false;
    }

    if (!ReadU32(binary, &offset, &header.version)) {
        SetError(error_message, "Failed to read package version.");
        return false;
    }
    if (header.version != kFormatVersion) {
        SetError(error_message, "Unsupported .ddct format version.");
        return false;
    }

    if (offset + 4 > binary.size()) {
        SetError(error_message, "Truncated package after version.");
        return false;
    }
    header.route = binary[offset++];
    header.metadata_codec = binary[offset++];
    header.block_codec = binary[offset++];
    header.reserved = binary[offset++];

    if (header.metadata_codec != kMetadataCodecVarint) {
        SetError(error_message, "Unsupported metadata codec in package.");
        return false;
    }

    if (header.block_codec != kBlockCodecRaw && header.block_codec != kBlockCodecZstd) {
        SetError(error_message, "Unsupported block codec in package header.");
        return false;
    }

    if (!ReadU32(binary, &offset, &header.num_components) ||
        !ReadU32(binary, &offset, &header.width_in_blocks) ||
        !ReadU32(binary, &offset, &header.height_in_blocks) ||
        !ReadU32(binary, &offset, &header.instruction_count) ||
        !ReadU32(binary, &offset, &header.insert_count) ||
        !ReadU32(binary, &offset, &header.base_id_size) ||
        !ReadU32(binary, &offset, &header.header_prefix_size) ||
        !ReadU32(binary, &offset, &header.metadata_size) ||
        !ReadU32(binary, &offset, &header.block_size)) {
        SetError(error_message, "Failed to read package header fields.");
        return false;
    }

    const uint64_t remain = static_cast<uint64_t>(binary.size() - offset);
    if (static_cast<uint64_t>(header.base_id_size) > remain) {
        SetError(error_message, "Header payload sizes do not match binary size.");
        return false;
    }

    if (header.width_in_blocks == 0 || header.height_in_blocks == 0) {
        SetError(error_message, "Invalid dimensions in package header.");
        return false;
    }

    std::string base_id(
        reinterpret_cast<const char *>(&binary[offset]),
        reinterpret_cast<const char *>(&binary[offset + header.base_id_size]));
    offset += header.base_id_size;

    std::vector<uint8_t> encoded_block_section(
        binary.begin() + static_cast<std::ptrdiff_t>(offset),
        binary.end());

    BlockCodec block_codec =
        (header.block_codec == kBlockCodecZstd) ? BlockCodec::kZstd : BlockCodec::kRaw;
    std::vector<uint8_t> raw_block_section;
    if (!DecodeBlockSection(
            block_codec,
            encoded_block_section,
            static_cast<size_t>(header.header_prefix_size) + static_cast<size_t>(header.metadata_size) +
                static_cast<size_t>(header.block_size),
            &raw_block_section,
            error_message)) {
        return false;
    }

    if (raw_block_section.size() < static_cast<size_t>(header.header_prefix_size) +
            static_cast<size_t>(header.metadata_size)) {
        SetError(error_message, "Decoded payload is smaller than expected.");
        return false;
    }

    std::vector<uint8_t> header_prefix(
        raw_block_section.begin(),
        raw_block_section.begin() + static_cast<std::ptrdiff_t>(header.header_prefix_size));
    std::vector<uint8_t> metadata(
        raw_block_section.begin() + static_cast<std::ptrdiff_t>(header.header_prefix_size),
        raw_block_section.begin() + static_cast<std::ptrdiff_t>(header.header_prefix_size + header.metadata_size));
    std::vector<uint8_t> insert_section(
        raw_block_section.begin() + static_cast<std::ptrdiff_t>(header.header_prefix_size + header.metadata_size),
        raw_block_section.end());

    std::vector<phase3::Instruction> instructions;
    if (!ParseMetadataAndBlock(header, metadata, insert_section, &instructions, error_message)) {
        return false;
    }

    DeltaDctPackage package;
    package.base_id = std::move(base_id);
    package.header_prefix = std::move(header_prefix);
    package.route = (header.route == 1u) ? phase3::MatchRoute::kDCHash : phase3::MatchRoute::kFpm;
    package.block_codec = block_codec;
    if (!ToIntWithinRange(header.width_in_blocks, &package.width_in_blocks) ||
        !ToIntWithinRange(header.height_in_blocks, &package.height_in_blocks)) {
        SetError(error_message, "Package dimension exceeds int range.");
        return false;
    }
    package.instructions = std::move(instructions);

    *out_package = std::move(package);
    return true;
}

bool WritePackageToFile(
    const std::string &output_path,
    const DeltaDctPackage &package,
    std::string *error_message,
    const StorageOptions &options) {
    std::vector<uint8_t> binary;
    if (!SerializePackage(package, &binary, error_message, options)) {
        return false;
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        SetError(error_message, "Failed to open output .ddct file: " + output_path);
        return false;
    }
    output.write(reinterpret_cast<const char *>(binary.data()), static_cast<std::streamsize>(binary.size()));
    if (!output.good()) {
        SetError(error_message, "Failed to write complete .ddct file: " + output_path);
        return false;
    }
    return true;
}

bool ReadPackageFromFile(
    const std::string &input_path,
    DeltaDctPackage *out_package,
    std::string *error_message) {
    if (out_package == nullptr) {
        SetError(error_message, "out_package is null.");
        return false;
    }

    std::ifstream input(input_path, std::ios::binary | std::ios::in);
    if (!input.is_open()) {
        SetError(error_message, "Failed to open input .ddct file: " + input_path);
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        SetError(error_message, "Failed to read .ddct file size: " + input_path);
        return false;
    }
    input.seekg(0, std::ios::beg);

    std::vector<uint8_t> binary(static_cast<size_t>(size));
    if (size > 0) {
        input.read(reinterpret_cast<char *>(binary.data()), size);
        if (!input.good()) {
            SetError(error_message, "Failed to read complete .ddct file: " + input_path);
            return false;
        }
    }

    return DeserializePackage(binary, out_package, error_message);
}

}  // namespace phase4
}  // namespace deltadct
