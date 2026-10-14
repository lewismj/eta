#pragma once


#include "eta/cst/parse.h"

namespace eta::format {
    enum class FormAlias { None, Define, Lambda, Let, Sequence, Conditional, ClauseList, Module, ImportExport, RecordType, Loop };

    bool is_identifier(const cst::File &file, cst::NodeId id);
    bool formals(const cst::File &file, cst::NodeId id, bool signature);
    FormAlias alias(const cst::File &file, const cst::Node &node);
}

