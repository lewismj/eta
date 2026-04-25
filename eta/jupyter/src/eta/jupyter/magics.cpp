#include "eta/jupyter/magics.h"

namespace eta::jupyter {

bool is_magic(std::string_view line)
{
    return !line.empty() && line.front() == '%';
}

std::string strip_magic_prefix(std::string_view line)
{
    if (line.size() >= 2 && line[0] == '%' && line[1] == '%') {
        return std::string(line.substr(2));
    }
    if (!line.empty() && line.front() == '%') {
        return std::string(line.substr(1));
    }
    return std::string(line);
}

} // namespace eta::jupyter
