/**
 * @file display_classifier.cpp
 * @brief Display classification helpers for session and notebook front-ends.
 */

#include "eta/session/display_classifier.h"

#include "eta/runtime/types/types.h"

namespace eta::session {

DisplayTag DisplayClassifier::classify_display_tag(const runtime::nanbox::LispVal value) const {
    using runtime::memory::heap::ObjectKind;

    std::string mime;
    if (try_unpack_jupyter_display(value, &mime, nullptr)) {
        if (const auto tag = display_tag_for_mime(mime); tag != DisplayTag::Text) {
            return tag;
        }
    }

    if (!runtime::nanbox::ops::is_boxed(value) ||
        runtime::nanbox::ops::tag(value) != runtime::nanbox::Tag::HeapObject) {
        return DisplayTag::Text;
    }

    const auto id = runtime::nanbox::ops::payload(value);
    if (heap_.try_get_as<ObjectKind::Tensor, void>(id)) return DisplayTag::Tensor;
    if (heap_.try_get_as<ObjectKind::FactTable, void>(id)) return DisplayTag::FactTable;
    return DisplayTag::Text;
}

DisplayTag DisplayClassifier::display_tag_for_mime(const std::string_view mime) {
    if (mime == "text/html") return DisplayTag::Html;
    if (mime == "text/markdown") return DisplayTag::Markdown;
    if (mime == "text/latex") return DisplayTag::Latex;
    if (mime == "image/svg+xml") return DisplayTag::Svg;
    if (mime == "image/png") return DisplayTag::Png;
    if (mime == "application/vnd.vegalite.v5+json") return DisplayTag::VegaLite;
    if (mime == "application/vnd.eta.tensor+json") return DisplayTag::Tensor;
    if (mime == "application/vnd.eta.facttable+json") return DisplayTag::FactTable;
    return DisplayTag::Text;
}

bool DisplayClassifier::try_decode_string(const runtime::nanbox::LispVal value, std::string* out) const {
    if (!out) return false;

    using runtime::nanbox::Tag;
    if (!runtime::nanbox::ops::is_boxed(value)) return false;

    const auto tag = runtime::nanbox::ops::tag(value);
    if (tag == Tag::String || tag == Tag::Symbol) {
        auto sv = intern_table_.get_string(runtime::nanbox::ops::payload(value));
        if (!sv) return false;
        *out = std::string(*sv);
        return true;
    }

    return false;
}

bool DisplayClassifier::try_unpack_jupyter_display(
    const runtime::nanbox::LispVal value,
    std::string* const mime_out,
    runtime::nanbox::LispVal* const payload_out) const {
    using runtime::memory::heap::ObjectKind;
    using runtime::types::Vector;
    using runtime::nanbox::Tag;

    if (!runtime::nanbox::ops::is_boxed(value) ||
        runtime::nanbox::ops::tag(value) != Tag::HeapObject) {
        return false;
    }

    const auto id = runtime::nanbox::ops::payload(value);
    auto* vec = heap_.try_get_as<ObjectKind::Vector, Vector>(id);
    if (!vec || vec->elements.size() < 3) return false;

    std::string marker;
    if (!try_decode_string(vec->elements[0], &marker)) return false;
    if (marker != "jupyter-display") return false;

    std::string mime;
    if (!try_decode_string(vec->elements[1], &mime)) return false;

    if (mime_out) *mime_out = std::move(mime);
    if (payload_out) *payload_out = vec->elements[2];
    return true;
}

} // namespace eta::session
