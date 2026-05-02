#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/jupyter/display.h"
#include "eta/jupyter/kernel_config.h"
#include "xeus/xcomm.hpp"
#include "xeus/xinterpreter.hpp"
#include "eta/session/driver.h"

namespace eta::jupyter {

/**
 * @brief Eta Jupyter interpreter backed by the shared session::Driver.
 */
class EtaInterpreter : public xeus::xinterpreter {
public:
    EtaInterpreter();
    ~EtaInterpreter() override = default;

    /**
     * @brief Request interruption of the currently executing cell.
     */
    void request_interrupt() noexcept;

private:
    /**
     * @brief Configure interpreter-side state after kernel bootstrap.
     */
    void configure_impl() override;

    /**
     * @brief Execute one Jupyter code cell.
     */
    void execute_request_impl(send_reply_callback cb,
                              int execution_counter,
                              const std::string& code,
                              xeus::execute_request_config config,
                              nl::json user_expressions) override;

    /**
     * @brief Return completion matches for the current cursor location.
     */
    nl::json complete_request_impl(const std::string& code,
                                   int cursor_pos) override;

    /**
     * @brief Return Markdown hover content for the token under the cursor.
     */
    nl::json inspect_request_impl(const std::string& code,
                                  int cursor_pos,
                                  int detail_level) override;

    /**
     * @brief Report whether the current cell text is syntactically complete.
     */
    nl::json is_complete_request_impl(const std::string& code) override;

    /**
     * @brief Return kernel capabilities and language metadata.
     */
    nl::json kernel_info_request_impl() override;

    /**
     * @brief Handle kernel shutdown.
     */
    nl::json shutdown_request_impl(bool restart) override;

    /**
     * @brief Handle kernel interrupt requests.
     */
    nl::json interrupt_request_impl() override;

    /**
     * @brief Format the driver's diagnostics as plain text.
     */
    [[nodiscard]] std::string diagnostics_to_text() const;

    /**
     * @brief Format a minimal traceback from the VM frame stack.
     */
    [[nodiscard]] std::vector<std::string> traceback_lines(const std::string& ename,
                                                           const std::string& evalue) const;

    /**
     * @brief Apply startup environment and auto-import configuration.
     */
    void apply_startup_configuration();

    /**
     * @brief Set process-level environment markers for kernel mode.
     */
    static void set_kernel_environment();

    /**
     * @brief Register Jupyter comm targets used by live notebook widgets.
     */
    void register_comm_targets();

    /**
     * @brief Register one comm target and install open/message/close handlers.
     */
    void register_comm_target(const std::string& target_name);

    /**
     * @brief Handle `comm_open` for one registered target.
     */
    void handle_comm_open(const std::string& target_name,
                          xeus::xcomm&& comm,
                          xeus::xmessage request);

    /**
     * @brief Handle `comm_msg` for one active comm id.
     */
    void handle_comm_message(const std::string& target_name,
                             const std::string& comm_id,
                             xeus::xmessage request);

    /**
     * @brief Remove a closed comm from the active map.
     */
    void remove_comm(const std::string& target_name,
                     const std::string& comm_id);

    /**
     * @brief Publish one payload to all active comms for a target.
     */
    void broadcast_comm(const std::string& target_name,
                        const nl::json& payload);

    /**
     * @brief Build a snapshot payload for the given target.
     */
    [[nodiscard]] nl::json build_comm_snapshot(const std::string& target_name,
                                               const nl::json& request_data);

    /**
     * @brief Emit actor lifecycle events to active `eta.actors` comms.
     */
    void handle_actor_lifecycle_event(const eta::session::Driver::ActorEvent& event);

    std::unique_ptr<eta::session::Driver> driver_;
    mutable std::mutex driver_mu_;
    mutable std::mutex comm_mu_;
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::unique_ptr<xeus::xcomm>>> comms_;
    KernelConfig kernel_config_{};
    display::RenderOptions render_options_{};
    bool configured_{false};
    bool comm_targets_registered_{false};
};

} ///< namespace eta::jupyter
