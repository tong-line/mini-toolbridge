// 有界队列反压、worker 管道、arena intern。

#include "tb/arena.hpp"
#include "tb/pipeline.hpp"
#include "tb/queue.hpp"
#include "tb/toolbridge.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

}  // namespace

int main() {
    // 1) arena intern：view 指向块内，多次分配后仍可读
    {
        tb::Arena arena(64);
        std::vector<std::string_view> views;
        for (int i = 0; i < 50; ++i) {
            const std::string s = "id_" + std::to_string(i);
            views.push_back(arena.intern(s));
        }
        bool ok = true;
        for (int i = 0; i < 50; ++i) {
            if (views[static_cast<std::size_t>(i)] != "id_" + std::to_string(i)) {
                ok = false;
                break;
            }
        }
        check(ok && arena.block_count() >= 1 && arena.used() > 0, "arena intern 50 个 id 仍可读");
        arena.reset();
        check(arena.used() == 0, "arena reset 后 used=0");
        const auto v = arena.intern("again");
        check(v == "again", "arena reset 后可再 intern");
    }

    // 2) ModelContext：哈希查找 + intern，拷贝后独立
    {
        tb::ModelContext ctx;
        ctx.add_object("c1");
        ctx.add_object("c1");  // 去重
        ctx.add_kind("canvas");
        check(ctx.object_count() == 1 && ctx.has_object("c1") && ctx.has_kind("canvas"),
              "ModelContext intern + 去重");
        check(!ctx.has_object("ghost"), "不存在的 id 查不到");
        tb::ModelContext copy = ctx;
        copy.add_object("c2");
        check(copy.has_object("c2") && !ctx.has_object("c2"), "拷贝后 intern 到独立 arena");
    }

    // 3) 有界队列反压：容量 2，第三个 push 阻塞直到 pop
    {
        tb::BoundedQueue<int> q(2);
        std::atomic<bool> third_started{false};
        std::atomic<bool> third_done{false};
        std::thread prod([&] {
            q.push(1);
            q.push(2);
            third_started.store(true, std::memory_order_release);
            q.push(3);
            third_done.store(true, std::memory_order_release);
        });
        while (!third_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(30ms);
        check(!third_done.load(std::memory_order_acquire) && q.size() == 2,
              "队列满时第三个 push 被反压");
        check(q.pop().value() == 1, "pop 得到 1");
        prod.join();
        check(third_done.load(std::memory_order_acquire), "pop 后第三个 push 完成");
        q.close();
        check(q.pop().value() == 2 && q.pop().value() == 3, "剩余元素按序弹出");
        check(!q.pop().has_value(), "close 且排空后 pop 返回 nullopt");
    }

    // 4) close 能唤醒阻塞的 push
    {
        tb::BoundedQueue<int> q(1);
        q.push(1);
        std::atomic<bool> returned{false};
        std::thread prod([&] {
            const bool ok = q.push(2);
            check(!ok, "close 后阻塞的 push 返回 false");
            returned.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(20ms);
        q.close();
        prod.join();
        check(returned.load(std::memory_order_acquire), "close 唤醒了阻塞的生产者");
    }

    tb::ComponentLibrary lib = tb::make_display_library();
    tb::CompositeRegistry composites;
    composites.add(tb::make_gear_component());

    // 5) 管道正常路径
    {
        tb::ModelContext ctx;
        tb::SvgExecutor svg;
        std::atomic<int> applied_cb{0};
        tb::InstructionPipeline::Config cfg;
        cfg.queue_capacity = 2;
        cfg.on_applied = [&](std::string_view) { applied_cb.fetch_add(1); };
        tb::InstructionPipeline pipe(lib, composites, ctx, svg, cfg);
        check(pipe.submit(R"({"op":"create_canvas","args":{"width":200,"height":200}})"),
              "submit canvas");
        check(pipe.submit(R"({"op":"create_circle","args":{"cx":100,"cy":100,"r":40}})"),
              "submit circle");
        const auto stats = pipe.finish();
        check(stats.applied == 2 && stats.rejected == 0, "管道执行 2 条");
        check(applied_cb.load() == 2, "on_applied 回调 2 次");
        check(svg.element_count() >= 2, "SVG 有元素");
    }

    // 6) 非法指令：worker 失败，后续 submit 返回 false
    {
        tb::ModelContext ctx;
        tb::SvgExecutor svg;
        tb::InstructionPipeline::Config cfg;
        cfg.queue_capacity = 4;
        tb::InstructionPipeline pipe(lib, composites, ctx, svg, cfg);
        check(pipe.submit(R"({"op":"create_canvas","args":{"width":200,"height":200}})"),
              "submit canvas (abort 路径)");
        check(pipe.submit(R"({"op":"melt","args":{}})"), "非法指令仍能入队");
        // worker 处理 melt 后 close。自旋等到 failed，再确认后续 submit 失败。
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!pipe.failed() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        check(pipe.failed(), "worker 在非法指令后置 failed");
        check(!pipe.submit(R"({"op":"create_circle","args":{"cx":1,"cy":1,"r":1}})"),
              "失败后 submit 返回 false");
        const auto stats = pipe.finish();
        check(stats.rejected >= 1, "至少拒绝 1 条");
    }

    if (g_failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("FAIL: %d 项\n", g_failures);
    return 1;
}
