// Copyright 2017 Global Phasing Ltd.

#include "gemmi/mmread.hpp"
#include "gemmi/gz.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

void add_mmread(py::module& m) {
  m.def("read_structure", [](const std::string& path) {
          return gemmi::read_structure(gemmi::MaybeGzipped(path));
        }, py::arg("path"),
        "Reads a coordinate file into Structure.");
}
