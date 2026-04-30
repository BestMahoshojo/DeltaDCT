#ifndef DELTADCT_PHASE5_RESTORE_HPP
#define DELTADCT_PHASE5_RESTORE_HPP

#include <string>

#include "phase1_dct_codec.hpp"
#include "phase3_delta.hpp"
#include "phase4_storage.hpp"

namespace deltadct {
namespace phase5 {

struct RestoreReport {
    int instruction_count = 0;
    int copy_count = 0;
    int insert_count = 0;
    bool verification_requested = false;
    bool luma_dct_verification_passed = false;
    bool bytewise_verification_passed = false;
    uint64_t reference_file_hash = 0;
    uint64_t restored_file_hash = 0;
};

bool RestoreTargetMatrixFromPackage(
    const phase3::BlockMatrix &base_matrix,
    const phase4::DeltaDctPackage &package,
    phase3::BlockMatrix *out_target_matrix,
    std::string *error_message);

bool VerifyLumaDctMatrixEqual(
    const std::string &jpeg_a,
    const std::string &jpeg_b,
    bool *out_equal,
    std::string *error_message);

bool VerifyFilesBytewiseEqual(
    const std::string &file_a,
    const std::string &file_b,
    bool *out_equal,
    uint64_t *out_hash_a,
    uint64_t *out_hash_b,
    std::string *error_message);

bool RestoreTargetJpegFromPackage(
    const std::string &base_jpeg_path,
    const std::string &deltadct_path,
    const std::string &output_target_jpeg_path,
    const std::string &reference_target_jpeg_path,
    RestoreReport *out_report,
    std::string *error_message);

}  // namespace phase5
}  // namespace deltadct

#endif
