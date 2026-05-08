#include "eta/runtime/prof/pprof.h"

namespace eta::runtime::prof {

std::expected<std::string, std::string> write_pprof_profile(const ArchiveSession& /*session*/) {
#ifdef ETA_PROF_PPROF
    return std::unexpected("pprof support is enabled, but the writer is not implemented yet");
#else
    return std::unexpected("pprof support is disabled (configure with ETA_PROF_PPROF=ON)");
#endif
}

} ///< namespace eta::runtime::prof

