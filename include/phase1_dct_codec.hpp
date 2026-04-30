#ifndef DELTADCT_PHASE1_DCT_CODEC_HPP
#define DELTADCT_PHASE1_DCT_CODEC_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace deltadct {
namespace phase1 {

constexpr int kDctCoeffsPerBlock = 64;

struct DctBlock {
    std::array<int16_t, kDctCoeffsPerBlock> coeff{};
};

struct DctComponent {
    int component_index = 0;
    int component_id = 0;
    int h_samp_factor = 0;
    int v_samp_factor = 0;
    int width_in_blocks = 0;
    int height_in_blocks = 0;
    std::vector<DctBlock> blocks;

    const DctBlock *GetBlock(int row, int col) const;
    DctBlock *GetBlock(int row, int col);
};

struct JpegDctImage {
    int image_width = 0;
    int image_height = 0;
    int num_components = 0;
    std::vector<DctComponent> components;
};

const DctComponent *GetLumaComponent(
    const JpegDctImage &image,
    int *out_component_index,
    std::string *error_message);

DctComponent *GetMutableLumaComponent(
    JpegDctImage *image,
    int *out_component_index,
    std::string *error_message);

bool ReadJpegDctImage(
    const std::string &input_path,
    JpegDctImage *out_image,
    std::string *error_message);

bool ExtractJpegHeaderPrefixBeforeSos(
    const std::string &input_path,
    std::vector<uint8_t> *out_prefix,
    std::string *error_message);

bool ApplyJpegHeaderPrefixBeforeSos(
    const std::string &jpeg_path,
    const std::vector<uint8_t> &prefix,
    std::string *error_message);

bool WriteJpegDctImage(
    const std::string &reference_jpeg_path,
    const JpegDctImage &image,
    const std::string &output_path,
    std::string *error_message);

bool DumpLumaBlocksToText(
    const JpegDctImage &image,
    const std::string &output_text_path,
    std::string *error_message);

bool LoadLumaBlocksFromText(
    const std::string &input_text_path,
    DctComponent *out_luma,
    std::string *error_message);

bool ReplaceLumaComponent(
    JpegDctImage *image,
    const DctComponent &luma,
    std::string *error_message);

}  // namespace phase1
}  // namespace deltadct

#endif
