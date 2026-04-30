#include "phase6_benchmark.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "phase1_dct_codec.hpp"
#include "phase4_storage.hpp"
#include "phase5_restore.hpp"

namespace deltadct {
namespace phase6 {

namespace {

void SetError(std::string *error_message, const std::string &message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

double SecondsBetween(
    const std::chrono::high_resolution_clock::time_point &begin,
    const std::chrono::high_resolution_clock::time_point &end) {
    const auto duration =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - begin);
    return duration.count();
}

bool EnsureDirectory(const std::string &path, std::string *error_message) {
    std::error_code ec;
    const std::filesystem::path dir(path);
    if (std::filesystem::exists(dir, ec)) {
        if (ec) {
            SetError(error_message, "Failed to query output directory: " + path);
            return false;
        }
        if (!std::filesystem::is_directory(dir, ec)) {
            SetError(error_message, "Output path exists but is not a directory: " + path);
            return false;
        }
        return true;
    }

    if (!std::filesystem::create_directories(dir, ec)) {
        if (ec) {
            SetError(error_message, "Failed to create output directory: " + path);
            return false;
        }
    }
    return true;
}

bool GetFileSizeU64(const std::string &path, uint64_t *out_size, std::string *error_message) {
    if (out_size == nullptr) {
        SetError(error_message, "out_size is null.");
        return false;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        SetError(error_message, "Failed to get file size: " + path);
        return false;
    }
    *out_size = static_cast<uint64_t>(size);
    return true;
}

phase3::CoordinateHint DeriveCoordinateHint(
    const phase2::Signature &base_signature,
    const phase2::Signature &target_signature) {
    phase3::CoordinateHint hint;
    if (base_signature.features.empty() || target_signature.features.empty()) {
        return hint;
    }

    size_t best_idx = 0;
    size_t min_size = std::min(base_signature.features.size(), target_signature.features.size());
    for (size_t i = 0; i < min_size; ++i) {
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

}  // namespace

bool RunBenchmarkOnPair(
    const BenchmarkOptions &options,
    BenchmarkResult *out_result,
    std::string *error_message) {
    if (out_result == nullptr) {
        SetError(error_message, "out_result is null.");
        return false;
    }
    if (options.base_jpeg_path.empty() || options.target_jpeg_path.empty() ||
        options.output_dir.empty()) {
        SetError(error_message, "base/target/output_dir must not be empty.");
        return false;
    }

    if (!EnsureDirectory(options.output_dir, error_message)) {
        return false;
    }

    uint64_t original_size = 0;
    if (!GetFileSizeU64(options.target_jpeg_path, &original_size, error_message)) {
        return false;
    }

    phase1::JpegDctImage base_image;
    phase1::JpegDctImage target_image;
    if (!phase1::ReadJpegDctImage(options.base_jpeg_path, &base_image, error_message)) {
        return false;
    }
    if (!phase1::ReadJpegDctImage(options.target_jpeg_path, &target_image, error_message)) {
        return false;
    }

    phase2::Signature base_signature;
    phase2::Signature target_signature;
    phase2::InMemoryInvertedIndex detector_index;
    const auto similarity_begin = std::chrono::high_resolution_clock::now();
    if (!phase2::BuildSignature(base_image, options.phase2_config, &base_signature, error_message)) {
        return false;
    }
    if (!phase2::BuildSignature(target_image, options.phase2_config, &target_signature, error_message)) {
        return false;
    }
    detector_index.AddImageSignature(options.base_jpeg_path, base_signature);
    (void)detector_index.QueryBestMatch(target_signature, options.target_jpeg_path);
    const auto similarity_end = std::chrono::high_resolution_clock::now();

    const phase3::CoordinateHint hint = DeriveCoordinateHint(base_signature, target_signature);

    phase3::BlockMatrix base_matrix;
    phase3::BlockMatrix target_matrix;
    phase3::DeltaResult delta;
    const auto delta_begin = std::chrono::high_resolution_clock::now();
    if (!phase3::BuildBlockMatrixFromLuma(base_image, &base_matrix, error_message)) {
        return false;
    }
    if (!phase3::BuildBlockMatrixFromLuma(target_image, &target_matrix, error_message)) {
        return false;
    }
    if (!phase3::GenerateDeltaInstructions(
            base_matrix,
            target_matrix,
            hint,
            options.phase3_config,
            &delta,
            error_message)) {
        return false;
    }
    const auto delta_end = std::chrono::high_resolution_clock::now();

    phase4::DeltaDctPackage package;
    if (!phase4::BuildPackageFromDelta(options.base_jpeg_path, delta, &package, error_message)) {
        return false;
    }

    const std::filesystem::path out_dir(options.output_dir);
    const std::string raw_path = (out_dir / "target.raw.ddct").string();
    const std::string zstd_path = (out_dir / "target.zstd.ddct").string();
    const std::string restored_path = (out_dir / "target.restored.jpg").string();

    phase4::StorageOptions raw_options;
    raw_options.block_codec_preference = phase4::BlockCodecPreference::kRaw;
    if (!phase4::WritePackageToFile(raw_path, package, error_message, raw_options)) {
        return false;
    }

    uint64_t raw_size = 0;
    if (!GetFileSizeU64(raw_path, &raw_size, error_message)) {
        return false;
    }

    CodecMetrics raw_metrics;
    raw_metrics.generated = true;
    raw_metrics.package_path = raw_path;
    raw_metrics.package_size_bytes = raw_size;
    raw_metrics.compression_ratio =
        (raw_size == 0) ? 0.0 : (static_cast<double>(original_size) / static_cast<double>(raw_size));

    CodecMetrics zstd_metrics;
    std::string restore_source_path = raw_path;
    if (phase4::IsZstdEnabledAtBuild()) {
        phase4::StorageOptions zstd_options;
        zstd_options.block_codec_preference = phase4::BlockCodecPreference::kZstd;
        zstd_options.zstd_level = options.zstd_level;

        if (!phase4::WritePackageToFile(zstd_path, package, error_message, zstd_options)) {
            return false;
        }

        uint64_t zstd_size = 0;
        if (!GetFileSizeU64(zstd_path, &zstd_size, error_message)) {
            return false;
        }

        zstd_metrics.generated = true;
        zstd_metrics.package_path = zstd_path;
        zstd_metrics.package_size_bytes = zstd_size;
        zstd_metrics.compression_ratio =
            (zstd_size == 0) ? 0.0 : (static_cast<double>(original_size) / static_cast<double>(zstd_size));

        restore_source_path = zstd_path;
    }

    phase5::RestoreReport restore_report;
    if (!phase5::RestoreTargetJpegFromPackage(
            options.base_jpeg_path,
            restore_source_path,
            restored_path,
            options.target_jpeg_path,
            &restore_report,
            error_message)) {
        return false;
    }

    const double similarity_seconds = SecondsBetween(similarity_begin, similarity_end);
    const double idelta_seconds = SecondsBetween(delta_begin, delta_end);
    const double total_seconds = similarity_seconds + idelta_seconds;
    const double original_mb = static_cast<double>(original_size) / (1024.0 * 1024.0);

    BenchmarkResult result;
    result.original_size_bytes = original_size;
    result.similarity_seconds = similarity_seconds;
    result.idelta_seconds = idelta_seconds;
    result.total_seconds = total_seconds;
    result.throughput_mb_s = (total_seconds > 0.0) ? (original_mb / total_seconds) : 0.0;
    result.route = delta.route;
    result.raw_metrics = raw_metrics;
    result.zstd_metrics = zstd_metrics;
    result.restored_luma_dct_equal = restore_report.luma_dct_verification_passed;
    result.restored_bytewise_equal = restore_report.bytewise_verification_passed;
    result.restored_reference_hash = restore_report.reference_file_hash;
    result.restored_output_hash = restore_report.restored_file_hash;
    result.restored_jpeg_path = restored_path;

    *out_result = std::move(result);
    return true;
}

}  // namespace phase6
}  // namespace deltadct
