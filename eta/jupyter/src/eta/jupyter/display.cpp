#include "eta/jupyter/display.h"

#include <utility>

namespace eta::jupyter {

DisplayValue make_plain_display(std::string text_repr)
{
    return DisplayValue{std::move(text_repr)};
}

} // namespace eta::jupyter
