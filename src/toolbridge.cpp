#include "tb/toolbridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace tb {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::string std_str(boost::json::string_view sv) {
    return std::string(sv.data(), sv.size());
}

const char* kind_name(boost::json::kind k) {
    switch (k) {
        case boost::json::kind::object:  return "object";
        case boost::json::kind::array:   return "array";
        case boost::json::kind::string:  return "string";
        case boost::json::kind::int64:   return "number";
        case boost::json::kind::uint64:  return "number";
        case boost::json::kind::double_: return "number";
        case boost::json::kind::bool_:   return "bool";
        case boost::json::kind::null:    return "null";
    }
    return "unknown";
}

std::string join(const std::vector<std::string>& items, std::size_t limit = 10) {
    std::ostringstream os;
    for (std::size_t i = 0; i < items.size() && i < limit; ++i) {
        if (i != 0) {
            os << ", ";
        }
        os << items[i];
    }
    if (items.size() > limit) {
        os << " …(共 " << items.size() << " 条)";
    }
    return os.str();
}

std::string num(double v) {
    std::ostringstream os;
    os.precision(3);
    os << std::fixed << v;
    std::string s = os.str();
    while (s.size() > 1 && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s;
}

const Argument* find_arg(const Command& cmd, std::string_view name) {
    for (const auto& a : cmd.args) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

Instruction make_instruction(std::string op, boost::json::object args) {
    Instruction ins;
    ins.op = std::move(op);
    ins.args = std::move(args);
    return ins;
}

}  // namespace

// ============================================================ 组件库

void ComponentLibrary::add(Command cmd) { commands_[cmd.name] = std::move(cmd); }

const Command* ComponentLibrary::find(std::string_view name) const {
    auto it = commands_.find(name);
    return it == commands_.end() ? nullptr : &it->second;
}

std::vector<std::string> ComponentLibrary::names() const {
    std::vector<std::string> out;
    out.reserve(commands_.size());
    for (const auto& [k, _] : commands_) {
        out.push_back(k);
    }
    return out;
}

// ============================================================ 校验

std::string_view layer_name(Layer layer) {
    switch (layer) {
        case Layer::Syntax:    return "语法";
        case Layer::Structure: return "结构";
        case Layer::Type:      return "类型";
        case Layer::Required:  return "必填";
        case Layer::Range:     return "范围";
        case Layer::Reference: return "引用";
        case Layer::Order:     return "顺序";
    }
    return "未知";
}

std::string ValidationError::describe() const {
    std::ostringstream os;
    os << "[" << layer_name(layer) << "] ";
    if (!path.empty()) {
        os << path << ": ";
    }
    os << message;
    return os.str();
}

void ModelContext::add_object(std::string id) { objects_.push_back(std::move(id)); }
void ModelContext::add_kind(std::string kind) { kinds_.push_back(std::move(kind)); }

bool ModelContext::has_object(std::string_view id) const {
    return std::find(objects_.begin(), objects_.end(), id) != objects_.end();
}
bool ModelContext::has_kind(std::string_view kind) const {
    return std::find(kinds_.begin(), kinds_.end(), kind) != kinds_.end();
}

ParseResult parse(std::string_view text) {
    boost::json::error_code ec;
    boost::json::value v = boost::json::parse(text, ec);
    if (ec) {
        return ValidationError{Layer::Syntax, "", ec.message()};
    }
    return v;
}

ValidateResult validate(const boost::json::value& doc, const ComponentLibrary& library) {
    if (!doc.is_object()) {
        return ValidationError{Layer::Structure, "$", "顶层必须是一个 JSON object"};
    }
    const auto& obj = doc.as_object();

    if (!obj.contains("op") || !obj.at("op").is_string()) {
        return ValidationError{Layer::Structure, "op", "缺少字符串字段 op"};
    }
    const std::string op = std_str(obj.at("op").as_string());

    const Command* cmd = library.find(op);
    if (cmd == nullptr) {
        return ValidationError{Layer::Structure, "op",
                               "未知指令: " + op + "；可用指令: " + join(library.names())};
    }

    if (!obj.contains("args") || !obj.at("args").is_object()) {
        return ValidationError{Layer::Structure, "args", "缺少 object 字段 args"};
    }
    const auto& args = obj.at("args").as_object();

    std::string id;
    if (obj.contains("id")) {
        if (!obj.at("id").is_string()) {
            return ValidationError{Layer::Type, "id", "id 必须是字符串"};
        }
        id = std_str(obj.at("id").as_string());
    }

    for (const auto& declared : cmd->args) {
        const std::string path = "args." + declared.name;
        if (!args.contains(declared.name)) {
            if (declared.required) {
                return ValidationError{Layer::Required, path, "缺少必填参数 " + declared.name};
            }
            continue;
        }
        const auto& value = args.at(declared.name);
        switch (declared.type) {
            case ArgType::Number: {
                if (!value.is_number()) {
                    return ValidationError{Layer::Type, path,
                                           std::string("期望 number，收到 ") +
                                               kind_name(value.kind())};
                }
                const double d = value.to_number<double>();
                if (declared.has_range && (d < declared.min || d > declared.max)) {
                    std::ostringstream os;
                    os << "值 " << d << " 超出范围 [" << declared.min << ", " << declared.max
                       << "]";
                    return ValidationError{Layer::Range, path, os.str()};
                }
                break;
            }
            case ArgType::String:
            case ArgType::Ref: {
                if (!value.is_string()) {
                    return ValidationError{Layer::Type, path,
                                           std::string("期望 string，收到 ") +
                                               kind_name(value.kind())};
                }
                if (declared.type == ArgType::Ref) {
                    const std::string ref = std_str(value.as_string());
                    if (ref.empty() || ref.front() == '.') {
                        return ValidationError{Layer::Reference, path,
                                               "引用格式非法: " + ref +
                                                   "（应为 <object_id>[.<elem>]）"};
                    }
                }
                break;
            }
            case ArgType::Bool: {
                if (!value.is_bool()) {
                    return ValidationError{Layer::Type, path,
                                           std::string("期望 bool，收到 ") +
                                               kind_name(value.kind())};
                }
                break;
            }
        }
    }

    for (const auto& [k, _] : args) {
        if (find_arg(*cmd, k) == nullptr) {
            std::vector<std::string> allowed;
            for (const auto& a : cmd->args) {
                allowed.push_back(a.name);
            }
            return ValidationError{Layer::Structure, "args." + std::string(k),
                                   "组件库中不存在参数 " + std::string(k) +
                                       "；可用参数: " + join(allowed)};
        }
    }

    return Instruction{id, op, args};
}

std::optional<ValidationError> check_preconditions(const Instruction& ins,
                                                   const ComponentLibrary& library,
                                                   const ModelContext& ctx) {
    const Command* cmd = library.find(ins.op);
    if (cmd == nullptr) {
        return ValidationError{Layer::Structure, "op", "未知指令: " + ins.op};
    }
    for (const auto& pre : cmd->preconditions) {
        if (pre == "canvas_exists" && !ctx.has_kind("canvas")) {
            return ValidationError{Layer::Order, "op",
                                   ins.op + " 需要先创建画布，当前模型为空"};
        }
    }
    for (const auto& declared : cmd->args) {
        if (declared.type != ArgType::Ref || !ins.args.contains(declared.name)) {
            continue;
        }
        const std::string ref = std_str(ins.args.at(declared.name).as_string());
        const std::string object_id = ref.substr(0, ref.find('.'));
        if (!ctx.has_object(object_id)) {
            return ValidationError{Layer::Reference, "args." + declared.name,
                                   "引用的对象 " + object_id + " 不存在"};
        }
    }
    return std::nullopt;
}

// ============================================================ 自定义组件

void CompositeRegistry::add(CompositeComponent c) { components_[c.name] = std::move(c); }

const CompositeComponent* CompositeRegistry::find(std::string_view name) const {
    auto it = components_.find(name);
    return it == components_.end() ? nullptr : &it->second;
}

std::vector<std::string> CompositeRegistry::names() const {
    std::vector<std::string> out;
    for (const auto& [k, _] : components_) {
        out.push_back(k);
    }
    return out;
}

// ============================================================ 执行器

void SvgExecutor::begin(double width, double height, const std::string& bg) {
    w_ = width;
    h_ = height;
    bg_ = bg;
    open_ = true;
}

void SvgExecutor::emit(const std::string& line) {
    body_ += "  " + line + "\n";
    ++elements_;
}

bool SvgExecutor::execute(const Instruction& ins) {
    const auto& a = ins.args;

    if (ins.op == "create_canvas") {
        std::string bg = "#ffffff";
        if (a.contains("bg")) {
            bg = std_str(a.at("bg").as_string());
        }
        begin(a.at("width").to_number<double>(), a.at("height").to_number<double>(), bg);
        ++elements_;
        return true;
    }
    if (!open_) {
        return false;
    }
    if (ins.op == "create_circle") {
        std::ostringstream os;
        os << "<circle cx=\"" << num(a.at("cx").to_number<double>()) << "\" cy=\""
           << num(a.at("cy").to_number<double>()) << "\" r=\"" << num(a.at("r").to_number<double>())
           << "\" fill=\""
           << (a.contains("fill") ? std_str(a.at("fill").as_string()) : "none") << "\" stroke=\""
           << (a.contains("stroke") ? std_str(a.at("stroke").as_string()) : "#1f3b66")
           << "\" stroke-width=\""
           << (a.contains("width") ? a.at("width").to_number<double>() : 2.0) << "\"/>";
        emit(os.str());
        return true;
    }
    if (ins.op == "create_line") {
        std::ostringstream os;
        os << "<line x1=\"" << num(a.at("x1").to_number<double>()) << "\" y1=\""
           << num(a.at("y1").to_number<double>()) << "\" x2=\""
           << num(a.at("x2").to_number<double>()) << "\" y2=\""
           << num(a.at("y2").to_number<double>()) << "\" stroke=\""
           << (a.contains("stroke") ? std_str(a.at("stroke").as_string()) : "#1f3b66")
           << "\" stroke-width=\""
           << (a.contains("width") ? a.at("width").to_number<double>() : 2.0)
           << "\" stroke-linecap=\"round\"/>";
        emit(os.str());
        return true;
    }
    if (ins.op == "create_text") {
        std::ostringstream os;
        os << "<text x=\"" << num(a.at("x").to_number<double>()) << "\" y=\""
           << num(a.at("y").to_number<double>()) << "\" font-size=\""
           << num(a.contains("size") ? a.at("size").to_number<double>() : 20.0)
           << "\" text-anchor=\"middle\" dominant-baseline=\"central\" "
              "font-family=\"sans-serif\">"
           << std_str(a.at("text").as_string()) << "</text>";
        emit(os.str());
        return true;
    }
    if (ins.op == "create_hand") {
        const double angle = a.at("angle").to_number<double>();
        const double len = a.at("length").to_number<double>();
        const double rad = (angle - 90.0) * kPi / 180.0;
        const double cx = w_ / 2.0;
        const double cy = h_ / 2.0;
        std::ostringstream os;
        os << "<line x1=\"" << num(cx) << "\" y1=\"" << num(cy) << "\" x2=\""
           << num(cx + len * std::cos(rad)) << "\" y2=\"" << num(cy + len * std::sin(rad))
           << "\" stroke=\""
           << (a.contains("stroke") ? std_str(a.at("stroke").as_string()) : "#1f3b66")
           << "\" stroke-width=\""
           << num(a.contains("width") ? a.at("width").to_number<double>() : 6.0)
           << "\" stroke-linecap=\"round\"/>";
        emit(os.str());
        return true;
    }
    return false;
}

std::string SvgExecutor::document() const {
    std::ostringstream os;
    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    os << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << num(w_) << "\" height=\""
       << num(h_) << "\" viewBox=\"0 0 " << num(w_) << " " << num(h_) << "\">\n";
    os << "  <rect width=\"100%\" height=\"100%\" fill=\"" << bg_ << "\"/>\n";
    os << body_;
    os << "</svg>\n";
    return os.str();
}

// ============================================================ 内置组件库

ComponentLibrary make_display_library() {
    ComponentLibrary lib;

    Command canvas;
    canvas.name = "create_canvas";
    canvas.category = "canvas";
    canvas.summary = "创建画布，必须是第一条指令";
    canvas.args = {
        {"width", ArgType::Number, true, 1, 4096, true, ""},
        {"height", ArgType::Number, true, 1, 4096, true, ""},
        {"bg", ArgType::String, false, 0, 0, false, ""},
    };
    lib.add(canvas);

    Command circle;
    circle.name = "create_circle";
    circle.category = "shape";
    circle.summary = "画圆";
    circle.args = {
        {"cx", ArgType::Number, true, -4096, 4096, true, ""},
        {"cy", ArgType::Number, true, -4096, 4096, true, ""},
        {"r", ArgType::Number, true, 0.1, 4096, true, ""},
        {"stroke", ArgType::String, false, 0, 0, false, ""},
        {"fill", ArgType::String, false, 0, 0, false, ""},
        {"width", ArgType::Number, false, 0.1, 64, true, ""},
    };
    circle.preconditions = {"canvas_exists"};
    lib.add(circle);

    Command line;
    line.name = "create_line";
    line.category = "shape";
    line.summary = "画线段";
    line.args = {
        {"x1", ArgType::Number, true, -4096, 4096, true, ""},
        {"y1", ArgType::Number, true, -4096, 4096, true, ""},
        {"x2", ArgType::Number, true, -4096, 4096, true, ""},
        {"y2", ArgType::Number, true, -4096, 4096, true, ""},
        {"width", ArgType::Number, false, 0.1, 64, true, ""},
        {"stroke", ArgType::String, false, 0, 0, false, ""},
    };
    line.preconditions = {"canvas_exists"};
    lib.add(line);

    Command text;
    text.name = "create_text";
    text.category = "shape";
    text.summary = "写字";
    text.args = {
        {"x", ArgType::Number, true, -4096, 4096, true, ""},
        {"y", ArgType::Number, true, -4096, 4096, true, ""},
        {"text", ArgType::String, true, 0, 0, false, ""},
        {"size", ArgType::Number, false, 1, 256, true, ""},
    };
    text.preconditions = {"canvas_exists"};
    lib.add(text);

    Command hand;
    hand.name = "create_hand";
    hand.category = "shape";
    hand.summary = "画指针；angle 以 12 点方向为 0，顺时针，单位度；从画布中心出发";
    hand.args = {
        {"angle", ArgType::Number, true, 0, 360, true, ""},
        {"length", ArgType::Number, true, 0.1, 4096, true, ""},
        {"width", ArgType::Number, false, 0.1, 64, true, ""},
        {"stroke", ArgType::String, false, 0, 0, false, ""},
    };
    hand.preconditions = {"canvas_exists"};
    lib.add(hand);

    return lib;
}

CompositeComponent make_gear_component() {
    CompositeComponent gear;
    gear.name = "create_gear";
    gear.summary = "齿轮：外圆 + 内圆 + N 条辐条（组合件）";
    gear.params = {
        {"cx", ArgType::Number, true, -4096, 4096, true, ""},
        {"cy", ArgType::Number, true, -4096, 4096, true, ""},
        {"r", ArgType::Number, true, 1, 4096, true, ""},
        {"teeth", ArgType::Number, true, 3, 24, true, ""},
    };
    gear.expand = [](const boost::json::object& a) -> Expansion {
        const double cx = a.at("cx").to_number<double>();
        const double cy = a.at("cy").to_number<double>();
        const double r = a.at("r").to_number<double>();
        const int teeth = static_cast<int>(a.at("teeth").to_number<double>());

        Expansion out;
        out.push_back(make_instruction(
            "create_circle",
            {{"cx", cx}, {"cy", cy}, {"r", r}, {"stroke", "#1f3b66"}, {"fill", "none"}}));
        out.push_back(make_instruction(
            "create_circle",
            {{"cx", cx}, {"cy", cy}, {"r", r * 0.35}, {"stroke", "#1f3b66"}, {"fill", "none"}}));
        for (int i = 0; i < teeth; ++i) {
            const double angle = static_cast<double>(i) * (2.0 * kPi / teeth);
            out.push_back(make_instruction(
                "create_line",
                {{"x1", cx + r * 0.35 * std::cos(angle)},
                 {"y1", cy + r * 0.35 * std::sin(angle)},
                 {"x2", cx + r * std::cos(angle)},
                 {"y2", cy + r * std::sin(angle)},
                 {"width", 4},
                 {"stroke", "#1f3b66"}}));
        }
        return out;
    };
    return gear;
}

// ============================================================ 管道

PipelineStats run_instructions(const std::string& json_text, const ComponentLibrary& lib,
                               const CompositeRegistry& composites, ModelContext& ctx,
                               SvgExecutor& out) {
    PipelineStats stats;

    auto parsed = parse(json_text);
    if (auto* e = std::get_if<ValidationError>(&parsed)) {
        stats.errors.push_back(e->describe());
        stats.rejected = 1;
        return stats;
    }
    const auto& doc = std::get<boost::json::value>(parsed);
    if (!doc.is_array()) {
        stats.errors.push_back("[结构] $: 指令文件必须是 JSON 数组");
        stats.rejected = 1;
        return stats;
    }

    for (const auto& item : doc.as_array()) {
        // 1) 当基础指令
        auto result = validate(item, lib);
        if (auto* ins = std::get_if<Instruction>(&result)) {
            if (auto e = check_preconditions(*ins, lib, ctx)) {
                stats.errors.push_back(e->describe());
                ++stats.rejected;
                continue;
            }
            if (!out.execute(*ins)) {
                stats.errors.push_back("[执行] 执行器不认识 " + ins->op);
                ++stats.rejected;
                continue;
            }
            if (ins->op == "create_canvas") {
                ctx.add_kind("canvas");
            }
            if (!ins->id.empty()) {
                ctx.add_object(ins->id);
            }
            ++stats.applied;
            continue;
        }

        // 2) 当组合件
        const auto& obj = item.as_object();
        const std::string op = std_str(obj.at("op").as_string());
        const CompositeComponent* comp = composites.find(op);
        if (comp == nullptr) {
            const auto* err = std::get_if<ValidationError>(&result);
            stats.errors.push_back(err->describe());
            ++stats.rejected;
            continue;
        }

        // 复用校验：临时把组合件当作一条命令
        ComponentLibrary probe;
        Command wrapper;
        wrapper.name = op;
        wrapper.args = comp->params;
        probe.add(wrapper);

        auto probe_result = validate(item, probe);
        if (auto* err = std::get_if<ValidationError>(&probe_result)) {
            stats.errors.push_back("组合件参数不合法: " + err->describe());
            ++stats.rejected;
            continue;
        }

        const auto& args = obj.at("args").as_object();
        const Expansion expansion = comp->expand(args);
        bool failed = false;
        for (const auto& sub : expansion) {
            if (!out.execute(sub)) {
                stats.errors.push_back("[执行] 展开出的 " + sub.op + " 失败");
                ++stats.rejected;
                failed = true;
                break;
            }
        }
        if (!failed) {
            ++stats.applied;
        }
    }

    return stats;
}

}  // namespace tb
