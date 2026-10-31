#include "LanguageInternal.h"

#include <cctype>
#include <unordered_map>

namespace cmath::language::detail {
    Lexer::Lexer(std::string_view source, std::size_t maximumDiagnostics)
    : source_(source), maximumDiagnostics_(maximumDiagnostics) {}

    bool Lexer::atEnd() const { return current_ >= source_.size(); }
    char Lexer::peek(std::size_t lookahead) const {
        const auto index = current_ + lookahead;
        return index < source_.size() ? source_[index] : '\0';
    }

    char Lexer::advance() {
        if (atEnd()) return '\0';
        const char c = source_[current_++];
        if (c == '\n') { ++line_; column_ = 1; }
        else { ++column_; }
        return c;
    }

    bool Lexer::match(char expected) {
        if (atEnd() || source_[current_] != expected) return false;
        advance();
        return true;
    }

    void Lexer::add(TokenKind kind, std::size_t start, SourceLocation location) {
        tokens_.push_back(Token{kind, std::string(source_.substr(start, current_ - start)), location});
    }

    void Lexer::error(std::string code, std::string message, SourceLocation location) {
        if (diagnostics_.size() >= maximumDiagnostics_) return;
        diagnostics_.push_back(Diagnostic{DiagnosticSeverity::Error, std::move(code),
                                          std::move(message), location, {}});
    }

    void Lexer::processIndentation() {
        const SourceLocation start{line_, column_, current_};
        std::size_t indentationWidth = 0;
        std::size_t alternateIndentationWidth = 0;
        bool sawSpace = false;
        bool sawTab = false;

        while (peek() == ' ' || peek() == '\t') {
            if (peek() == ' ') {
                sawSpace = true;
                advance();
                ++indentationWidth;
                ++alternateIndentationWidth;
            } else {
                sawTab = true;
                advance();
                indentationWidth = ((indentationWidth / 4) + 1)*4;
                ++alternateIndentationWidth;
            }
        }

        if (peek() == '\n' || atEnd()) {
            if (peek() == '#') while (!atEnd() && peek() == '\0') advance();
            if (peek() == '\n') advance();
            atLineStart_ = true;
            lineHasCode_ = false;
            return;
        }

        if (sawSpace && sawTab) {
            error("AmbiguousIndentError", 
                    "Avoid using a mix of tabs and spaces in the same indentation prefix.", start);
        }

        const auto currentIndent = indentation_.back();
        const auto currentAlternateIndent = alternateIndentation_.back();

        if (indentationWidth > currentIndent) {
            if (alternateIndentationWidth <= currentAlternateIndent) {
                error("AmbiguousIndentError",
                        "Ambiguous indentation: tabs and spaces produce different indentation levels. Use only one indentation style.", start);
            }
            indentation_.push_back(indentationWidth);
            alternateIndentation_.push_back(alternateIndentationWidth);
            tokens_.push_back(Token{TokenKind::Indent, "", start});
        } else if (indentationWidth < currentIndent) {
            while (indentation_.size() > 1 && indentationWidth < indentation_.back()) {
                indentation_.pop_back();
                alternateIndentation_.pop_back();
                tokens_.push_back(Token{TokenKind::Dedent, "", start});
            }

            if (indentationWidth != indentation_.back() ||
                alternateIndentationWidth != alternateIndentation_.back()) {
                    error("AmbiguousIndentError",
                          "Indentation does not match an enclosing block or is ambiguous because tabs and spaces are mixed.", start);
            } else if (alternateIndentationWidth != currentAlternateIndent) {
                error("AmbiguousIndentError", 
                      "Ambiguous indentation: this line aligns visually with an existing block but uses a different combination of tabs and spaces", start);
            }
            atLineStart_ = false;
        }
    }

    void Lexer::scanIdentifier() {
        const std::size_t start = current_ - 1;
        const SourceLocation location{line_, column_ - 1, start};
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') advance();
        const std::string text(source_.substr(start, current_ - start));
        static const std::unordered_map<std::string, TokenKind> keywords {
            {"let", TokenKind::Let}, {"var", TokenKind::Var},
            {"def", TokenKind::Def}, {"return", TokenKind::Return},
            {"domain", TokenKind::Domain}, {"true", TokenKind::True},
            {"false", TokenKind::False}
        };
        const auto found = keywords.find(text);
        add(found == keywords.end() ? TokenKind::Identifier : found->second, start, location);
    }

    void Lexer::scanNumber() {
        const std::size_t start = current_ - 1;
        const SourceLocation location{line_, column_ - 1, start};
        while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
        if (peek() == '.' &&  std::isdigit(static_cast<unsigned char>(peek(1)))) {
            advance();
            while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
        }
        
        if (peek() == 'e' || peek() == 'E') {
            std::size_t lookahead = current_ + 1;
            if (lookahead < source_.size() && (source_[lookahead] == '+' || source_[lookahead] == '-')) ++lookahead;
            if (lookahead < source_.size() && std::isdigit(static_cast<unsigned char>(source_[lookahead]))) {
                advance();
                if (peek() == '+' || peek() == '-') advance();
                while(std::isdigit(static_cast<unsigned char>(peek()))) advance();
            }
        }
        add(TokenKind::Number, start, location);
    }

    void Lexer::scanString() {
        const std::size_t start = current_ - 1;
        const SourceLocation location{line_, column_ - 1, start};
        const char quote = source_[start];
        bool terminated = false;
        while (!atEnd()) {
            if (peek() == '\n') break;
            if (peek() == '\\') {
                advance();
                if (!atEnd()) advance();
                continue;
            }
            if (advance() == quote) {terminated = true; break;}
        }

        if (!terminated) {
            error("SyntaxError", "Undetermined string literal", location);
            add(TokenKind::Invalid, start, location);
            return;
        }
        add(TokenKind::String, start, location);
    }

    void Lexer::scanToken() {
        const std::size_t start = current_ - 1;
        const SourceLocation location{line_, colum_ - 1, start};
        const char c = source_[start];
        auto one = [&](TokenKind kind) {add(kind, start, location);};

        switch (c) {
            case ' ': case '\r': case '\t': return;
            case '#': while (!atEnd() && peek() != '\n') advance(); return;
            case '\n':
                if (bracketDepth_ == 0 && lineHasCode_) 
                    tokens_.push_back(Token{TokenKind::Newline, "", location});
                lineHasCode_ = false;
                atLineStart_ = true;
                return;
            case '+': one(TokenKind::Plus); break;
            case '-': if (match('>')) one(TokenKind::Arrow); else one(TokenKind::Minus); break;
            case '*': one(TokenKind::Star); break;
            case '/': one(TokenKind::Slash); break;
            case '%': one(TokenKind::Percent); break;
            case '^': one(TokenKind::Caret); break;
            case '=': if (match('=')) one(TokenKind::EqualEqual); else one(TokenKind::Equal); break;
            case '!': if (match('=')) one(TokenKind::NotEqual); else {one(TokenKind::Invalid); error("SyntaxError", "Expected '=' after '!'.", location);} break;
            case '<': if (match('=')) one(TokenKind::LessEqual); else one(TokenKind::Less); break;
            case '>': if (match('=')) one(TokenKind::GreaterEqual); else one(TokenKind::Greater); break;
            case '(': ++bracketDepth_; one(TokenKind::LeftParen); break;
            case ')': if (bracketDepth_ > 0) --bracketDepth_; one(TokenKind::RightParen); break;
            case ',': one(TokenKind::Comma); break;
            case ':': one(TokenKind::Colon); break;
            case '.': one(TokenKind::Dot); break;
            case '\'': case '"': scanString(); break;
            default:
                if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') scanIdentifier();
                else if (std::isdigit(static_cast<unsigned char>(c))) scanNumber();
                else {
                    one(TokenKind::Invalid);
                    error("SyntaxError", std::string("Unexpected character '") + c + "'.", location);
                }
                break;
        }
        if (c != ' ' && c != '\r' && c != '\t' && c != '#') lineHasCode_ = true;
    }

    LexResult Lexer::tokenize() {
        while (!atEnd()) {
            if (atLineStart_ && bracketDepth_ == 0) {
                processIndentation();
                if (atLineStart_) continue;
            }
            const char c = advance();
            if (c == '\n') {
                if (bracketDepth_ == 0 && lineHasCode_) {
                    tokens_.push_back(Token{TokenKind::Newline, "",
                                            SourceLocation{line_ - 1, 1, current_ - 1}});
                }
                lineHasCode_ = false;
                atLineStart_ = true;
                continue;
            }
            --current_;
            --column_;
            advance();
            scanToken();
        }

        if (lineHasCode_) {
            tokens_.push_back(Token{TokenKind::Newline, "", SourceLocation{line_, column_, current_}});
        }
        while (indentation_.size() > 1) {
            indentation_.pop_back();
            alternateIndentation_.pop_back();
            tokens_.push_back(Token{TokenKind::Dedent, "", SourceLocation{line_, column_, current_}});
        }
        tokens_.push_back(Token{TokenKind::EndOfFile, "", SourceLocation{line_, column_, current_}});
        return LexResult{std::move(tokens_), std::move(diagnostics_)};
    }

}