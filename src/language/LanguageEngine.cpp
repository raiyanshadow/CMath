#include "LanguageInternal.h"

#include "cmath/language/LanguageEngine.h"

#include <iostream>
#include <iterator>
#include <string>
#include <utility>

namespace cmath {

CompileResult LanguageEngine::compile(std::string_view source,
                                      const CompileOptions& options) const {
    CompileResult result;

    if (source.size() > options.maximumSourceBytes) {
        Diagnostic diagnostic;
        diagnostic.severity = DiagnosticSeverity::Error;
        diagnostic.code = "LEX000";
        diagnostic.message =
            "Source file exceeds the configured maximum size of " +
            std::to_string(options.maximumSourceBytes) + " bytes.";
        diagnostic.location = SourceLocation{};
        result.diagnostics.push_back(std::move(diagnostic));
        return result;
    }

    std::cerr << "[cmath] Lexer: starting (" << source.size() << " bytes)\n" << std::flush;
    language::detail::Lexer lexer(source, options.maximumDiagnostics);
    auto lexed = lexer.tokenize();
    std::cerr << "[cmath] Lexer: finished (" << lexed.tokens.size()
              << " tokens, " << lexed.diagnostics.size() << " diagnostics)\n" << std::flush;
    result.diagnostics = std::move(lexed.diagnostics);
    if (!result.diagnostics.empty()) {
        return result;
    }

    std::cerr << "[cmath] Parser: starting\n" << std::flush;
    language::detail::Parser parser(lexed.tokens, options.maximumDiagnostics);
    auto parsed = parser.parse();
    std::cerr << "[cmath] Parser: finished (program=" << (parsed.program ? "yes" : "no")
              << ", " << parsed.diagnostics.size() << " diagnostics)\n" << std::flush;
    result.diagnostics.insert(result.diagnostics.end(),
                              std::make_move_iterator(parsed.diagnostics.begin()),
                              std::make_move_iterator(parsed.diagnostics.end()));
    if (!parsed.program || !result.diagnostics.empty()) {
        return result;
    }

    std::cerr << "[cmath] Semantic analysis: starting\n" << std::flush;
    language::detail::SemanticAnalyzer analyzer(options.maximumDiagnostics,
                                                options.allowShadowing);
    auto analyzed = analyzer.analyze(*parsed.program);
    std::cerr << "[cmath] Semantic analysis: finished (program="
              << (analyzed.program ? "yes" : "no") << ", "
              << analyzed.diagnostics.size() << " diagnostics)\n" << std::flush;
    result.diagnostics.insert(result.diagnostics.end(),
                              std::make_move_iterator(analyzed.diagnostics.begin()),
                              std::make_move_iterator(analyzed.diagnostics.end()));
    if (!analyzed.program || !result.diagnostics.empty()) {
        return result;
    }

    result.program = CompiledProgram(std::move(analyzed.program));
    return result;
}

ExecutionResult LanguageEngine::execute(const CompiledProgram& program,
                                        const ExecutionOptions& options) const {
    ExecutionResult result;

    if (!program.program_) {
        Diagnostic diagnostic;
        diagnostic.severity = DiagnosticSeverity::Error;
        diagnostic.code = "API001";
        diagnostic.message = "Cannot execute an empty compiled program.";
        diagnostic.location = SourceLocation{};
        result.diagnostics.push_back(std::move(diagnostic));
        return result;
    }

    language::detail::Interpreter interpreter(options);
    auto internal = interpreter.execute(*program.program_);

    result.succeeded = internal.succeeded;
    result.output = std::move(internal.output);
    result.lastValue = std::move(internal.lastValue);
    result.diagnostics = std::move(internal.diagnostics);
    return result;
}

} // namespace cmath
