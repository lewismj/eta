#pragma once

#include <string>
#include <vector>

#include "eta/linter/lint.h"

namespace eta::linter::cli {
    /// `path:line:col: severity[CODE]: message` lines plus coverage notes.
    std::string pretty_report(const std::vector<FileReport> &reports, bool quiet);
}

