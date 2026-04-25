#pragma once

#include <string>

namespace eta::jupyter {

/**
 * @brief Minimal interpreter facade for the eta_jupyter scaffold.
 *
 * The full xeus request/reply wiring is implemented in later stages; this
 * lightweight facade keeps the target buildable while dependency integration
 * lands.
 */
class EtaInterpreter {
public:
    EtaInterpreter() = default;
    ~EtaInterpreter() = default;

    /**
     * @brief Evaluate a snippet and return a textual placeholder result.
     *
     * @param code Eta source text.
     * @return Placeholder evaluation output.
     */
    std::string eval(std::string code);
};

} // namespace eta::jupyter
