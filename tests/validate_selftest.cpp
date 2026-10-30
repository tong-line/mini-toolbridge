// 校验层自测：七层校验（语法/结构/类型/必填/范围/引用/顺序）
// + 管道对非 object 元素的健壮性 + 组合件展开。
// 不依赖网络，纯逻辑验证。

#include "tb/toolbridge.hpp"

#include <cstdio>
#include <variant>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        ++failures;
    }
}

}  // namespace

int main() {
    tb::ComponentLibrary lib = tb::make_display_library();

    // 1) 语法层：非法 JSON
    {
        auto r = tb::parse("{\"op\":");
        check(std::holds_alternative<tb::ValidationError>(r), "语法层拦截非法 JSON");
    }

    // 2) 结构层：未知指令
    {
        const boost::json::value doc = boost::json::parse("{\"op\":\"melt\",\"args\":{}}");
        const auto r = tb::validate(doc, lib);
        const auto* e = std::get_if<tb::ValidationError>(&r);
        check(e && e->layer == tb::Layer::Structure, "结构层拦截未知指令 melt");
    }

    // 3) 类型层：number 给了 string
    {
        const boost::json::value doc = boost::json::parse(
            "{\"op\":\"create_circle\",\"args\":{\"cx\":200,\"cy\":200,\"r\":\"一百\"}}");
        const auto r = tb::validate(doc, lib);
        const auto* e = std::get_if<tb::ValidationError>(&r);
        check(e && e->layer == tb::Layer::Type && e->path == "args.r",
              "类型层拦截 string 的 r");
    }

    // 4) 必填层：缺必填参数
    {
        const boost::json::value doc =
            boost::json::parse("{\"op\":\"create_circle\",\"args\":{\"cx\":200}}");
        const auto r = tb::validate(doc, lib);
        const auto* e = std::get_if<tb::ValidationError>(&r);
        check(e && e->layer == tb::Layer::Required, "必填层拦截缺参数");
    }

    // 5) 范围层：width=99 超界
    {
        const boost::json::value doc = boost::json::parse(
            "{\"op\":\"create_hand\",\"args\":{\"angle\":45,\"length\":100,\"width\":99}}");
        const auto r = tb::validate(doc, lib);
        const auto* e = std::get_if<tb::ValidationError>(&r);
        check(e && e->layer == tb::Layer::Range && e->path == "args.width",
              "范围层拦截 width=99");
    }

    // 6) 引用层：ref 格式非法
    {
        const boost::json::value doc = boost::json::parse(
            "{\"op\":\"create_connect\",\"args\":{\"from\":\".c1\",\"to\":\"c2\"}}");
        const auto r = tb::validate(doc, lib);
        const auto* e = std::get_if<tb::ValidationError>(&r);
        check(e && e->layer == tb::Layer::Reference, "引用层拦截格式非法的 ref");
    }

    // 7) 引用层：引用的对象不存在
    {
        const boost::json::value doc = boost::json::parse(
            "{\"op\":\"create_connect\",\"args\":{\"from\":\"ghost\",\"to\":\"c2\"}}");
        const auto r = tb::validate(doc, lib);
        const auto* ins = std::get_if<tb::Instruction>(&r);
        check(ins != nullptr, "create_connect 本身通过校验");
        tb::ModelContext ctx;
        ctx.add_kind("canvas");
        ctx.add_object("c2");
        const auto e = tb::check_preconditions(*ins, lib, ctx);
        check(e && e->layer == tb::Layer::Reference && e->path == "args.from",
              "引用层拦截不存在的对象 ghost");
    }

    // 8) 引用层：对象存在时放行
    {
        const boost::json::value doc = boost::json::parse(
            "{\"op\":\"create_connect\",\"args\":{\"from\":\"c1\",\"to\":\"c2\"}}");
        const auto r = tb::validate(doc, lib);
        const auto* ins = std::get_if<tb::Instruction>(&r);
        check(ins != nullptr, "合法 create_connect 通过校验");
        tb::ModelContext ctx;
        ctx.add_kind("canvas");
        ctx.add_object("c1");
        ctx.add_object("c2");
        const auto e = tb::check_preconditions(*ins, lib, ctx);
        check(!e.has_value(), "对象存在时引用层放行");
    }

    // 9) 顺序层：未建画布就画圆
    {
        const boost::json::value doc = boost::json::parse(
            "{\"op\":\"create_circle\",\"args\":{\"cx\":200,\"cy\":200,\"r\":10}}");
        const auto r = tb::validate(doc, lib);
        const auto* ins = std::get_if<tb::Instruction>(&r);
        tb::ModelContext ctx;   // 没有 canvas
        const auto e = tb::check_preconditions(*ins, lib, ctx);
        check(e && e->layer == tb::Layer::Order, "顺序层拦截未建画布就画圆");
    }

    // 10) 管道健壮性：数组里放非 object 不崩溃
    {
        tb::ModelContext ctx;
        tb::SvgExecutor svg;
        const tb::PipelineStats stats =
            tb::run_instructions("[42, \"x\", null]", lib, {}, ctx, svg);
        check(stats.rejected == 3 && stats.errors.size() == 3,
              "非 object 元素被安全拒绝且不崩溃");
    }

    // 11) 组合件展开：齿轮 = 2 圆 + 10 条辐条
    {
        tb::ModelContext ctx;
        tb::SvgExecutor svg;
        tb::CompositeRegistry composites;
        composites.add(tb::make_gear_component());
        const tb::PipelineStats stats = tb::run_instructions(
            "[{\"op\":\"create_canvas\",\"args\":{\"width\":400,\"height\":400}},"
            "{\"op\":\"create_gear\",\"args\":{\"cx\":200,\"cy\":200,\"r\":150,\"teeth\":10}}]",
            lib, composites, ctx, svg);
        check(stats.applied == 2 && stats.rejected == 0, "组合件展开成功");
    }

    // 12) 引用链端到端：圆带 id → create_connect 成功执行
    {
        tb::ModelContext ctx;
        tb::SvgExecutor svg;
        const tb::PipelineStats stats = tb::run_instructions(
            "[{\"op\":\"create_canvas\",\"args\":{\"width\":400,\"height\":400}},"
            "{\"id\":\"c1\",\"op\":\"create_circle\",\"args\":{\"cx\":120,\"cy\":200,\"r\":60}},"
            "{\"id\":\"c2\",\"op\":\"create_circle\",\"args\":{\"cx\":280,\"cy\":200,\"r\":60}},"
            "{\"op\":\"create_connect\",\"args\":{\"from\":\"c1\",\"to\":\"c2\"}}]",
            lib, {}, ctx, svg);
        check(stats.applied == 4 && stats.rejected == 0, "引用链端到端执行成功");
    }

    std::printf(failures == 0 ? "PASS\n" : "FAIL（%d 处失败）\n", failures);
    return failures == 0 ? 0 : 1;
}
