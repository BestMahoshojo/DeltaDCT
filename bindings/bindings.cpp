#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "deltadct.h"

namespace py = pybind11;

PYBIND11_MODULE(pydeltadct, m) {
    m.doc() = "Python bindings for high-level DeltaDCT APIs";

    py::enum_<deltadct::RoutingMode>(m, "RoutingMode")
        .value("ROUTING_AUTO", deltadct::RoutingMode::kAuto)
        .value("FPM_ONLY", deltadct::RoutingMode::kFpmOnly)
        .value("DCHASH_ONLY", deltadct::RoutingMode::kDCHashOnly)
        .export_values();

    py::class_<deltadct::CompressOptions>(m, "CompressOptions")
        .def(py::init<>())
        .def_readwrite("mode", &deltadct::CompressOptions::mode)
        .def_readwrite("window_size", &deltadct::CompressOptions::window_size)
        .def_readwrite("num_features", &deltadct::CompressOptions::num_features)
        .def_readwrite("strict_lossless", &deltadct::CompressOptions::strict_lossless);

    py::class_<deltadct::DedupResult>(m, "DedupResult")
        .def(py::init<>())
        .def_readonly("success", &deltadct::DedupResult::success)
        .def_readonly("compression_ratio", &deltadct::DedupResult::compression_ratio)
        .def_readonly("original_size", &deltadct::DedupResult::original_size)
        .def_readonly("compressed_size", &deltadct::DedupResult::compressed_size)
        .def_readonly("header_bytes", &deltadct::DedupResult::header_bytes)
        .def_readonly("copy_meta_bytes", &deltadct::DedupResult::copy_meta_bytes)
        .def_readonly("insert_raw_bytes", &deltadct::DedupResult::insert_raw_bytes)
        .def_readonly("time_ms", &deltadct::DedupResult::time_ms)
        .def_readonly("similarity_time_ms", &deltadct::DedupResult::similarity_time_ms)
        .def_readonly("delta_time_ms", &deltadct::DedupResult::delta_time_ms)
        .def_readonly("throughput_mb_s", &deltadct::DedupResult::throughput_mb_s)
        .def_readonly("route_used", &deltadct::DedupResult::route_used)
        .def_readonly("blocks_total", &deltadct::DedupResult::blocks_total)
        .def_readonly("blocks_exact", &deltadct::DedupResult::blocks_exact)
        .def_readonly("blocks_similar", &deltadct::DedupResult::blocks_similar)
        .def_readonly("blocks_different", &deltadct::DedupResult::blocks_different)
        .def_readonly("blocks_width", &deltadct::DedupResult::blocks_width)
        .def_readonly("blocks_height", &deltadct::DedupResult::blocks_height)
        .def_readonly("block_map", &deltadct::DedupResult::block_map);

    m.def(
        "compress_image",
        [](const std::string &target_jpg,
           const std::string &base_jpg,
           const std::string &output_ddct,
           deltadct::RoutingMode mode,
           int window_size,
           int num_features,
           bool strict_lossless) {
            deltadct::CompressOptions options;
            options.mode = mode;
            options.window_size = window_size;
            options.num_features = num_features;
            options.strict_lossless = strict_lossless;
            return deltadct::compress_image(target_jpg, base_jpg, output_ddct, options);
        },
        py::arg("target_jpg"),
        py::arg("base_jpg"),
        py::arg("output_ddct"),
        py::arg("mode") = deltadct::RoutingMode::kAuto,
        py::arg("window_size") = 2,
        py::arg("num_features") = 10,
        py::arg("strict_lossless") = true);

    m.def(
        "decompress_image",
        &deltadct::decompress_image,
        py::arg("input_ddct"),
        py::arg("base_jpg"),
        py::arg("output_jpg"));

    m.def(
        "extract_features",
        &deltadct::extract_features,
        py::arg("img_path"),
        py::arg("window_size") = 2,
        py::arg("num_features") = 10);
}
