#include <cstdlib>
#include <cstdint>

#include "eta/cst/bridge_sexpr.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::string source(reinterpret_cast<const char *>(data), size);
    auto parsed = eta::cst::parse(source, {});
    if (!parsed) return 0;
    auto serialized = eta::cst::serialize_losslessly(*parsed->file);
    if (!serialized || *serialized != source || !eta::cst::has_unique_byte_ownership(*parsed->file)) std::abort();
    if (parsed->status == eta::cst::Status::Valid) {
        auto original = eta::cst::read_original(*parsed->file);
        auto bridged = eta::cst::to_sexpr(*parsed);
        if (!original || !bridged || original->size() != bridged->size()) std::abort();
        for (std::size_t i = 0; i < original->size(); ++i)
            if (!eta::cst::equivalent_sexpr(*(*original)[i], *(*bridged)[i])) std::abort();
    }
    return 0;
}
