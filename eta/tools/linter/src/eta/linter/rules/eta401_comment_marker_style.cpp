#include "eta/linter/rules/rules.h"

namespace eta::linter::rules {
    void run_eta401(RuleContext &context) {
        for (const auto &association : context.file.associations) {
            const auto &trivia = context.file.trivia[association.comment];
            if (trivia.kind != cst::TriviaKind::LineComment) continue;
            const auto raw = context.file.source->slice(trivia.span);
            std::size_t markers = 0;
            while (markers < raw.size() && raw[markers] == ';') ++markers;
            const cst::ByteSpan span{trivia.span.begin, trivia.span.begin + markers};
            if (association.kind == cst::CommentAssociation::EndOfLine && markers != 1) {
                context.out.push_back({"ETA401", "eta_lint", Severity::Info,
                    "end-of-line comment should use a single semicolon", span,
                    true, 1, 1, std::string(context.path)});
            } else if (association.kind == cst::CommentAssociation::Leading && markers == 1) {
                context.out.push_back({"ETA401", "eta_lint", Severity::Info,
                    "standalone comment should use a double semicolon", span,
                    true, 1, 1, std::string(context.path)});
            }
        }
    }
}

