/// Project scaffolding for `eta new` and `eta init`.

#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#include "eta/cli/cli_internal.h"

namespace eta::cli {

namespace {

ErrorResult write_text_file(const fs::path& path, std::string_view content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return std::unexpected("failed to create directory '" + path.parent_path().string()
                               + "': " + ec.message());
    }

    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected("failed to open '" + path.string() + "' for writing");
    }
    out << content;
    if (!out.good()) {
        return std::unexpected("failed to write '" + path.string() + "'");
    }
    return {};
}

[[nodiscard]] std::string render_manifest(std::string_view package_name) {
    std::ostringstream out;
    out << "[package]\n"
        << "name = \"" << package_name << "\"\n"
        << "version = \"0.1.0\"\n"
        << "license = \"MIT\"\n"
        << "\n"
        << "[compatibility]\n"
        << "eta = \">=0.6, <0.8\"\n"
        << "\n"
        << "[dependencies]\n";
    return out.str();
}

[[nodiscard]] std::string render_module_source(std::string_view module_name, ProjectKind kind) {
    std::ostringstream out;
    if (kind == ProjectKind::Lib) {
        out << "(module " << module_name << "\n"
            << "  (export square)\n"
            << "  (begin\n"
            << "    (define (square x)\n"
            << "      (* x x))))\n";
    } else {
        out << "(module " << module_name << "\n"
            << "  (import std.io)\n"
            << "  (begin\n"
            << "    (println \"Hello from " << module_name << "\")))\n";
    }
    return out.str();
}

[[nodiscard]] std::string render_smoke_test(std::string_view module_name, ProjectKind kind) {
    std::ostringstream out;
    if (kind == ProjectKind::Lib) {
        out << "(module " << module_name << ".tests.smoke\n"
            << "  (import std.test " << module_name << ")\n"
            << "  (begin\n"
            << "    (define suite\n"
            << "      (make-group \"" << module_name << "\"\n"
            << "        (list\n"
            << "          (make-test \"square\"\n"
            << "            (lambda ()\n"
            << "              (assert-equal 4 (square 2)))))))\n"
            << "    (print-tap (run suite))))\n";
    } else {
        out << "(module " << module_name << ".tests.smoke\n"
            << "  (import std.test)\n"
            << "  (begin\n"
            << "    (define suite\n"
            << "      (make-group \"" << module_name << "\"\n"
            << "        (list\n"
            << "          (make-test \"smoke\"\n"
            << "            (lambda ()\n"
            << "              (assert-true #t))))))\n"
            << "    (print-tap (run suite))))\n";
    }
    return out.str();
}

[[nodiscard]] std::string render_readme(std::string_view package_name, ProjectKind kind) {
    std::ostringstream out;
    out << "# " << package_name << "\n"
        << "\n";
    if (kind == ProjectKind::Lib) {
        out << "Eta library package scaffolded by `eta new`.\n";
    } else {
        out << "Eta executable package scaffolded by `eta new`.\n";
    }
    return out.str();
}

} ///< namespace

CliResult<int> scaffold_project(const fs::path& project_root,
                                std::string package_name,
                                ProjectKind kind,
                                bool in_place) {
    std::error_code ec;
    if (!in_place) {
        if (fs::exists(project_root, ec) && !ec) {
            return std::unexpected("destination already exists: " + project_root.string());
        }
        fs::create_directories(project_root, ec);
        if (ec) {
            return std::unexpected("failed to create project directory '" + project_root.string()
                                   + "': " + ec.message());
        }
    } else {
        if (fs::is_regular_file(project_root / "eta.toml", ec) && !ec) {
            return std::unexpected("eta.toml already exists in " + project_root.string());
        }
    }

    const std::string module_name = module_name_from_package_name(package_name);
    const auto source_path = project_root / "src" / (module_name + ".eta");
    const auto test_path = project_root / "tests" / "smoke.test.eta";

    if (auto res = write_text_file(project_root / "eta.toml", render_manifest(package_name)); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = write_text_file(source_path, render_module_source(module_name, kind)); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = write_text_file(test_path, render_smoke_test(module_name, kind)); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = write_text_file(project_root / ".gitignore", ".eta/\n"); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = write_text_file(project_root / "README.md", render_readme(package_name, kind)); !res) {
        return std::unexpected(res.error());
    }

    std::cout << (in_place ? "initialized " : "created ") << project_root.string() << "\n";
    return 0;
}

} ///< namespace eta::cli
