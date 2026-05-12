/**
 * @file display_classifier.h
 * @brief Display classification helpers for session and notebook front-ends.
 */

#pragma once

#include <string>
#include <string_view>

#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/session/eval_display.h"

namespace eta::session {

/**
 * @brief Classifies runtime values for display rendering.
 */
class DisplayClassifier {
public:
    DisplayClassifier(
        const runtime::memory::heap::Heap& heap,
        const runtime::memory::intern::InternTable& intern_table) noexcept
        : heap_(heap),
          intern_table_(intern_table) {}

    /**
     * @brief Classify a runtime value into a high-level display tag.
     */
    [[nodiscard]] DisplayTag classify_display_tag(runtime::nanbox::LispVal value) const;

    /**
     * @brief Map a MIME type to a display tag.
     */
    [[nodiscard]] static DisplayTag display_tag_for_mime(std::string_view mime);

    /**
     * @brief Decode a runtime string/symbol value into UTF-8 text.
     */
    [[nodiscard]] bool try_decode_string(runtime::nanbox::LispVal value, std::string* out) const;

    /**
     * @brief Unpack `(vector 'jupyter-display mime payload)` wrappers.
     */
    [[nodiscard]] bool try_unpack_jupyter_display(runtime::nanbox::LispVal value,
                                                  std::string* mime_out,
                                                  runtime::nanbox::LispVal* payload_out) const;

private:
    const runtime::memory::heap::Heap& heap_;
    const runtime::memory::intern::InternTable& intern_table_;
};

} // namespace eta::session
