#pragma once

// mini-toolbridge —— 大模型驱动的工业软件指令桥接层
//
// 链路：自然语言 →（LLM）→ 受约束 JSON 指令 → 七层校验 → 执行 → 建模产物
//
// 设计要点：
//   1. 组件库限定模型的可选集合，输出空间从自由文本收窄为合法指令
//   2. 七层校验（语法/结构/类型/必填/范围/引用/顺序），错误带 JSON 路径
//   3. 自定义组件 = 已有组件的参数化组合，合法性可递归证明
//   4. LLM 通过 ILlmProvider 抽象接入，可换任意 OpenAI 兼容端点

#include <boost/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tb {

// ============================================================ 组件库

enum class ArgType { Number, String, Bool, Ref };

struct Argument {
    std::string name;
    ArgType type = ArgType::String;
    bool required = true;
    double min = 0.0;
    double max = 0.0;
    bool has_range = false;
    std::string ref_kind;
};

struct Command {
    std::string name;
    std::string category;
    std::vector<Argument> args;
    std::vector<std::string> preconditions;   // 例："canvas_exists"
    std::string summary;                      // 给模型看的一句话说明
};

class ComponentLibrary {
public:
    void add(Command cmd);
    const Command* find(std::string_view name) const;
    std::vector<std::string> names() const;
    const std::map<std::string, Command, std::less<>>& all() const { return commands_; }

private:
    std::map<std::string, Command, std::less<>> commands_;
};

// ============================================================ 校验

enum class Layer { Syntax, Structure, Type, Required, Range, Reference, Order };
std::string_view layer_name(Layer layer);

struct ValidationError {
    Layer layer = Layer::Structure;
    std::string path;
    std::string message;
    std::string describe() const;
};

struct Instruction {
    std::string id;
    std::string op;
    boost::json::object args;
};

class ModelContext {
public:
    void add_object(std::string id);
    void add_kind(std::string kind);
    bool has_object(std::string_view id) const;
    bool has_kind(std::string_view kind) const;
    std::size_t object_count() const { return objects_.size(); }

private:
    std::vector<std::string> objects_;
    std::vector<std::string> kinds_;
};

using ParseResult = std::variant<boost::json::value, ValidationError>;
using ValidateResult = std::variant<Instruction, ValidationError>;

ParseResult parse(std::string_view text);
ValidateResult validate(const boost::json::value& doc, const ComponentLibrary& library);
std::optional<ValidationError> check_preconditions(const Instruction& ins,
                                                   const ComponentLibrary& library,
                                                   const ModelContext& ctx);

// ============================================================ 自定义组件

using Expansion = std::vector<Instruction>;
using Expander = std::function<Expansion(const boost::json::object&)>;

struct CompositeComponent {
    std::string name;
    std::string summary;
    std::vector<Argument> params;
    Expander expand;
};

class CompositeRegistry {
public:
    void add(CompositeComponent c);
    const CompositeComponent* find(std::string_view name) const;
    std::vector<std::string> names() const;

private:
    std::map<std::string, CompositeComponent, std::less<>> components_;
};

// ============================================================ 执行器（SVG 后端）

class SvgExecutor {
public:
    void begin(double width, double height, const std::string& bg);
    bool ready() const { return open_; }
    bool execute(const Instruction& ins);   // 按 op 派发
    std::string document() const;
    std::size_t element_count() const { return elements_; }

private:
    void emit(const std::string& line);

    bool open_ = false;
    double w_ = 0;
    double h_ = 0;
    std::string bg_ = "#ffffff";
    std::string body_;
    std::size_t elements_ = 0;
};

// 显示建模组件库（canvas / circle / line / text / hand）
ComponentLibrary make_display_library();
// 自定义组件示例：齿轮 = 2 圆 + N 条辐条
CompositeComponent make_gear_component();

// ============================================================ 管道

// 完整执行一次：文本 → 校验 → 执行 → SVG
struct PipelineStats {
    int applied = 0;
    int rejected = 0;
    std::vector<std::string> errors;
};

PipelineStats run_instructions(const std::string& json_text,
                               const ComponentLibrary& lib,
                               const CompositeRegistry& composites,
                               ModelContext& ctx,
                               SvgExecutor& out);

// ============================================================ LLM 接口

struct LlmConfig {
    std::string api_base;   // 例：https://api.deepseek.com/v1
    std::string api_key;
    std::string model;      // 例：deepseek-chat
    int timeout_sec = 60;
};

struct LlmReply {
    bool ok = false;
    std::string text;
    std::string error;
};

class ILlmProvider {
public:
    virtual ~ILlmProvider() = default;
    virtual LlmReply complete(const std::string& system_prompt,
                              const std::string& user_prompt) = 0;
};

// 离线调试用：固定返回一段预设文本，不联网
std::unique_ptr<ILlmProvider> make_scripted_provider(std::string fixed_reply);

// 走 curl 调任意 OpenAI 兼容端点（DeepSeek / Qwen / Ollama / vLLM 均可）
std::unique_ptr<ILlmProvider> make_curl_provider(LlmConfig config);

// 从环境变量读：TB_API_BASE / TB_API_KEY / TB_MODEL
LlmConfig llm_config_from_env();

// 把组件库渲染成给模型看的说明（system prompt 的一部分）
std::string render_library_prompt(const ComponentLibrary& lib,
                                  const CompositeRegistry& composites);

// 从模型回复里抽出指令数组。容忍 ```json 围栏与前后废话。
std::vector<std::string> extract_instruction_array(const std::string& reply);

}  // namespace tb
