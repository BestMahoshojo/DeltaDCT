#ifndef DELTADCT_PHASE4_STORAGE_HPP
#define DELTADCT_PHASE4_STORAGE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "phase3_delta.hpp"

namespace deltadct {
namespace phase4 {

enum class BlockCodec {
    kRaw = 0,
    kZstd = 1,
};

enum class BlockCodecPreference {
    kAuto = 0,
    kRaw = 1,
    kZstd = 2,
};

struct StorageOptions {
    BlockCodecPreference block_codec_preference = BlockCodecPreference::kAuto;
    // Request a very high default; encode will clamp to the build's ZSTD_maxCLevel().
    int zstd_level = 999;
};

struct DeltaDctPackage {
    std::string base_id;
    std::vector<uint8_t> header_prefix;
    std::vector<uint8_t> raw_target_jpeg;
    phase3::MatchRoute route = phase3::MatchRoute::kFpm;
    BlockCodec block_codec = BlockCodec::kRaw;
    int width_in_blocks = 0;
    int height_in_blocks = 0;
    int num_components = 1;
    std::vector<phase3::Instruction> instructions;
};

bool IsZstdEnabledAtBuild();

bool BuildPackageFromDelta(
    const std::string &base_id,
    const phase3::DeltaResult &delta,
    DeltaDctPackage *out_package,
    std::string *error_message);

bool PackageToDelta(
    const DeltaDctPackage &package,
    phase3::DeltaResult *out_delta,
    std::string *error_message);

bool SerializePackage(
    const DeltaDctPackage &package,
    std::vector<uint8_t> *out_binary,
    std::string *error_message,
    const StorageOptions &options = StorageOptions());

bool DeserializePackage(
    const std::vector<uint8_t> &binary,
    DeltaDctPackage *out_package,
    std::string *error_message);

bool WritePackageToFile(
    const std::string &output_path,
    const DeltaDctPackage &package,
    std::string *error_message,
    const StorageOptions &options = StorageOptions());

bool ReadPackageFromFile(
    const std::string &input_path,
    DeltaDctPackage *out_package,
    std::string *error_message);

}  // namespace phase4
}  // namespace deltadct

#endif
