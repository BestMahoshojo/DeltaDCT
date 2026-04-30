#include "phase5_restore.hpp"

#include <fstream>
#include <string>
#include <vector>

namespace deltadct {
namespace phase5 {

namespace {

void SetError(std::string *error_message, const std::string &message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

bool BlockMatricesEqual(const phase3::BlockMatrix &lhs, const phase3::BlockMatrix &rhs) {
    if (lhs.width_in_blocks != rhs.width_in_blocks ||
        lhs.height_in_blocks != rhs.height_in_blocks ||
        lhs.blocks.size() != rhs.blocks.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.blocks.size(); ++i) {
        const phase3::MultiDctBlock &L = lhs.blocks[i];
        const phase3::MultiDctBlock &R = rhs.blocks[i];
        if (L.components.size() != R.components.size()) return false;
        for (size_t c = 0; c < L.components.size(); ++c) {
            if (L.components[c].coeff != R.components[c].coeff) return false;
        }
    }
    return true;
}

phase1::JpegDctImage BuildImageFromMatrix(
    const phase1::JpegDctImage &template_image,
    const phase3::BlockMatrix &matrix,
    std::string *error_message) {
    phase1::JpegDctImage out = template_image;
    if (matrix.width_in_blocks <= 0 || matrix.height_in_blocks <= 0) {
        SetError(error_message, "Invalid matrix dimensions for image build.");
        return out;
    }
    const int num_comp = template_image.num_components;
    out.image_width = template_image.image_width;
    out.image_height = template_image.image_height;
    out.num_components = num_comp;
    out.components.clear();
    out.components.resize(static_cast<size_t>(num_comp));

    for (int c = 0; c < num_comp; ++c) {
        const phase1::DctComponent &tpl = template_image.components[static_cast<size_t>(c)];
        phase1::DctComponent comp = tpl;
        comp.width_in_blocks = matrix.width_in_blocks;
        comp.height_in_blocks = matrix.height_in_blocks;
        comp.blocks.resize(static_cast<size_t>(comp.width_in_blocks * comp.height_in_blocks));
        out.components[static_cast<size_t>(c)] = std::move(comp);
    }

    for (int row = 0; row < matrix.height_in_blocks; ++row) {
        for (int col = 0; col < matrix.width_in_blocks; ++col) {
            const phase3::MultiDctBlock *mb = matrix.GetBlock(row, col);
            if (mb == nullptr) {
                SetError(error_message, "Matrix missing block while building image.");
                return out;
            }
            if (static_cast<int>(mb->components.size()) != num_comp) {
                SetError(error_message, "Component count mismatch while building image.");
                return out;
            }
            for (int c = 0; c < num_comp; ++c) {
                phase1::DctComponent &comp = out.components[static_cast<size_t>(c)];
                phase1::DctBlock *dst = comp.GetBlock(row, col);
                if (dst == nullptr) {
                    SetError(error_message, "Failed to access destination block while building image.");
                    return out;
                }
                *dst = mb->components[static_cast<size_t>(c)];
            }
        }
    }

    return out;
}

uint64_t HashFNV1a64(const std::vector<uint8_t> &data) {
    uint64_t hash = 1469598103934665603ULL;
    for (uint8_t byte : data) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool ReadWholeFile(
    const std::string &path,
    std::vector<uint8_t> *out_data,
    std::string *error_message) {
    if (out_data == nullptr) {
        SetError(error_message, "out_data is null.");
        return false;
    }

    std::ifstream input(path, std::ios::binary | std::ios::in);
    if (!input.is_open()) {
        SetError(error_message, "Failed to open file: " + path);
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        SetError(error_message, "Failed to read file size: " + path);
        return false;
    }
    input.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0) {
        input.read(reinterpret_cast<char *>(data.data()), size);
        if (!input.good()) {
            SetError(error_message, "Failed to read complete file: " + path);
            return false;
        }
    }

    *out_data = std::move(data);
    return true;
}

}  // namespace

bool RestoreTargetMatrixFromPackage(
    const phase3::BlockMatrix &base_matrix,
    const phase4::DeltaDctPackage &package,
    phase3::BlockMatrix *out_target_matrix,
    std::string *error_message) {
    if (out_target_matrix == nullptr) {
        SetError(error_message, "out_target_matrix is null.");
        return false;
    }

    phase3::DeltaResult delta;
    if (!phase4::PackageToDelta(package, &delta, error_message)) {
        return false;
    }

    if (delta.width_in_blocks != base_matrix.width_in_blocks ||
        delta.height_in_blocks != base_matrix.height_in_blocks) {
        SetError(error_message, "Package/base matrix dimension mismatch for restoration.");
        return false;
    }

    return phase3::ReconstructTargetFromInstructions(base_matrix, delta, out_target_matrix, error_message);
}

bool VerifyLumaDctMatrixEqual(
    const std::string &jpeg_a,
    const std::string &jpeg_b,
    bool *out_equal,
    std::string *error_message) {
    if (out_equal == nullptr) {
        SetError(error_message, "out_equal is null.");
        return false;
    }

    phase1::JpegDctImage image_a;
    phase1::JpegDctImage image_b;
    if (!phase1::ReadJpegDctImage(jpeg_a, &image_a, error_message)) {
        return false;
    }
    if (!phase1::ReadJpegDctImage(jpeg_b, &image_b, error_message)) {
        return false;
    }

    phase3::BlockMatrix matrix_a;
    phase3::BlockMatrix matrix_b;
    if (!phase3::BuildBlockMatrixFromLuma(image_a, &matrix_a, error_message)) {
        return false;
    }
    if (!phase3::BuildBlockMatrixFromLuma(image_b, &matrix_b, error_message)) {
        return false;
    }

    *out_equal = BlockMatricesEqual(matrix_a, matrix_b);
    return true;
}

bool VerifyFilesBytewiseEqual(
    const std::string &file_a,
    const std::string &file_b,
    bool *out_equal,
    uint64_t *out_hash_a,
    uint64_t *out_hash_b,
    std::string *error_message) {
    if (out_equal == nullptr) {
        SetError(error_message, "out_equal is null.");
        return false;
    }
    if (out_hash_a == nullptr || out_hash_b == nullptr) {
        SetError(error_message, "hash output pointer is null.");
        return false;
    }

    std::vector<uint8_t> data_a;
    std::vector<uint8_t> data_b;
    if (!ReadWholeFile(file_a, &data_a, error_message)) {
        return false;
    }
    if (!ReadWholeFile(file_b, &data_b, error_message)) {
        return false;
    }

    *out_hash_a = HashFNV1a64(data_a);
    *out_hash_b = HashFNV1a64(data_b);
    *out_equal = (data_a == data_b);
    return true;
}

bool RestoreTargetJpegFromPackage(
    const std::string &base_jpeg_path,
    const std::string &deltadct_path,
    const std::string &output_target_jpeg_path,
    const std::string &reference_target_jpeg_path,
    RestoreReport *out_report,
    std::string *error_message) {
    if (out_report == nullptr) {
        SetError(error_message, "out_report is null.");
        return false;
    }

    phase4::DeltaDctPackage package;
    if (!phase4::ReadPackageFromFile(deltadct_path, &package, error_message)) {
        return false;
    }

    phase1::JpegDctImage base_image;
    if (!phase1::ReadJpegDctImage(base_jpeg_path, &base_image, error_message)) {
        return false;
    }

    const phase1::DctComponent *base_luma = phase1::GetLumaComponent(base_image, nullptr, error_message);
    if (base_luma == nullptr) {
        return false;
    }

    phase3::BlockMatrix base_matrix;
    if (!phase3::BuildBlockMatrixFromLuma(base_image, &base_matrix, error_message)) {
        return false;
    }

    phase3::BlockMatrix target_matrix;
    if (!RestoreTargetMatrixFromPackage(base_matrix, package, &target_matrix, error_message)) {
        return false;
    }

    phase1::JpegDctImage restored_image = BuildImageFromMatrix(base_image, target_matrix, error_message);
    if (!error_message->empty()) {
        return false;
    }
    if (!phase1::WriteJpegDctImage(base_jpeg_path, restored_image, output_target_jpeg_path, error_message)) {
        return false;
    }

    if (!package.header_prefix.empty()) {
        if (!phase1::ApplyJpegHeaderPrefixBeforeSos(
                output_target_jpeg_path,
                package.header_prefix,
                error_message)) {
            return false;
        }
    }

    RestoreReport report;
    report.instruction_count = static_cast<int>(package.instructions.size());
    for (const phase3::Instruction &instruction : package.instructions) {
        if (instruction.kind == phase3::InstructionKind::kCopy) {
            ++report.copy_count;
        } else {
            ++report.insert_count;
        }
    }

    if (!reference_target_jpeg_path.empty()) {
        report.verification_requested = true;
        bool dct_equal = false;
        if (!VerifyLumaDctMatrixEqual(
                reference_target_jpeg_path,
                output_target_jpeg_path,
                &dct_equal,
                error_message)) {
            return false;
        }

        bool bytewise_equal = false;
        uint64_t ref_hash = 0;
        uint64_t restored_hash = 0;
        if (!VerifyFilesBytewiseEqual(
                reference_target_jpeg_path,
                output_target_jpeg_path,
                &bytewise_equal,
                &ref_hash,
                &restored_hash,
                error_message)) {
            return false;
        }

        report.luma_dct_verification_passed = dct_equal;
        report.bytewise_verification_passed = bytewise_equal;
        report.reference_file_hash = ref_hash;
        report.restored_file_hash = restored_hash;
    }

    *out_report = report;
    return true;
}

}  // namespace phase5
}  // namespace deltadct
