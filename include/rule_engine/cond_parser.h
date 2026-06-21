#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace cortexlink {

// ============================================================
// Token types
// ============================================================
enum class TokenType {
    LPAREN,
    RPAREN,
    AND,
    OR,
    GT,
    LT,
    GE,
    LE,
    EQ,
    NE,
    EVENT_PARAM,   // {event.xxx}
    DEVICE_DATA,   // {dev_uuid.data_name}
    TIME,          // {time}
    NUMBER,
    STRING,
    END,
};

// ============================================================
// AST node types
// ============================================================
enum class AstType {
    BINARY_OP,
    EVENT_PARAM,
    DEVICE_DATA,
    TIME,
    NUMBER_LITERAL,
    STRING_LITERAL,
};

struct AstNode {
    AstType type;

    // For BINARY_OP
    TokenType op = TokenType::END;
    std::unique_ptr<AstNode> left;
    std::unique_ptr<AstNode> right;

    // For literals and param/data references
    std::string value;

    // For DEVICE_DATA
    std::string dev_id;
    std::string data_name;

    static std::unique_ptr<AstNode> MakeBinary(TokenType op,
                                               std::unique_ptr<AstNode> left,
                                               std::unique_ptr<AstNode> right);
    static std::unique_ptr<AstNode> MakeEventParam(const std::string &name);
    static std::unique_ptr<AstNode> MakeDeviceData(const std::string &dev_id,
                                                   const std::string &data_name);
    static std::unique_ptr<AstNode> MakeTime();
    static std::unique_ptr<AstNode> MakeNumber(const std::string &val);
    static std::unique_ptr<AstNode> MakeString(const std::string &val);
};

// ============================================================
// Evaluation context
// ============================================================
struct EvalContext {
    // Event parameters from the triggering event (param_name → value string)
    std::unordered_map<std::string, std::string> event_params;

    // Callback to read device data: (dev_id, data_name) → value or nullopt
    std::function<std::optional<std::string>(const std::string &dev_id,
                                              const std::string &data_name)>
        get_device_data;

    // Current time (east-8)
    int hour = 0;
    int min = 0;
    int sec = 0;
};

// ============================================================
// Parser API
// ============================================================

// Parse cond_expr, extract the condition bound to `evt_id_str`,
// and return its AST.
// Returns nullptr if the evt_id is not found in cond_expr, or if
// its condition is empty (e.g. "evt_id()" means always-true).
std::unique_ptr<AstNode> ParseCondition(const std::string &cond_expr,
                                         const std::string &evt_id_str);

// Evaluate an AST node against the given context. Returns true/false.
bool EvaluateAst(const AstNode *node, const EvalContext &ctx);

}  // namespace cortexlink
