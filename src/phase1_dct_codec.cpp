#include "phase1_dct_codec.hpp"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include <jerror.h>
#include <jpeglib.h>
}

namespace deltadct {
namespace phase1 {

namespace {

struct JpegErrorManager {
    jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
    char message[JMSG_LENGTH_MAX];
};

extern "C" void JpegErrorExit(j_common_ptr cinfo);

void SetError(std::string *error_message, const std::string &msg) {
    if (error_message != nullptr) {
        *error_message = msg;
    }
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

bool FindSosOffset(
    const std::vector<uint8_t> &data,
    size_t *out_offset,
    std::string *error_message) {
    if (out_offset == nullptr) {
        SetError(error_message, "out_offset is null.");
        return false;
    }
    if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        SetError(error_message, "Invalid JPEG SOI header.");
        return false;
    }

    size_t offset = 2;
    while (offset + 1 < data.size()) {
        if (data[offset] != 0xFF) {
            ++offset;
            continue;
        }
        while (offset < data.size() && data[offset] == 0xFF) {
            ++offset;
        }
        if (offset >= data.size()) {
            break;
        }
        const uint8_t marker = data[offset++];
        const size_t marker_start = (offset >= 2) ? (offset - 2) : 0;

        if (marker == 0xDA) {
            *out_offset = marker_start;
            return true;
        }

        if (marker == 0xD8 || marker == 0xD9) {
            continue;
        }
        if (marker >= 0xD0 && marker <= 0xD7) {
            continue;
        }
        if (marker == 0x01) {
            continue;
        }

        if (offset + 1 >= data.size()) {
            SetError(error_message, "Truncated JPEG marker segment.");
            return false;
        }
        const uint16_t length = static_cast<uint16_t>(data[offset] << 8) |
                                static_cast<uint16_t>(data[offset + 1]);
        if (length < 2) {
            SetError(error_message, "Invalid JPEG marker length.");
            return false;
        }
        if (offset + length > data.size()) {
            SetError(error_message, "JPEG marker length exceeds file size.");
            return false;
        }
        offset += length;
    }

    SetError(error_message, "SOS marker not found in JPEG.");
    return false;
}

extern "C" void JpegErrorExit(j_common_ptr cinfo) {
    auto *err = reinterpret_cast<JpegErrorManager *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->setjmp_buffer, 1);
}

bool ValidateImageStruct(const JpegDctImage &image, std::string *error_message) {
    if (image.num_components <= 0) {
        SetError(error_message, "Invalid image: num_components must be positive.");
        return false;
    }
    if (static_cast<int>(image.components.size()) != image.num_components) {
        SetError(error_message, "Invalid image: components.size does not match num_components.");
        return false;
    }
    for (int comp_idx = 0; comp_idx < image.num_components; ++comp_idx) {
        const DctComponent &comp = image.components[comp_idx];
        if (comp.width_in_blocks <= 0 || comp.height_in_blocks <= 0) {
            SetError(error_message, "Invalid image: component block dimensions must be positive.");
            return false;
        }
        const int expected_blocks = comp.width_in_blocks * comp.height_in_blocks;
        if (static_cast<int>(comp.blocks.size()) != expected_blocks) {
            SetError(error_message, "Invalid image: component block count mismatch.");
            return false;
        }
    }
    return true;
}

int FindLumaComponentIndex(const JpegDctImage &image) {
    if (image.components.empty()) {
        return -1;
    }

    if (image.num_components == 1) {
        return 0;
    }

    for (size_t i = 0; i < image.components.size(); ++i) {
        if (image.components[i].component_id == 1) {
            return static_cast<int>(i);
        }
    }

    int best_index = 0;
    int best_area = -1;
    for (size_t i = 0; i < image.components.size(); ++i) {
        const DctComponent &comp = image.components[i];
        const int area = comp.width_in_blocks * comp.height_in_blocks;
        if (area > best_area) {
            best_area = area;
            best_index = static_cast<int>(i);
        }
    }

    return best_index;
}

}  // namespace

const DctComponent *GetLumaComponent(
    const JpegDctImage &image,
    int *out_component_index,
    std::string *error_message) {
    const int luma_index = FindLumaComponentIndex(image);
    if (luma_index < 0 || luma_index >= static_cast<int>(image.components.size())) {
        SetError(error_message, "Failed to locate luma component.");
        return nullptr;
    }
    if (out_component_index != nullptr) {
        *out_component_index = luma_index;
    }
    return &image.components[static_cast<size_t>(luma_index)];
}

DctComponent *GetMutableLumaComponent(
    JpegDctImage *image,
    int *out_component_index,
    std::string *error_message) {
    if (image == nullptr) {
        SetError(error_message, "image is null.");
        return nullptr;
    }
    const int luma_index = FindLumaComponentIndex(*image);
    if (luma_index < 0 || luma_index >= static_cast<int>(image->components.size())) {
        SetError(error_message, "Failed to locate luma component.");
        return nullptr;
    }
    if (out_component_index != nullptr) {
        *out_component_index = luma_index;
    }
    return &image->components[static_cast<size_t>(luma_index)];
}

bool ReadJpegDctImage(
    const std::string &input_path,
    JpegDctImage *out_image,
    std::string *error_message) {
    if (out_image == nullptr) {
        SetError(error_message, "out_image is null.");
        return false;
    }

    if (input_path.empty()) {
        SetError(error_message, "Input JPEG path is empty.");
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(input_path, ec) ||
        !std::filesystem::is_regular_file(input_path, ec)) {
        SetError(error_message, "Input JPEG does not exist or is not a file: " + input_path);
        return false;
    }
    std::ifstream readable_check(input_path, std::ios::in | std::ios::binary);
    if (!readable_check.good()) {
        SetError(error_message, "Input JPEG is not readable: " + input_path);
        return false;
    }

    FILE *input_fp = std::fopen(input_path.c_str(), "rb");
    if (input_fp == nullptr) {
        SetError(error_message, "Failed to open input JPEG: " + input_path);
        return false;
    }

    jpeg_decompress_struct dinfo;
    std::memset(&dinfo, 0, sizeof(dinfo));
    JpegErrorManager jerr;
    std::memset(&jerr, 0, sizeof(jerr));

    dinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = JpegErrorExit;

    if (setjmp(jerr.setjmp_buffer) != 0) {
        const std::string msg = std::string("libjpeg read error: ") + jerr.message;
        jpeg_destroy_decompress(&dinfo);
        std::fclose(input_fp);
        SetError(error_message, msg);
        return false;
    }

    jpeg_create_decompress(&dinfo);
    jpeg_stdio_src(&dinfo, input_fp);

    if (jpeg_read_header(&dinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&dinfo);
        std::fclose(input_fp);
        SetError(error_message, "Invalid JPEG header.");
        return false;
    }

    if (dinfo.num_components <= 0 || dinfo.comp_info == nullptr) {
        jpeg_destroy_decompress(&dinfo);
        std::fclose(input_fp);
        SetError(error_message, "Invalid JPEG components.");
        return false;
    }

    jvirt_barray_ptr *coef_arrays = jpeg_read_coefficients(&dinfo);
    if (coef_arrays == nullptr) {
        jpeg_destroy_decompress(&dinfo);
        std::fclose(input_fp);
        SetError(error_message, "Failed to decode JPEG DCT coefficients.");
        return false;
    }

    JpegDctImage image;
    image.image_width = static_cast<int>(dinfo.image_width);
    image.image_height = static_cast<int>(dinfo.image_height);
    image.num_components = dinfo.num_components;
    image.components.reserve(static_cast<size_t>(dinfo.num_components));

    bool has_luma = (dinfo.num_components == 1);

    for (int comp_idx = 0; comp_idx < dinfo.num_components; ++comp_idx) {
        const jpeg_component_info &comp_info = dinfo.comp_info[comp_idx];
        if (comp_info.component_id == 1) {
            has_luma = true;
        }

        DctComponent component;
        component.component_index = comp_idx;
        component.component_id = comp_info.component_id;
        component.h_samp_factor = comp_info.h_samp_factor;
        component.v_samp_factor = comp_info.v_samp_factor;
        component.width_in_blocks = static_cast<int>(comp_info.width_in_blocks);
        component.height_in_blocks = static_cast<int>(comp_info.height_in_blocks);
        component.blocks.resize(
            static_cast<size_t>(component.width_in_blocks * component.height_in_blocks));

        for (JDIMENSION row = 0; row < comp_info.height_in_blocks; ++row) {
            JBLOCKARRAY row_ptr = (*dinfo.mem->access_virt_barray)(
                reinterpret_cast<j_common_ptr>(&dinfo),
                coef_arrays[comp_idx],
                row,
                1,
                FALSE);
            if (row_ptr == nullptr || row_ptr[0] == nullptr) {
                jpeg_finish_decompress(&dinfo);
                jpeg_destroy_decompress(&dinfo);
                std::fclose(input_fp);
                SetError(error_message, "Failed to access JPEG DCT blocks.");
                return false;
            }

            for (JDIMENSION col = 0; col < comp_info.width_in_blocks; ++col) {
                const JCOEFPTR src_block = row_ptr[0][col];
                DctBlock *dst_block = component.GetBlock(static_cast<int>(row), static_cast<int>(col));
                if (dst_block == nullptr) {
                    jpeg_finish_decompress(&dinfo);
                    jpeg_destroy_decompress(&dinfo);
                    std::fclose(input_fp);
                    SetError(error_message, "Internal error: failed to address destination block.");
                    return false;
                }
                for (int k = 0; k < kDctCoeffsPerBlock; ++k) {
                    dst_block->coeff[static_cast<size_t>(k)] =
                        static_cast<int16_t>(src_block[k]);
                }
            }
        }

        image.components.push_back(std::move(component));
    }

    if (!has_luma) {
        jpeg_finish_decompress(&dinfo);
        jpeg_destroy_decompress(&dinfo);
        std::fclose(input_fp);
        SetError(error_message, "Failed to locate luma (Y) component in JPEG.");
        return false;
    }

    jpeg_finish_decompress(&dinfo);
    jpeg_destroy_decompress(&dinfo);
    std::fclose(input_fp);

    *out_image = std::move(image);
    return true;
}

bool ExtractJpegHeaderPrefixBeforeSos(
    const std::string &input_path,
    std::vector<uint8_t> *out_prefix,
    std::string *error_message) {
    if (out_prefix == nullptr) {
        SetError(error_message, "out_prefix is null.");
        return false;
    }

    std::vector<uint8_t> data;
    if (!ReadWholeFile(input_path, &data, error_message)) {
        return false;
    }

    size_t sos_offset = 0;
    if (!FindSosOffset(data, &sos_offset, error_message)) {
        return false;
    }

    if (sos_offset == 0 || sos_offset > data.size()) {
        SetError(error_message, "Invalid SOS offset for header prefix.");
        return false;
    }

    out_prefix->assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(sos_offset));
    return true;
}

bool ApplyJpegHeaderPrefixBeforeSos(
    const std::string &jpeg_path,
    const std::vector<uint8_t> &prefix,
    std::string *error_message) {
    if (prefix.empty()) {
        return true;
    }
    if (prefix.size() < 2 || prefix[0] != 0xFF || prefix[1] != 0xD8) {
        SetError(error_message, "Invalid JPEG header prefix.");
        return false;
    }

    std::vector<uint8_t> data;
    if (!ReadWholeFile(jpeg_path, &data, error_message)) {
        return false;
    }

    size_t sos_offset = 0;
    if (!FindSosOffset(data, &sos_offset, error_message)) {
        return false;
    }

    if (sos_offset > data.size()) {
        SetError(error_message, "Invalid SOS offset in output JPEG.");
        return false;
    }

    std::vector<uint8_t> merged;
    merged.reserve(prefix.size() + (data.size() - sos_offset));
    merged.insert(merged.end(), prefix.begin(), prefix.end());
    merged.insert(merged.end(), data.begin() + static_cast<std::ptrdiff_t>(sos_offset), data.end());

    std::ofstream output(jpeg_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        SetError(error_message, "Failed to open output JPEG: " + jpeg_path);
        return false;
    }
    output.write(reinterpret_cast<const char *>(merged.data()), static_cast<std::streamsize>(merged.size()));
    if (!output.good()) {
        SetError(error_message, "Failed to write output JPEG: " + jpeg_path);
        return false;
    }

    return true;
}

bool WriteJpegDctImage(
    const std::string &reference_jpeg_path,
    const JpegDctImage &image,
    const std::string &output_path,
    std::string *error_message) {
    if (!ValidateImageStruct(image, error_message)) {
        return false;
    }

    FILE *ref_fp = std::fopen(reference_jpeg_path.c_str(), "rb");
    if (ref_fp == nullptr) {
        SetError(error_message, "Failed to open reference JPEG: " + reference_jpeg_path);
        return false;
    }

    FILE *output_fp = std::fopen(output_path.c_str(), "wb");
    if (output_fp == nullptr) {
        std::fclose(ref_fp);
        SetError(error_message, "Failed to open output JPEG: " + output_path);
        return false;
    }

    jpeg_decompress_struct srcinfo;
    std::memset(&srcinfo, 0, sizeof(srcinfo));
    jpeg_compress_struct dstinfo;
    std::memset(&dstinfo, 0, sizeof(dstinfo));
    JpegErrorManager jerr;
    std::memset(&jerr, 0, sizeof(jerr));

    srcinfo.err = jpeg_std_error(&jerr.pub);
    dstinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = JpegErrorExit;

    if (setjmp(jerr.setjmp_buffer) != 0) {
        const std::string msg = std::string("libjpeg write error: ") + jerr.message;
        jpeg_destroy_compress(&dstinfo);
        jpeg_destroy_decompress(&srcinfo);
        std::fclose(output_fp);
        std::fclose(ref_fp);
        SetError(error_message, msg);
        return false;
    }

    jpeg_create_decompress(&srcinfo);
    jpeg_stdio_src(&srcinfo, ref_fp);
    if (jpeg_read_header(&srcinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&srcinfo);
        std::fclose(output_fp);
        std::fclose(ref_fp);
        SetError(error_message, "Invalid reference JPEG header.");
        return false;
    }

    jvirt_barray_ptr *src_coef_arrays = jpeg_read_coefficients(&srcinfo);

    if (srcinfo.num_components != image.num_components) {
        jpeg_finish_decompress(&srcinfo);
        jpeg_destroy_decompress(&srcinfo);
        std::fclose(output_fp);
        std::fclose(ref_fp);
        SetError(error_message, "Component count mismatch between image data and reference JPEG.");
        return false;
    }

    for (int comp_idx = 0; comp_idx < image.num_components; ++comp_idx) {
        const jpeg_component_info &comp_info = srcinfo.comp_info[comp_idx];
        const DctComponent &component = image.components[comp_idx];

        if (component.width_in_blocks != static_cast<int>(comp_info.width_in_blocks) ||
            component.height_in_blocks != static_cast<int>(comp_info.height_in_blocks)) {
            jpeg_finish_decompress(&srcinfo);
            jpeg_destroy_decompress(&srcinfo);
            std::fclose(output_fp);
            std::fclose(ref_fp);
            SetError(error_message, "Block geometry mismatch between matrix and reference JPEG.");
            return false;
        }

        for (JDIMENSION row = 0; row < comp_info.height_in_blocks; ++row) {
            JBLOCKARRAY row_ptr = (*srcinfo.mem->access_virt_barray)(
                reinterpret_cast<j_common_ptr>(&srcinfo),
                src_coef_arrays[comp_idx],
                row,
                1,
                TRUE);

            for (JDIMENSION col = 0; col < comp_info.width_in_blocks; ++col) {
                JCOEFPTR dst_block = row_ptr[0][col];
                const DctBlock *src_block =
                    component.GetBlock(static_cast<int>(row), static_cast<int>(col));
                if (src_block == nullptr) {
                    jpeg_finish_decompress(&srcinfo);
                    jpeg_destroy_decompress(&srcinfo);
                    std::fclose(output_fp);
                    std::fclose(ref_fp);
                    SetError(error_message, "Internal error: failed to address source block.");
                    return false;
                }

                for (int k = 0; k < kDctCoeffsPerBlock; ++k) {
                    dst_block[k] = static_cast<JCOEF>(src_block->coeff[static_cast<size_t>(k)]);
                }
            }
        }
    }

    jpeg_create_compress(&dstinfo);
    jpeg_stdio_dest(&dstinfo, output_fp);
    jpeg_copy_critical_parameters(&srcinfo, &dstinfo);
    jpeg_write_coefficients(&dstinfo, src_coef_arrays);

    jpeg_finish_compress(&dstinfo);
    jpeg_finish_decompress(&srcinfo);
    jpeg_destroy_compress(&dstinfo);
    jpeg_destroy_decompress(&srcinfo);

    std::fclose(output_fp);
    std::fclose(ref_fp);
    return true;
}

bool DumpLumaBlocksToText(
    const JpegDctImage &image,
    const std::string &output_text_path,
    std::string *error_message) {
    int luma_index = -1;
    const DctComponent *luma = GetLumaComponent(image, &luma_index, error_message);
    if (luma == nullptr) {
        return false;
    }

    std::ofstream output(output_text_path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        SetError(error_message, "Failed to open output text file: " + output_text_path);
        return false;
    }

    output << luma->width_in_blocks << " " << luma->height_in_blocks << "\n";
    for (const DctBlock &block : luma->blocks) {
        for (int k = 0; k < kDctCoeffsPerBlock; ++k) {
            output << block.coeff[static_cast<size_t>(k)];
            if (k + 1 != kDctCoeffsPerBlock) {
                output << " ";
            }
        }
        output << "\n";
    }

    return true;
}

bool LoadLumaBlocksFromText(
    const std::string &input_text_path,
    DctComponent *out_luma,
    std::string *error_message) {
    if (out_luma == nullptr) {
        SetError(error_message, "out_luma is null.");
        return false;
    }

    std::ifstream input(input_text_path);
    if (!input.is_open()) {
        SetError(error_message, "Failed to open input text file: " + input_text_path);
        return false;
    }

    int width_blocks = 0;
    int height_blocks = 0;
    if (!(input >> width_blocks >> height_blocks)) {
        SetError(error_message, "Failed to parse matrix header.");
        return false;
    }
    if (width_blocks <= 0 || height_blocks <= 0) {
        SetError(error_message, "Invalid matrix dimensions in text file.");
        return false;
    }

    DctComponent luma;
    luma.component_index = 0;
    luma.width_in_blocks = width_blocks;
    luma.height_in_blocks = height_blocks;
    luma.blocks.resize(static_cast<size_t>(width_blocks * height_blocks));

    for (DctBlock &block : luma.blocks) {
        for (int k = 0; k < kDctCoeffsPerBlock; ++k) {
            int value = 0;
            if (!(input >> value)) {
                SetError(error_message, "Insufficient coefficient values in matrix text file.");
                return false;
            }
            if (value < std::numeric_limits<int16_t>::min() ||
                value > std::numeric_limits<int16_t>::max()) {
                SetError(error_message, "Coefficient value out of int16 range in matrix text file.");
                return false;
            }
            block.coeff[static_cast<size_t>(k)] = static_cast<int16_t>(value);
        }
    }

    *out_luma = std::move(luma);
    return true;
}

bool ReplaceLumaComponent(
    JpegDctImage *image,
    const DctComponent &luma,
    std::string *error_message) {
    int luma_index = -1;
    DctComponent *target_luma = GetMutableLumaComponent(image, &luma_index, error_message);
    if (target_luma == nullptr) {
        return false;
    }

    if (target_luma->width_in_blocks != luma.width_in_blocks ||
        target_luma->height_in_blocks != luma.height_in_blocks) {
        SetError(error_message, "Luma matrix geometry mismatch.");
        return false;
    }
    if (target_luma->blocks.size() != luma.blocks.size()) {
        SetError(error_message, "Luma matrix block count mismatch.");
        return false;
    }

    target_luma->blocks = luma.blocks;
    return true;
}

}  // namespace phase1
}  // namespace deltadct

const deltadct::phase1::DctBlock *deltadct::phase1::DctComponent::GetBlock(int row, int col) const {
    if (row < 0 || col < 0 || row >= height_in_blocks || col >= width_in_blocks) {
        return nullptr;
    }
    const int idx = row * width_in_blocks + col;
    return &blocks[idx];
}

deltadct::phase1::DctBlock *deltadct::phase1::DctComponent::GetBlock(int row, int col) {
    if (row < 0 || col < 0 || row >= height_in_blocks || col >= width_in_blocks) {
        return nullptr;
    }
    const int idx = row * width_in_blocks + col;
    return &blocks[idx];
}
