// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  lexer.cpp · Lua Lexer                      │
// └─────────────────────────────────────────────┘

#include "lexer.h"
#include <cctype>
#include <stdexcept>

namespace clx {

//------------------ LEXER: constructor - initializes lexer with source text and filename
Lexer::Lexer(std::string_view source, const char *filename)
    : src(source)
    , pos(0)
    , current_line(1)
    , filename_str(filename) {
    advance();
}

//------------------ LEXER: remaining_source - returns unprocessed source text after current position
std::string_view Lexer::remaining_source() const {
    return src.substr(pos);
}

//------------------ LEXER: advance - scans next token, skipping whitespace and comments
void Lexer::advance() {
    while (pos < src.length()) {
        if (src[pos] == '\n') {
            current_line++;
            pos++;
            continue;
        }
        if (std::isspace(src[pos])) {
            pos++;
            continue;
        }
        if (src[pos] == '-' && pos + 1 < src.length() && src[pos + 1] == '-') {
            pos += 2;
            if (pos < src.length() && src[pos] == '[') {
                size_t eq = 0;
                while (pos + eq + 1 < src.length() && src[pos + 1 + eq] == '=')
                    eq++;
                if (pos + 1 + eq < src.length() && src[pos + 1 + eq] == '[') {
                    pos += 2 + eq;
                    std::string close;
                    close += ']';
                    close.append(eq, '=');
                    close += ']';
                    while (pos + eq + 1 < src.length()) {
                        if (src[pos] == '\n')
                            current_line++;
                        if (src.compare(pos, close.size(), close) == 0) {
                            pos += close.size();
                            break;
                        }
                        pos++;
                    }
                } else {
                    while (pos < src.length() && src[pos] != '\n')
                        pos++;
                }
            } else {
                while (pos < src.length() && src[pos] != '\n')
                    pos++;
            }
            continue;
        }
        if (src[pos] == '#' && pos + 1 < src.length() && src[pos + 1] == '!') {
            while (pos < src.length() && src[pos] != '\n')
                pos++;
            continue;
        }
        break;
    }
    if (pos >= src.length()) {
        current_token = Token { TokenType::TokEof, std::string_view(), 0.0, current_line };
        return;
    }
    if (std::isalpha(src[pos]) || src[pos] == '_') {
        size_t start = pos;
        while (pos < src.length() && (std::isalnum(src[pos]) || src[pos] == '_'))
            pos++;
        std::string_view text(src.data() + start, pos - start);
        TokenType type = TokenType::TokIdent;
        if (text == "local")
            type = TokenType::TokLocal;
        else if (text == "global")
            type = TokenType::TokGlobal;
        else if (text == "if")
            type = TokenType::TokIf;
        else if (text == "then")
            type = TokenType::TokThen;
        else if (text == "elseif")
            type = TokenType::TokElseIf;
        else if (text == "else")
            type = TokenType::TokElse;
        else if (text == "end")
            type = TokenType::TokEnd;
        else if (text == "while")
            type = TokenType::TokWhile;
        else if (text == "do")
            type = TokenType::TokDo;
        else if (text == "repeat")
            type = TokenType::TokRepeat;
        else if (text == "until")
            type = TokenType::TokUntil;
        else if (text == "for")
            type = TokenType::TokFor;
        else if (text == "function")
            type = TokenType::TokFunction;
        else if (text == "return")
            type = TokenType::TokReturn;
        else if (text == "and")
            type = TokenType::TokAnd;
        else if (text == "or")
            type = TokenType::TokOr;
        else if (text == "not")
            type = TokenType::TokNot;
        else if (text == "true")
            type = TokenType::TokTrue;
        else if (text == "false")
            type = TokenType::TokFalse;
        else if (text == "nil")
            type = TokenType::TokNil;
        else if (text == "goto")
            type = TokenType::TokGoto;
        else if (text == "break")
            type = TokenType::TokBreak;
        current_token = { type, text, 0, current_line };
        return;
    }
    if (std::isdigit(src[pos])) {
        char *end;
        double val = std::strtod(src.data() + pos, &end);
        size_t len = end - (src.data() + pos);
        std::string_view text(src.data() + pos, len);
        pos += len;
        current_token = { TokenType::TokNumber, text, val, current_line };
        return;
    }
    if (src[pos] == '"' || src[pos] == '\'') {
        char quote = src[pos++];
        size_t start = pos;
        while (pos < src.length() && src[pos] != quote) {
            if (src[pos] == '\\') {
                if (pos + 1 >= src.length()) {
                    pos++;
                    break;
                }
                char esc = src[pos + 1];
                if (esc == '\n' || esc == '\r') {
                    pos += 2;
                    if (pos < src.length() && (src[pos] == '\n' || src[pos] == '\r') && src[pos] != esc)
                        pos++;
                    current_line++;
                    continue;
                }
                if (esc == 'z') {
                    pos += 2;
                    while (pos < src.length() && std::isspace(static_cast<unsigned char>(src[pos]))) {
                        char ws = src[pos++];
                        if (ws == '\n' || ws == '\r') {
                            if (pos < src.length() && (src[pos] == '\n' || src[pos] == '\r') && src[pos] != ws)
                                pos++;
                            current_line++;
                        }
                    }
                    continue;
                }
                pos += 2;
                continue;
            }
            if (src[pos] == '\n' || src[pos] == '\r')
                throw std::runtime_error(std::to_string(current_line) + ": unfinished string");
            pos++;
        }
        if (pos >= src.length())
            throw std::runtime_error(std::to_string(current_line) + ": unfinished string");
        std::string_view text(src.data() + start, pos - start);
        if (pos < src.length() && src[pos] == quote)
            pos++;
        current_token = { TokenType::TokString, text, 0, current_line };
        return;
    }
    char c = src[pos++];
    switch (c) {
    case '+':
        current_token = { TokenType::TokPlus, "+", 0, current_line };
        return;
    case '-':
        current_token = { TokenType::TokMinus, "-", 0, current_line };
        return;
    case '*':
        current_token = { TokenType::TokStar, "*", 0, current_line };
        return;
    case '/':
        if (pos < src.length() && src[pos] == '/') {
            pos++;
            current_token = { TokenType::TokFloorDiv, "//", 0, current_line };
        } else {
            current_token = { TokenType::TokSlash, "/", 0, current_line };
        }
        return;
    case '(':
        current_token = { TokenType::TokLParen, "(", 0, current_line };
        return;
    case ')':
        current_token = { TokenType::TokRParen, ")", 0, current_line };
        return;
    case '{':
        current_token = { TokenType::TokLBrace, "{", 0, current_line };
        return;
    case '}':
        current_token = { TokenType::TokRBrace, "}", 0, current_line };
        return;
    case '[': {
        size_t eq = 0;
        while (pos + eq < src.length() && src[pos + eq] == '=')
            eq++;
        if (pos + eq < src.length() && src[pos + eq] == '[') {
            size_t close_eq = eq;
            pos += eq + 1;
            if (pos < src.length() && src[pos] == '\r')
                pos++;
            if (pos < src.length() && src[pos] == '\n') {
                pos++;
                current_line++;
            }
            size_t start = pos;
            std::string close;
            close += ']';
            close.append(close_eq, '=');
            close += ']';
            while (pos + close.size() <= src.length()) {
                if (src[pos] == '\n')
                    current_line++;
                if (src.compare(pos, close.size(), close) == 0) {
                    std::string_view text(src.data() + start, pos - start);
                    pos += close.size();
                    current_token = { TokenType::TokString, text, 0, current_line, true };
                    return;
                }
                pos++;
            }
            throw std::runtime_error(std::to_string(current_line) + ": unfinished long string");
        }
        current_token = { TokenType::TokLBracket, "[", 0, current_line };
    }
        return;
    case ']':
        current_token = { TokenType::TokRBracket, "]", 0, current_line };
        return;
    case ':':
        if (pos < src.length() && src[pos] == ':') {
            pos++;
            current_token = { TokenType::TokDoubleColon, "::", 0, current_line };
        } else {
            current_token = { TokenType::TokColon, ":", 0, current_line };
        }
        return;
    case '#':
        current_token = { TokenType::TokLen, "#", 0, current_line };
        return;
    case '.':
        if (pos + 1 < src.length() && src[pos] == '.' && src[pos + 1] == '.') {
            pos += 2;
            current_token = { TokenType::TokVararg, "...", 0, current_line };
        } else if (pos < src.length() && src[pos] == '.') {
            pos++;
            current_token = { TokenType::TokConcat, "..", 0, current_line };
        } else if (pos < src.length() && std::isdigit(src[pos])) {
            char *end = nullptr;
            double val = std::strtod(src.data() + pos - 1, &end);
            size_t len = end - (src.data() + pos - 1);
            current_token = { TokenType::TokNumber, std::string_view(src.data() + pos - 1, len), val, current_line };
            pos = end - src.data();
        } else {
            current_token = { TokenType::TokDot, ".", 0, current_line };
        }
        return;
    case ',':
        current_token = { TokenType::TokComma, ",", 0, current_line };
        return;
    case ';':
        current_token = { TokenType::TokSemicolon, ";", 0, current_line };
        return;
    case '=':
        if (pos < src.length() && src[pos] == '=') {
            pos++;
            current_token = { TokenType::TokEqEq, "==", 0, current_line };
        } else {
            current_token = { TokenType::TokAssign, "=", 0, current_line };
        }
        return;
    case '<':
        if (pos < src.length() && src[pos] == '<') {
            pos++;
            current_token = { TokenType::TokShl, "<<", 0, current_line };
        } else if (pos < src.length() && src[pos] == '=') {
            pos++;
            current_token = { TokenType::TokLessEq, "<=", 0, current_line };
        } else {
            current_token = { TokenType::TokLess, "<", 0, current_line };
        }
        return;
    case '>':
        if (pos < src.length() && src[pos] == '>') {
            pos++;
            current_token = { TokenType::TokShr, ">>", 0, current_line };
        } else if (pos < src.length() && src[pos] == '=') {
            pos++;
            current_token = { TokenType::TokGreaterEq, ">=", 0, current_line };
        } else {
            current_token = { TokenType::TokGreater, ">", 0, current_line };
        }
        return;
    case '~':
        if (pos < src.length() && src[pos] == '=') {
            pos++;
            current_token = { TokenType::TokNotEq, "~=", 0, current_line };
        } else
            current_token = { TokenType::TokBitXor, "~", 0, current_line };
        return;
    case '%':
        current_token = { TokenType::TokMod, "%", 0, current_line };
        return;
    case '^':
        current_token = { TokenType::TokPow, "^", 0, current_line };
        return;
    case '&':
        current_token = { TokenType::TokBitAnd, "&", 0, current_line };
        return;
    case '|':
        current_token = { TokenType::TokBitOr, "|", 0, current_line };
        return;
    }
    throw std::runtime_error(std::to_string(current_line) + ": unexpected symbol near '" + std::string(1, c) + "'");
}

//------------------ LEXER: current - returns current token
const Token &Lexer::current() const {
    return current_token;
}

//------------------ LEXER: position - returns current source byte position
size_t Lexer::position() const {
    return pos;
}

//------------------ LEXER: line - returns current line number
int Lexer::line() const {
    return current_line;
}

//------------------ LEXER: filename - returns source filename
const char *Lexer::filename() const {
    return filename_str.c_str();
}
}