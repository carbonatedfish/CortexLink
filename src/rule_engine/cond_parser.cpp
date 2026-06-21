#include "rule_engine/cond_parser.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include <spdlog/spdlog.h>

namespace cortexlink {

// ============================================================
// AstNode factory methods
// ============================================================

std::unique_ptr<AstNode> AstNode::MakeBinary(TokenType op,
                                             std::unique_ptr<AstNode> left,
                                             std::unique_ptr<AstNode> right) {
    auto node = std::make_unique<AstNode>();
    node->type = AstType::BINARY_OP;
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    return node;
}

std::unique_ptr<AstNode> AstNode::MakeEventParam(const std::string &name) {
    auto node = std::make_unique<AstNode>();
    node->type = AstType::EVENT_PARAM;
    node->value = name;
    return node;
}

std::unique_ptr<AstNode> AstNode::MakeDeviceData(const std::string &dev_id,
                                                  const std::string &data_name) {
    auto node = std::make_unique<AstNode>();
    node->type = AstType::DEVICE_DATA;
    node->dev_id = dev_id;
    node->data_name = data_name;
    return node;
}

std::unique_ptr<AstNode> AstNode::MakeTime() {
    auto node = std::make_unique<AstNode>();
    node->type = AstType::TIME;
    return node;
}

std::unique_ptr<AstNode> AstNode::MakeNumber(const std::string &val) {
    auto node = std::make_unique<AstNode>();
    node->type = AstType::NUMBER_LITERAL;
    node->value = val;
    return node;
}

std::unique_ptr<AstNode> AstNode::MakeString(const std::string &val) {
    auto node = std::make_unique<AstNode>();
    node->type = AstType::STRING_LITERAL;
    node->value = val;
    return node;
}

// ============================================================
// Token (internal)
// ============================================================
namespace {

struct Token {
    TokenType type = TokenType::END;
    std::string value;       // for EVENT_PARAM(name), NUMBER, STRING
    std::string dev_id;      // for DEVICE_DATA
    std::string data_name;   // for DEVICE_DATA
};

// ============================================================
// Lexer (internal)
// ============================================================
class Lexer {
public:
    explicit Lexer(const std::string &input)
        : input_(input), pos_(0) {}

    Token Next() {
        SkipWhitespace();
        if (pos_ >= input_.size()) {
            return {TokenType::END, ""};
        }

        char c = input_[pos_];

        // Two-character tokens
        if (c == '&' && Peek(1) == '&') {
            pos_ += 2;
            return {TokenType::AND, ""};
        }
        if (c == '|' && Peek(1) == '|') {
            pos_ += 2;
            return {TokenType::OR, ""};
        }
        if (c == '>' && Peek(1) == '=') {
            pos_ += 2;
            return {TokenType::GE, ""};
        }
        if (c == '<' && Peek(1) == '=') {
            pos_ += 2;
            return {TokenType::LE, ""};
        }
        if (c == '=' && Peek(1) == '=') {
            pos_ += 2;
            return {TokenType::EQ, ""};
        }
        if (c == '!' && Peek(1) == '=') {
            pos_ += 2;
            return {TokenType::NE, ""};
        }

        // Single-character tokens
        if (c == '(') { pos_++; return {TokenType::LPAREN, ""}; }
        if (c == ')') { pos_++; return {TokenType::RPAREN, ""}; }
        if (c == '>') { pos_++; return {TokenType::GT, ""}; }
        if (c == '<') { pos_++; return {TokenType::LT, ""}; }

        // Braced references: {event.xxx}, {dev_id.data_name}, {time}
        if (c == '{') return LexBraced();

        // String literals
        if (c == '"') return LexString();

        // Numbers
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && std::isdigit(static_cast<unsigned char>(Peek(1))))) {
            return LexNumber();
        }

        // Unexpected character — skip and continue
        spdlog::warn("Lexer: unexpected character '{}' at position {}", c, pos_);
        pos_++;
        return Next();
    }

    size_t pos() const { return pos_; }

private:
    void SkipWhitespace() {
        while (pos_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            pos_++;
        }
    }

    char Peek(int offset) const {
        size_t idx = pos_ + static_cast<size_t>(offset);
        if (idx >= input_.size()) return '\0';
        return input_[idx];
    }

    Token LexBraced() {
        pos_++;  // skip '{'
        std::string content;
        while (pos_ < input_.size() && input_[pos_] != '}') {
            content += input_[pos_];
            pos_++;
        }
        if (pos_ < input_.size()) {
            pos_++;  // skip '}'
        } else {
            spdlog::warn("Lexer: unclosed brace reference");
        }

        if (content.empty()) {
            spdlog::warn("Lexer: empty brace reference");
            return {TokenType::END, ""};
        }

        // {time}
        if (content == "time") {
            return {TokenType::TIME, ""};
        }

        // {event.xxx}
        if (content.size() > 6 && content.compare(0, 6, "event.") == 0) {
            std::string param_name = content.substr(6);
            if (param_name.empty()) {
                spdlog::warn("Lexer: empty event param name");
            }
            Token t{TokenType::EVENT_PARAM, ""};
            t.value = param_name;
            return t;
        }

        // {dev_id.data_name}
        auto dot_pos = content.find('.');
        if (dot_pos != std::string::npos) {
            std::string did = content.substr(0, dot_pos);
            std::string dname = content.substr(dot_pos + 1);
            if (did.empty() || dname.empty()) {
                spdlog::warn("Lexer: malformed device data reference {{{}}}", content);
            }
            Token t{TokenType::DEVICE_DATA, ""};
            t.dev_id = did;
            t.data_name = dname;
            return t;
        }

        // Bare identifier — treat as device data with empty data_name (recovery)
        spdlog::warn("Lexer: unrecognized brace reference {{{}}}", content);
        Token t{TokenType::DEVICE_DATA, ""};
        t.dev_id = content;
        return t;
    }

    Token LexString() {
        pos_++;  // skip opening '"'
        std::string val;
        while (pos_ < input_.size() && input_[pos_] != '"') {
            if (input_[pos_] == '\\' && pos_ + 1 < input_.size()) {
                pos_++;
                char escaped = input_[pos_];
                switch (escaped) {
                    case 'n':  val += '\n'; break;
                    case 't':  val += '\t'; break;
                    case '\\': val += '\\'; break;
                    case '"':  val += '"';  break;
                    default:   val += escaped; break;
                }
            } else {
                val += input_[pos_];
            }
            pos_++;
        }
        if (pos_ < input_.size()) {
            pos_++;  // skip closing '"'
        }
        Token t{TokenType::STRING, ""};
        t.value = val;
        return t;
    }

    Token LexNumber() {
        std::string val;
        bool has_dot = false;
        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                val += c;
                pos_++;
            } else if (c == '.' && !has_dot) {
                has_dot = true;
                val += c;
                pos_++;
            } else {
                break;
            }
        }
        Token t{TokenType::NUMBER, ""};
        t.value = val;
        return t;
    }

    std::string input_;
    size_t pos_;
};

// ============================================================
// Parser — recursive descent (internal)
// ============================================================
class Parser {
public:
    explicit Parser(const std::string &source) : lexer_(source) {
        Advance();
    }

    std::unique_ptr<AstNode> Parse() {
        if (cur_.type == TokenType::END) {
            return nullptr;  // empty condition → always true
        }
        auto ast = ParseOrExpr();
        if (cur_.type != TokenType::END) {
            spdlog::warn("Parser: unexpected token '{}' after expression "
                         "at position {}",
                         TokenName(cur_.type), lexer_.pos());
        }
        return ast;
    }

private:
    void Advance() {
        cur_ = lexer_.Next();
    }

    bool Match(TokenType type) {
        if (cur_.type == type) {
            Advance();
            return true;
        }
        return false;
    }

    static const char *TokenName(TokenType t) {
        switch (t) {
            case TokenType::LPAREN:      return "(";
            case TokenType::RPAREN:      return ")";
            case TokenType::AND:         return "&&";
            case TokenType::OR:          return "||";
            case TokenType::GT:          return ">";
            case TokenType::LT:          return "<";
            case TokenType::GE:          return ">=";
            case TokenType::LE:          return "<=";
            case TokenType::EQ:          return "==";
            case TokenType::NE:          return "!=";
            case TokenType::EVENT_PARAM: return "{event.…}";
            case TokenType::DEVICE_DATA: return "{dev.data}";
            case TokenType::TIME:        return "{time}";
            case TokenType::NUMBER:      return "<number>";
            case TokenType::STRING:      return "<string>";
            case TokenType::END:         return "<end>";
        }
        return "<?>";
    }

    // or_expr → and_expr ("||" and_expr)*
    std::unique_ptr<AstNode> ParseOrExpr() {
        auto left = ParseAndExpr();
        if (!left) return nullptr;

        while (cur_.type == TokenType::OR) {
            TokenType op = cur_.type;
            Advance();
            auto right = ParseAndExpr();
            if (!right) {
                spdlog::error("Parser: expected expression after ||");
                return nullptr;
            }
            left = AstNode::MakeBinary(op, std::move(left), std::move(right));
        }
        return left;
    }

    // and_expr → comparison ("&&" comparison)*
    std::unique_ptr<AstNode> ParseAndExpr() {
        auto left = ParseComparison();
        if (!left) return nullptr;

        while (cur_.type == TokenType::AND) {
            TokenType op = cur_.type;
            Advance();
            auto right = ParseComparison();
            if (!right) {
                spdlog::error("Parser: expected expression after &&");
                return nullptr;
            }
            left = AstNode::MakeBinary(op, std::move(left), std::move(right));
        }
        return left;
    }

    // comparison → primary ((>|<|>=|<=|==|!=) primary)?
    std::unique_ptr<AstNode> ParseComparison() {
        auto left = ParsePrimary();
        if (!left) return nullptr;

        if (IsComparisonOp(cur_.type)) {
            TokenType op = cur_.type;
            Advance();
            auto right = ParsePrimary();
            if (!right) {
                spdlog::error("Parser: expected expression after comparison operator");
                return nullptr;
            }
            left = AstNode::MakeBinary(op, std::move(left), std::move(right));
        }
        return left;
    }

    // primary → "(" or_expr ")"
    //         | EVENT_PARAM  | DEVICE_DATA  | TIME
    //         | NUMBER       | STRING
    std::unique_ptr<AstNode> ParsePrimary() {
        if (Match(TokenType::LPAREN)) {
            auto expr = ParseOrExpr();
            if (!expr) {
                spdlog::error("Parser: expected expression inside parentheses");
                return nullptr;
            }
            if (!Match(TokenType::RPAREN)) {
                spdlog::error("Parser: expected ')'");
                return nullptr;
            }
            return expr;
        }

        if (cur_.type == TokenType::EVENT_PARAM) {
            std::string name = cur_.value;
            Advance();
            return AstNode::MakeEventParam(name);
        }

        if (cur_.type == TokenType::DEVICE_DATA) {
            std::string did = cur_.dev_id;
            std::string dname = cur_.data_name;
            Advance();
            return AstNode::MakeDeviceData(did, dname);
        }

        if (cur_.type == TokenType::TIME) {
            Advance();
            return AstNode::MakeTime();
        }

        if (cur_.type == TokenType::NUMBER) {
            std::string val = cur_.value;
            Advance();
            return AstNode::MakeNumber(val);
        }

        if (cur_.type == TokenType::STRING) {
            std::string val = cur_.value;
            Advance();
            return AstNode::MakeString(val);
        }

        spdlog::error("Parser: unexpected token '{}'", TokenName(cur_.type));
        return nullptr;
    }

    static bool IsComparisonOp(TokenType t) {
        return t == TokenType::GT || t == TokenType::LT ||
               t == TokenType::GE || t == TokenType::LE ||
               t == TokenType::EQ || t == TokenType::NE;
    }

    Lexer lexer_;
    Token cur_;
};

// ============================================================
// EvaluateAst helpers (internal)
// ============================================================

bool IsNumeric(const std::string &s) {
    if (s.empty()) return false;
    char *end = nullptr;
    std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && end != s.c_str();
}

double ToNumber(const std::string &s) {
    return std::strtod(s.c_str(), nullptr);
}

std::string EvalLeafValue(const AstNode *node, const EvalContext &ctx) {
    switch (node->type) {
    case AstType::NUMBER_LITERAL:
    case AstType::STRING_LITERAL:
        return node->value;

    case AstType::EVENT_PARAM: {
        auto it = ctx.event_params.find(node->value);
        if (it != ctx.event_params.end()) {
            return it->second;
        }
        spdlog::warn("EvaluateAst: event param '{}' not found in context",
                     node->value);
        return "";
    }

    case AstType::DEVICE_DATA: {
        if (ctx.get_device_data) {
            auto val = ctx.get_device_data(node->dev_id, node->data_name);
            if (val.has_value()) return *val;
        }
        spdlog::warn("EvaluateAst: device data '{}.{}' not found",
                     node->dev_id, node->data_name);
        return "";
    }

    case AstType::TIME: {
        char buf[6];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", ctx.hour, ctx.min);
        return buf;
    }

    case AstType::BINARY_OP:
        spdlog::error("EvaluateAst: BINARY_OP reached in leaf evaluation");
        return "";
    }
    return "";
}

bool CompareValues(const std::string &lhs, const std::string &rhs, TokenType op) {
    bool lhs_num = IsNumeric(lhs);
    bool rhs_num = IsNumeric(rhs);

    if (lhs_num && rhs_num) {
        double l = ToNumber(lhs);
        double r = ToNumber(rhs);
        switch (op) {
            case TokenType::GT: return l > r;
            case TokenType::LT: return l < r;
            case TokenType::GE: return l >= r;
            case TokenType::LE: return l <= r;
            case TokenType::EQ: return l == r;
            case TokenType::NE: return l != r;
            default: return false;
        }
    } else {
        switch (op) {
            case TokenType::GT: return lhs > rhs;
            case TokenType::LT: return lhs < rhs;
            case TokenType::GE: return lhs >= rhs;
            case TokenType::LE: return lhs <= rhs;
            case TokenType::EQ: return lhs == rhs;
            case TokenType::NE: return lhs != rhs;
            default: return false;
        }
    }
}

}  // anonymous namespace

// ============================================================
// ParseCondition (public API)
// ============================================================

std::unique_ptr<AstNode> ParseCondition(const std::string &cond_expr,
                                         const std::string &evt_id_str) {
    if (cond_expr.empty()) return nullptr;

    std::istringstream stream(cond_expr);
    std::string line;

    while (std::getline(stream, line)) {
        // Strip trailing \r (CRLF)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) continue;

        // Find first '(' — everything before it is the evt_id
        size_t paren_start = line.find('(');
        if (paren_start == std::string::npos) {
            spdlog::warn("ParseCondition: no '(' found in line: {}", line);
            continue;
        }

        std::string line_evt_id = line.substr(0, paren_start);

        // Trim whitespace around the evt_id
        size_t id_start = 0;
        size_t id_end = line_evt_id.size();
        while (id_start < id_end &&
               std::isspace(static_cast<unsigned char>(line_evt_id[id_start]))) {
            id_start++;
        }
        while (id_end > id_start &&
               std::isspace(static_cast<unsigned char>(line_evt_id[id_end - 1]))) {
            id_end--;
        }
        line_evt_id = line_evt_id.substr(id_start, id_end - id_start);

        if (line_evt_id != evt_id_str) continue;

        // Found matching evt_id — extract condition inside matching parentheses
        size_t cond_start = paren_start + 1;
        int depth = 1;
        size_t cond_end = cond_start;
        while (cond_end < line.size() && depth > 0) {
            char c = line[cond_end];
            if (c == '(') depth++;
            else if (c == ')') depth--;
            if (depth > 0) cond_end++;
        }

        if (depth != 0) {
            spdlog::warn("ParseCondition: unmatched parentheses in line: {}", line);
            return nullptr;
        }

        std::string condition = line.substr(cond_start, cond_end - cond_start);

        // Trim whitespace
        size_t c_start = 0;
        size_t c_end = condition.size();
        while (c_start < c_end &&
               std::isspace(static_cast<unsigned char>(condition[c_start]))) {
            c_start++;
        }
        while (c_end > c_start &&
               std::isspace(static_cast<unsigned char>(condition[c_end - 1]))) {
            c_end--;
        }
        condition = condition.substr(c_start, c_end - c_start);

        if (condition.empty()) {
            return nullptr;  // always-true
        }

        Parser parser(condition);
        return parser.Parse();
    }

    return nullptr;  // evt_id not found
}

// ============================================================
// EvaluateAst (public API)
// ============================================================

bool EvaluateAst(const AstNode *node, const EvalContext &ctx) {
    if (!node) {
        return true;  // nullptr means always-true
    }

    switch (node->type) {
    case AstType::BINARY_OP: {
        switch (node->op) {
        case TokenType::AND:
            // Short-circuit AND
            if (!EvaluateAst(node->left.get(), ctx)) return false;
            return EvaluateAst(node->right.get(), ctx);

        case TokenType::OR:
            // Short-circuit OR
            if (EvaluateAst(node->left.get(), ctx)) return true;
            return EvaluateAst(node->right.get(), ctx);

        case TokenType::GT:
        case TokenType::LT:
        case TokenType::GE:
        case TokenType::LE:
        case TokenType::EQ:
        case TokenType::NE: {
            std::string lhs = EvalLeafValue(node->left.get(), ctx);
            std::string rhs = EvalLeafValue(node->right.get(), ctx);
            return CompareValues(lhs, rhs, node->op);
        }

        default:
            spdlog::error("EvaluateAst: unknown binary operator");
            return false;
        }
    }

    // Leaf nodes at top level — treat as truthy/falsy
    case AstType::NUMBER_LITERAL:
        return IsNumeric(node->value) && ToNumber(node->value) != 0.0;

    case AstType::STRING_LITERAL:
        return !node->value.empty();

    case AstType::EVENT_PARAM: {
        auto it = ctx.event_params.find(node->value);
        return it != ctx.event_params.end() && !it->second.empty();
    }

    case AstType::DEVICE_DATA: {
        if (ctx.get_device_data) {
            auto val = ctx.get_device_data(node->dev_id, node->data_name);
            return val.has_value() && !val->empty();
        }
        return false;
    }

    case AstType::TIME:
        return true;
    }

    return false;
}

}  // namespace cortexlink
