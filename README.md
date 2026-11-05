# mini-toolbridge

[![M8ven Score](https://m8ven.ai/badge/mcp/tong-line-mini-toolbridge-153nfr?v=2fe5ce4d29747455cb354c2598e9b7b6)](https://m8ven.ai/mcp/tong-line-mini-toolbridge-153nfr?s=readme)

**自然语言 → 受约束的 JSON 指令 → 七层校验 → 建模产物。**

大模型驱动工业软件时，真正难的不是"让模型说话"，是"让模型的输出可靠地变成软件能执行的操作"。
这个项目把那层桥接做出来，并且可以换任意 OpenAI 兼容的大模型。

> 设计来源于一段航电建模软件的实习工作：当时的链路是「自然语言 → 大模型 → JSON → 建模软件接口」。
> 本仓库是**独立实现的设计还原**，不含任何单位代码，不涉及具体软件与项目信息。

---

## 它解决什么问题

让大模型直接产出**最终的建模产物 XML**，会崩在三层：

| 失败层 | 后果 |
|---|---|
| 结构 | 元素名/属性名写错，产物加载即失败 |
| 语义 | 缺必填属性、数值越界，加载阶段才暴露 |
| 时序 | 引用了还不存在的对象，错误在中途出现且难定位 |

**共同痛点：错误在「加载阶段」才暴露，且无法回溯到是哪一句自然语言导致的。**

本项目的做法：**不让模型碰产物，只让它生成受约束的指令。**

```
自然语言 ──► 大模型 ──► JSON 指令 ──► 七层校验 ──► 执行器 ──► 产物
                                     └─ 每一步都可校验、可定位、可重试
```

核心判断：

> **XML 是产物，不是接口。** 让模型直接写产物，等于绕过软件的全部约束去拼机器码。

---

## 快速开始

```bash
# 依赖：Boost.JSON
# Ubuntu / Debian
sudo apt install -y libboost-json-dev
# macOS
brew install boost openssl

cmake -S . -B build
cmake --build build

# sanitizer（Debug 建议开）
# cmake -S . -B build-asan -DTB_SANITIZE=ON
# cmake -S . -B build-tsan -DTB_SANITIZE_THREAD=ON

# 1. 看组件库（这也是喂给模型的 system prompt）
./build/tb library

# 2. 校验一个指令文件
./build/tb validate examples/clock.json

# 3. 渲染成 SVG，浏览器打开
./build/tb render examples/clock.json -o clock.svg

# 4. 自定义组件（齿轮 = 2 圆 + N 条辐条）
./build/tb render examples/gear.json -o gear.svg

# 5. 引用链：create_connect 连接两个带 id 的圆（引用层校验）
./build/tb render examples/refs.json -o refs.svg

# 6. 走真实大模型（需要环境变量）
export TB_API_BASE=https://api.deepseek.com/v1
export TB_API_KEY=sk-xxxx
export TB_MODEL=deepseek-chat
./build/tb ask "画一个钟表，指针指向 10 点 10 分" -o clock.svg

# 7. 没有 key 时离线跑通链路
./build/tb ask "画一个钟表" --offline -o clock.svg

# 8. 本地 mock SSE 服务 + 流式（不需要任何 API key）
./build/tb mock-llm --port 8081 &
./build/tb ask "画一个钟表" --stream \
    --url http://127.0.0.1:8081/v1/chat/completions -o clock.svg

# 9. 演示「流式校验提前中止」：第 4 条是非法指令
./build/tb mock-llm --port 8082 --bad &
./build/tb ask "画一个钟表" --stream \
    --url http://127.0.0.1:8082/v1/chat/completions -o partial.svg
```

---

## 七层校验

| 层 | 检查 | 失败示例 |
|---|---|---|
| 语法 | JSON 是否合法 | `incomplete JSON` |
| 结构 | `op` 是否在组件库、参数名是否存在 | `未知指令: melt；可用指令: create_circle, ...` |
| 类型 | 参数类型 | `args.r: 期望 number，收到 string` |
| 必填 | 必填参数齐全 | `args.length: 缺少必填参数 length` |
| 范围 | 数值区间 | `args.width: 值 99 超出范围 [0.1, 64]` |
| 引用 | 引用的对象是否存在 | `args.from: 引用的对象 ghost 不存在` |
| 顺序 | 前置条件 | `create_hand 需要先创建画布，当前模型为空` |

每一层失败都返回**带 JSON 路径**的错误，而不是"加载失败"。

---

## 组件库：把模型的输出空间收窄

模型的自由度不是靠提示词"请不要乱写"约束的，是靠**把可选集合做成有限集**：

```cpp
Command hand;
hand.name = "create_hand";
hand.summary = "画指针；angle 以 12 点方向为 0，顺时针，单位度；从画布中心出发";
hand.args = {
    {"angle",  ArgType::Number, true, 0, 360, true, ""},   // 必填，0~360
    {"length", ArgType::Number, true, 0.1, 4096, true, ""},
    {"width",  ArgType::Number, false, 0.1, 64, true, ""},  // 可选，有范围
    {"stroke", ArgType::String, false, 0, 0, false, ""},
};
hand.preconditions = {"canvas_exists"};
```

- 模型只能在 `create_hand` 的四个参数里选
- `angle` 只给角度，**端点由执行器算** —— 语义指令与几何计算分离
- `create_connect` 的 `from`/`to` 是引用参数：对象要先带 `id` 创建，引用层拦截悬空引用（见 `examples/refs.json`）
- 组件库同时被渲染成 system prompt（见 `tb library`），模型看到的就是这份 schema

---

## 自定义组件：有限集合 + 可扩展

矛盾点：组件库要**有限**才能约束模型，但软件支持用户自定义组件，集合是**动态增长**的。

解法：**自定义组件 = 已有组件的参数化组合**，不是黑盒代码。

```
create_gear(cx, cy, r, teeth)
   └─ 展开为 2 个 create_circle + teeth 条 create_line
```

**合法性可以递归证明**：组合件展开成基础指令 → 基础指令逐条通过校验 → 组合件合法。

约束没被破坏，扩展性也拿到了。见 `make_gear_component()` 与 `examples/gear.json`。

---

## MCP 服务端（兼容 Claude Desktop 等客户端）

```bash
./build/tb mcp        # 启动 stdio 服务端
```

三个工具直接映射组件库：

| 工具 | 作用 |
|---|---|
| `list_components` | 返回全部指令与参数 schema |
| `validate_instructions` | 只校验，不产生产物 |
| `render_instructions` | 校验 + 执行 + 写出 SVG |

接入 Claude Desktop，编辑 `claude_desktop_config.json`（见 `examples/claude_desktop_config.json`）：

```json
{
  "mcpServers": {
    "mini-toolbridge": {
      "command": "/绝对路径/mini-toolbridge/build/tb",
      "args": ["mcp"]
    }
  }
}
```

重启客户端后即可让它「画一个 8 齿齿轮」。

**协议实现要点**：

- 传输是**换行分隔的 JSON-RPC 2.0**（不是 LSP 的 Content-Length 分帧）
- `stdout` 只写协议消息，日志一律走 `stderr`（否则客户端解析失败）
- 通知（无 `id`）不回复
- `initialize` 回显客户端请求的 `protocolVersion`，客户端不支持时自行降级
- 工具执行失败返回 `isError: true` 的**结果**，而不是 JSON-RPC error（协议约定）

## 流式链路（这块是技术重点）

LLM 是流式返回的。等完整回复再解析有两个浪费：首字节延迟 = 全量延迟；模型生成了 2000 token 废话时，你要等完才发现前几个字就错了。

本项目的做法：**边收边校验边执行**。

```
HTTP chunk ──► SseParser ──► delta 文本 ──► InstructionScanner ──► BoundedQueue ──► worker 线程
                                              （跨 chunk 拼装，                 校验 + 执行
                                               compact 丢掉已吐出文本）
```

队列容量有上限：满了扫描侧阻塞（反压），校验失败则 close 队列，扫描侧 `submit` 返回 false，连接断开。

实测（示例环境：mock 流，53 个 token，每个间隔 60ms，数值随机器浮动）：

| 指标 | 数值 |
|---|---|
| 首条指令可用 | **259 ms** |
| 全部完成 | **1352 ms** |
| 非法指令时的中止时间 | **838 ms**（省掉 38% 的等待） |

三个实现要点：

1. **`SseParser`**：按行累积，空行触发事件。任意字节位置切包都能正确还原（自测用 7 字节一刀验证）。
2. **`InstructionScanner`**：只跟踪 `{}` 配对与字符串转义，**忽略 `[]`**，所以一个对象被切成三段也能拼回；每凑齐一个对象立刻交给下游。已吐出的前缀会被 compact 掉，缓冲区不随流长无限涨。
3. **有界队列 + worker**：扫描（asio 回调）只入队；`mutex` + `condition_variable` 做反压。校验失败时 worker 关闭队列，生产者被唤醒后断开连接。
4. **组合件两阶段**：展开后的每条子指令走同一套 `validate_instruction` + 前置条件；先在 `ModelContext` 副本上证明全部合法，再执行。失败不改产物。

## 接入大模型

`include/tb/toolbridge.hpp` 里的 `ILlmProvider` 是唯一接口：

```cpp
class ILlmProvider {
public:
    virtual ~ILlmProvider() = default;
    virtual LlmReply complete(const std::string& system_prompt,
                              const std::string& user_prompt) = 0;
};
```

| 实现 | 用途 |
|---|---|
| `make_scripted_provider(text)` | 离线调试，固定回复，不联网 |
| `make_http_provider(cfg)` | 走 boost::beast 调任意 OpenAI 兼容端点（带超时） |

**换模型只改环境变量，代码一行不动**：

```bash
# DeepSeek
TB_API_BASE=https://api.deepseek.com/v1  TB_MODEL=deepseek-chat
# 通义千问（DashScope 兼容模式）
TB_API_BASE=https://dashscope.aliyuncs.com/compatible-mode/v1  TB_MODEL=qwen-plus
# 本地 Ollama
TB_API_BASE=http://localhost:11434/v1  TB_MODEL=qwen2.5-coder
```

> 密钥只进请求头，不出现在命令行参数或日志里。

---

## 目录

```
mini-toolbridge/
├── include/tb/toolbridge.hpp   组件库 / 校验 / 组合件 / 执行器 / LLM 接口
├── include/tb/arena.hpp        块式 bump arena（字符串 intern）
├── include/tb/queue.hpp        有界阻塞队列
├── include/tb/pipeline.hpp     扫描线程 → worker 的指令管道
├── include/tb/stream.hpp       SSE 解析 + 增量指令提取
├── include/tb/net.hpp          异步流式 POST + 本地 mock 服务
├── src/toolbridge.cpp          七层校验 / 组合件两阶段 / SVG 执行器 / 管道
├── src/pipeline.cpp            InstructionPipeline worker
├── src/stream.cpp              SseParser / InstructionScanner（含 compact）
├── src/net_stream.cpp          协程 HTTP 客户端（http/https）+ mock LLM
├── src/llm.cpp                 ILlmProvider + prompt 渲染 + 回复解析
├── include/tb/mcp.hpp          MCP 服务端接口
├── src/mcp_server.cpp          JSON-RPC 分发 + 三个工具
├── apps/tb.cpp                 CLI：library / validate / render / ask / mock-llm / mcp
├── tests/stream_selftest.cpp   SSE 切包 + compact 有界
├── tests/mcp_selftest.cpp      MCP 协议层自测（8 组断言）
├── tests/validate_selftest.cpp 七层校验 + 组合件两阶段
├── tests/pipeline_selftest.cpp 反压 / close 唤醒 / worker 失败
├── examples/                   clock.json / gear.json / refs.json / bad.json
└── docs/design.md              设计还原与面试问答
```

---

## 边界与改进方向

| 项 | 现状 |
|---|---|
| 组件库规模 | 6 条基础指令 + 1 个组合件（原场景 700+，**结构同构、规模不同**） |
| 执行器 | SVG 后端；真实系统的执行器是建模软件的接口 |
| TLS | 已支持 https，但未做证书自定义与代理 |
| 错误恢复 | 流式下可提前中止；未实现「把错误喂回模型自修正」 |
| 并发 | 单请求内：扫描线程 + worker，有界队列反压；未做多请求限流 |
| 超时 | 网络层支持可选超时（到点取消底层 socket，协程栈正常展开）；同步 ask 已启用 |
| 内存 | `ModelContext` 用 arena intern + 哈希表；scanner 会 compact。未做指令对象池 |
| 性能 | 未对 700 条组件库做过基准；查表已是 `map` / `unordered_set`，不再线性扫对象 id |

---

## 为什么执行器可以换

链路里唯一与具体软件耦合的是 `SvgExecutor::execute()`：

```cpp
if (ins.op == "create_circle") { ... }
```

换成 FreeCAD / Unity / 或自研建模软件，只改这一个类，**组件库、校验、LLM 接入、CLI 全都不用动**。
这就是分层的意义 —— 也是这个项目能声称"设计可迁移"的依据。
