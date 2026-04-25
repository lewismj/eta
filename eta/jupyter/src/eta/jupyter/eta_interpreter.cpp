#include "eta/jupyter/eta_interpreter.h"

#include <utility>

namespace eta::jupyter {

std::string EtaInterpreter::eval(std::string code)
{
    return std::move(code);
}

} // namespace eta::jupyter
