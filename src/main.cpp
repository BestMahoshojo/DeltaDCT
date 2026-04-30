#include "phase1_dct_codec.hpp"
#include "phase2_similarity.hpp"
#include "phase3_delta.hpp"
#include "phase4_storage.hpp"
#include "phase5_restore.hpp"
#include "phase6_benchmark.hpp"

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

using deltadct::phase1::DctComponent;
using deltadct::phase1::DumpLumaBlocksToText;
using deltadct::phase1::JpegDctImage;
using deltadct::phase1::LoadLumaBlocksFromText;
using deltadct::phase1::ReadJpegDctImage;
using deltadct::phase1::ReplaceLumaComponent;
using deltadct::phase1::WriteJpegDctImage;
using deltadct::phase2::BuildSignature;
using deltadct::phase2::DumpSignatureToText;
using deltadct::phase2::InMemoryInvertedIndex;
using deltadct::phase2::MatchResult;
using deltadct::phase2::Phase2Config;
using deltadct::phase2::Signature;
using deltadct::phase3::BlockMatrix;
using deltadct::phase3::CoordinateHint;
using deltadct::phase3::DeltaResult;
using deltadct::phase3::GenerateDeltaInstructions;
using deltadct::phase3::MatchRoute;
using deltadct::phase3::Phase3Config;
using deltadct::phase4::BlockCodec;
using deltadct::phase4::BuildPackageFromDelta;
using deltadct::phase4::DeltaDctPackage;
using deltadct::phase4::IsZstdEnabledAtBuild;
using deltadct::phase4::ReadPackageFromFile;
using deltadct::phase4::WritePackageToFile;
using deltadct::phase5::RestoreReport;
using deltadct::phase5::RestoreTargetJpegFromPackage;
using deltadct::phase6::BenchmarkOptions;
using deltadct::phase6::BenchmarkResult;
using deltadct::phase6::RunBenchmarkOnPair;

namespace {

bool ParsePositiveInt(const std::string &text, const char *name, int *out_value) {
    if (out_value == nullptr) {
        return false;
    }
    try {
        const int value = std::stoi(text);
        if (value <= 0) {
            std::cerr << name << " must be positive.\n";
            return false;
        }
        *out_value = value;
        return true;
    } catch (...) {
        std::cerr << "Invalid integer for " << name << ": " << text << "\n";
        return false;
    }
}

bool ParseNonNegativeInt(const std::string &text, const char *name, int *out_value) {
    if (out_value == nullptr) {
        return false;
    }
    try {
        const int value = std::stoi(text);
        if (value < 0) {
            std::cerr << name << " must be non-negative.\n";
            return false;
        }
        *out_value = value;
        return true;
    } catch (...) {
        std::cerr << "Invalid integer for " << name << ": " << text << "\n";
        return false;
    }
}

const char *MatchRouteName(MatchRoute route) {
    return route == MatchRoute::kFpm ? "FPM" : "DCHash";
}

const char *BlockCodecName(BlockCodec codec) {
    return codec == BlockCodec::kZstd ? "zstd" : "raw";
}

void PrintUsage() {
    std::cerr << "Usage:\n"
              << "  deltadct_tool dump-y <input.jpg> <output.txt>\n"
              << "  deltadct_tool repack-same <reference.jpg> <output.jpg>\n"
              << "  deltadct_tool repack-y <reference.jpg> <luma.txt> <output.jpg>\n"
              << "  deltadct_tool phase2-signature <input.jpg> <output.txt> [window_size] [num_transforms]\n"
              << "  deltadct_tool phase2-best-base <target.jpg> <window_size> <num_transforms> <candidate1.jpg> [candidate2.jpg ...]\n"
              << "  deltadct_tool phase4-pack <base_id> <base.jpg> <target.jpg> <output.ddct> [target_row target_col base_row base_col]\n"
              << "  deltadct_tool phase5-restore <base.jpg> <input.ddct> <output_target.jpg> [reference_target.jpg]\n"
              << "  deltadct_tool phase6-benchmark <base.jpg> <target.jpg> <output_dir> [window_size num_transforms zstd_level]\n";
}

int RunDumpY(const std::string &input_jpeg, const std::string &output_text) {
    JpegDctImage image;
    std::string error_message;

    if (!ReadJpegDctImage(input_jpeg, &image, &error_message)) {
        std::cerr << "ReadJpegDctImage failed: " << error_message << "\n";
        return 1;
    }
    if (!DumpLumaBlocksToText(image, output_text, &error_message)) {
        std::cerr << "DumpLumaBlocksToText failed: " << error_message << "\n";
        return 1;
    }

    std::cout << "Luma DCT matrix saved to: " << output_text << "\n";
    return 0;
}

int RunRepackSame(const std::string &reference_jpeg, const std::string &output_jpeg) {
    JpegDctImage image;
    std::string error_message;

    if (!ReadJpegDctImage(reference_jpeg, &image, &error_message)) {
        std::cerr << "ReadJpegDctImage failed: " << error_message << "\n";
        return 1;
    }
    if (!WriteJpegDctImage(reference_jpeg, image, output_jpeg, &error_message)) {
        std::cerr << "WriteJpegDctImage failed: " << error_message << "\n";
        return 1;
    }

    std::cout << "JPEG repacked from DCT coefficients: " << output_jpeg << "\n";
    return 0;
}

int RunRepackY(
    const std::string &reference_jpeg,
    const std::string &luma_matrix_path,
    const std::string &output_jpeg) {
    JpegDctImage image;
    DctComponent luma;
    std::string error_message;

    if (!ReadJpegDctImage(reference_jpeg, &image, &error_message)) {
        std::cerr << "ReadJpegDctImage failed: " << error_message << "\n";
        return 1;
    }
    if (!LoadLumaBlocksFromText(luma_matrix_path, &luma, &error_message)) {
        std::cerr << "LoadLumaBlocksFromText failed: " << error_message << "\n";
        return 1;
    }
    if (!ReplaceLumaComponent(&image, luma, &error_message)) {
        std::cerr << "ReplaceLumaComponent failed: " << error_message << "\n";
        return 1;
    }
    if (!WriteJpegDctImage(reference_jpeg, image, output_jpeg, &error_message)) {
        std::cerr << "WriteJpegDctImage failed: " << error_message << "\n";
        return 1;
    }

    std::cout << "JPEG repacked using external luma DCT matrix: " << output_jpeg << "\n";
    return 0;
}

int RunPhase2Signature(
    const std::string &input_jpeg,
    const std::string &output_text,
    int window_size,
    int num_transforms) {
    JpegDctImage image;
    std::string error_message;
    Phase2Config config;
    Signature signature;

    config.window_size = window_size;
    config.num_transforms = num_transforms;

    if (!ReadJpegDctImage(input_jpeg, &image, &error_message)) {
        std::cerr << "ReadJpegDctImage failed: " << error_message << "\n";
        return 1;
    }
    if (!BuildSignature(image, config, &signature, &error_message)) {
        std::cerr << "BuildSignature failed: " << error_message << "\n";
        return 1;
    }
    if (!DumpSignatureToText(signature, output_text, &error_message)) {
        std::cerr << "DumpSignatureToText failed: " << error_message << "\n";
        return 1;
    }

    std::cout << "Phase2 signature saved to: " << output_text << "\n"
              << "luma_component_index=" << signature.luma_component_index << ", "
              << "feature_count=" << signature.features.size() << "\n";
    return 0;
}

int RunPhase2BestBase(
    const std::string &target_jpeg,
    int window_size,
    int num_transforms,
    const std::vector<std::string> &candidate_jpegs) {
    if (candidate_jpegs.empty()) {
        std::cerr << "No candidate images provided.\n";
        return 1;
    }

    std::string error_message;
    Phase2Config config;
    config.window_size = window_size;
    config.num_transforms = num_transforms;

    InMemoryInvertedIndex index;
    for (const std::string &candidate_path : candidate_jpegs) {
        JpegDctImage candidate_image;
        Signature candidate_signature;
        if (!ReadJpegDctImage(candidate_path, &candidate_image, &error_message)) {
            std::cerr << "ReadJpegDctImage failed for candidate " << candidate_path
                      << ": " << error_message << "\n";
            return 1;
        }
        if (!BuildSignature(candidate_image, config, &candidate_signature, &error_message)) {
            std::cerr << "BuildSignature failed for candidate " << candidate_path
                      << ": " << error_message << "\n";
            return 1;
        }
        index.AddImageSignature(candidate_path, candidate_signature);
    }

    JpegDctImage target_image;
    Signature target_signature;
    if (!ReadJpegDctImage(target_jpeg, &target_image, &error_message)) {
        std::cerr << "ReadJpegDctImage failed for target: " << error_message << "\n";
        return 1;
    }
    if (!BuildSignature(target_image, config, &target_signature, &error_message)) {
        std::cerr << "BuildSignature failed for target: " << error_message << "\n";
        return 1;
    }

    const MatchResult result = index.QueryBestMatch(target_signature, target_jpeg);
    if (result.best_image_id.empty() || result.best_score <= 0) {
        std::cout << "No matching base image found (best_score=0).\n";
        return 0;
    }

    std::cout << "Best base image: " << result.best_image_id
              << " (score=" << result.best_score << ")\n";
    return 0;
}

int RunPhase4Pack(
    const std::string &base_id,
    const std::string &base_jpeg,
    const std::string &target_jpeg,
    const std::string &output_deltadct,
    const CoordinateHint &hint) {
    JpegDctImage base_image;
    JpegDctImage target_image;
    std::string error_message;

    if (!ReadJpegDctImage(base_jpeg, &base_image, &error_message)) {
        std::cerr << "ReadJpegDctImage(base) failed: " << error_message << "\n";
        return 1;
    }
    if (!ReadJpegDctImage(target_jpeg, &target_image, &error_message)) {
        std::cerr << "ReadJpegDctImage(target) failed: " << error_message << "\n";
        return 1;
    }

    BlockMatrix base_matrix;
    BlockMatrix target_matrix;
    if (!deltadct::phase3::BuildBlockMatrixFromLuma(base_image, &base_matrix, &error_message)) {
        std::cerr << "BuildBlockMatrixFromLuma(base) failed: " << error_message << "\n";
        return 1;
    }
    if (!deltadct::phase3::BuildBlockMatrixFromLuma(target_image, &target_matrix, &error_message)) {
        std::cerr << "BuildBlockMatrixFromLuma(target) failed: " << error_message << "\n";
        return 1;
    }

    Phase3Config config;
    DeltaResult delta;
    if (!GenerateDeltaInstructions(base_matrix, target_matrix, hint, config, &delta, &error_message)) {
        std::cerr << "GenerateDeltaInstructions failed: " << error_message << "\n";
        return 1;
    }

    DeltaDctPackage package;
    if (!BuildPackageFromDelta(base_id, delta, &package, &error_message)) {
        std::cerr << "BuildPackageFromDelta failed: " << error_message << "\n";
        return 1;
    }
    if (!WritePackageToFile(output_deltadct, package, &error_message)) {
        std::cerr << "WritePackageToFile failed: " << error_message << "\n";
        return 1;
    }

    DeltaDctPackage parsed_package;
    if (!ReadPackageFromFile(output_deltadct, &parsed_package, &error_message)) {
        std::cerr << "ReadPackageFromFile failed after write: " << error_message << "\n";
        return 1;
    }

    int copy_count = 0;
    int insert_count = 0;
    for (const auto &instruction : delta.instructions) {
        if (instruction.kind == deltadct::phase3::InstructionKind::kCopy) {
            ++copy_count;
        } else {
            ++insert_count;
        }
    }

    std::cout << "Phase4 package written: " << output_deltadct << "\n"
              << "base_id=" << base_id << ", route=" << MatchRouteName(delta.route)
              << ", block_codec=" << BlockCodecName(parsed_package.block_codec)
              << ", zstd_build=" << (IsZstdEnabledAtBuild() ? "enabled" : "disabled")
              << ", instructions=" << delta.instructions.size()
              << " (copy=" << copy_count << ", insert=" << insert_count << ")\n";
    return 0;
}

int RunPhase5Restore(
    const std::string &base_jpeg,
    const std::string &input_deltadct,
    const std::string &output_target_jpeg,
    const std::string &reference_target_jpeg) {
    RestoreReport report;
    std::string error_message;
    if (!RestoreTargetJpegFromPackage(
            base_jpeg,
            input_deltadct,
            output_target_jpeg,
            reference_target_jpeg,
            &report,
            &error_message)) {
        std::cerr << "RestoreTargetJpegFromPackage failed: " << error_message << "\n";
        return 1;
    }

    std::cout << "Phase5 restore output: " << output_target_jpeg << "\n"
              << "instructions=" << report.instruction_count
              << " (copy=" << report.copy_count << ", insert=" << report.insert_count << ")\n";

    if (report.verification_requested) {
        std::cout << "verification.luma_dct="
                  << (report.luma_dct_verification_passed ? "passed" : "failed")
                  << "\n"
                  << "verification.bytewise="
                  << (report.bytewise_verification_passed ? "passed" : "failed")
                  << "\n"
                  << "hash.reference_fnv1a64=0x"
                  << std::hex << report.reference_file_hash << std::dec << "\n"
                  << "hash.restored_fnv1a64=0x"
                  << std::hex << report.restored_file_hash << std::dec << "\n";
        return (report.luma_dct_verification_passed && report.bytewise_verification_passed) ? 0 : 2;
    }

    return 0;
}

int RunPhase6Benchmark(
    const std::string &base_jpeg,
    const std::string &target_jpeg,
    const std::string &output_dir,
    int window_size,
    int num_transforms,
    int zstd_level) {
    BenchmarkOptions options;
    options.base_jpeg_path = base_jpeg;
    options.target_jpeg_path = target_jpeg;
    options.output_dir = output_dir;
    options.phase2_config.window_size = window_size;
    options.phase2_config.num_transforms = num_transforms;
    options.zstd_level = zstd_level;

    BenchmarkResult result;
    std::string error_message;
    if (!RunBenchmarkOnPair(options, &result, &error_message)) {
        std::cerr << "RunBenchmarkOnPair failed: " << error_message << "\n";
        return 1;
    }

    std::cout << "[Router] Using "
              << (result.route == MatchRoute::kFpm ? "FPM Branch" : "DCHash Branch")
              << "\n";
    std::cout << "Original Size (bytes): " << result.original_size_bytes << "\n";

    if (result.raw_metrics.generated) {
        std::cout << "Dedup Size RAW (bytes): " << result.raw_metrics.package_size_bytes << "\n"
                  << "Compression Ratio RAW: " << result.raw_metrics.compression_ratio << "\n"
                  << "RAW Package Path: " << result.raw_metrics.package_path << "\n";
    }
    if (result.zstd_metrics.generated) {
        std::cout << "Dedup Size ZSTD (bytes): " << result.zstd_metrics.package_size_bytes << "\n"
                  << "Compression Ratio ZSTD: " << result.zstd_metrics.compression_ratio << "\n"
                  << "ZSTD Package Path: " << result.zstd_metrics.package_path << "\n";
    } else {
        std::cout << "Dedup Size ZSTD: unavailable (build without zstd support)\n";
    }

    std::cout << "Similarity Detector Time (s): " << result.similarity_seconds << "\n"
              << "Idelta Compressor Time (s): " << result.idelta_seconds << "\n"
              << "Total Time (s): " << result.total_seconds << "\n"
              << "Throughput (MB/s): " << result.throughput_mb_s << "\n";

    std::cout << "Restored JPEG Path: " << result.restored_jpeg_path << "\n"
              << "Restore Check Luma-DCT: "
              << (result.restored_luma_dct_equal ? "passed" : "failed") << "\n"
              << "Restore Check Bytewise: "
              << (result.restored_bytewise_equal ? "passed" : "failed") << "\n"
              << "Reference Hash FNV1a64: 0x" << std::hex << result.restored_reference_hash << std::dec << "\n"
              << "Restored Hash FNV1a64: 0x" << std::hex << result.restored_output_hash << std::dec << "\n";

    return (result.restored_luma_dct_equal && result.restored_bytewise_equal) ? 0 : 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::string command = argv[1];
    if (command == "dump-y") {
        if (argc != 4) {
            PrintUsage();
            return 1;
        }
        return RunDumpY(argv[2], argv[3]);
    }

    if (command == "repack-same") {
        if (argc != 4) {
            PrintUsage();
            return 1;
        }
        return RunRepackSame(argv[2], argv[3]);
    }

    if (command == "repack-y") {
        if (argc != 5) {
            PrintUsage();
            return 1;
        }
        return RunRepackY(argv[2], argv[3], argv[4]);
    }

    if (command == "phase2-signature") {
        if (argc != 4 && argc != 6) {
            PrintUsage();
            return 1;
        }
        int window_size = 2;
        int num_transforms = 10;
        if (argc == 6) {
            if (!ParsePositiveInt(argv[4], "window_size", &window_size) ||
                !ParsePositiveInt(argv[5], "num_transforms", &num_transforms)) {
                return 1;
            }
        }
        return RunPhase2Signature(argv[2], argv[3], window_size, num_transforms);
    }

    if (command == "phase2-best-base") {
        if (argc < 6) {
            PrintUsage();
            return 1;
        }

        int window_size = 0;
        int num_transforms = 0;
        if (!ParsePositiveInt(argv[3], "window_size", &window_size) ||
            !ParsePositiveInt(argv[4], "num_transforms", &num_transforms)) {
            return 1;
        }

        std::vector<std::string> candidates;
        for (int i = 5; i < argc; ++i) {
            candidates.push_back(argv[i]);
        }
        return RunPhase2BestBase(argv[2], window_size, num_transforms, candidates);
    }

    if (command == "phase4-pack") {
        if (argc != 6 && argc != 10) {
            PrintUsage();
            return 1;
        }

        CoordinateHint hint;
        if (argc == 10) {
            hint.valid = true;
            if (!ParseNonNegativeInt(argv[6], "target_row", &hint.target_row) ||
                !ParseNonNegativeInt(argv[7], "target_col", &hint.target_col) ||
                !ParseNonNegativeInt(argv[8], "base_row", &hint.base_row) ||
                !ParseNonNegativeInt(argv[9], "base_col", &hint.base_col)) {
                return 1;
            }
        }

        return RunPhase4Pack(argv[2], argv[3], argv[4], argv[5], hint);
    }

    if (command == "phase5-restore") {
        if (argc != 5 && argc != 6) {
            PrintUsage();
            return 1;
        }

        const std::string reference = (argc == 6) ? argv[5] : "";
        return RunPhase5Restore(argv[2], argv[3], argv[4], reference);
    }

    if (command == "phase6-benchmark") {
        if (argc != 5 && argc != 8) {
            PrintUsage();
            return 1;
        }

        int window_size = 2;
        int num_transforms = 10;
        int zstd_level = 3;
        if (argc == 8) {
            if (!ParsePositiveInt(argv[5], "window_size", &window_size) ||
                !ParsePositiveInt(argv[6], "num_transforms", &num_transforms) ||
                !ParsePositiveInt(argv[7], "zstd_level", &zstd_level)) {
                return 1;
            }
        }

        return RunPhase6Benchmark(argv[2], argv[3], argv[4], window_size, num_transforms, zstd_level);
    }

    PrintUsage();
    return 1;
}
