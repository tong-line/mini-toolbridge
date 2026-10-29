# mini-toolbridge

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
sudo apt install -y libboost-json-dev

cmake -S . -B build
cmake --build build

# 1. 看组件库（这也是喂给模型的 system prompt）
./build/tb library

# 2. 校验一个指令文件
./build/tb validate examples/clock.json

# 3. 渲染成 SVG，浏览器打开
./build/tb render examples/clock.json -o clock.svg

# 4. 自定义组件（齿轮 = 2 圆 + N 条辐条）
./build/tb render examples/gear.json -o gear.svg

# 5. 走真实大模型（需要环境变量）
export TB_API_BASE=https://api.deepseek.com/v1
export TB_API_KEY=sk-xxxx
export TB_MODEL=deepseek-chat
./build/tb ask "画一个钟表，指针指向 10 点 10 分" -o clock.svg

# 6. 没有 key 时离线跑通链路
./build/tb ask "画一个钟表" --offline -o clock.svg
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
| 引用 | 引用的对象是否存在 | `args.edge_ref: 引用的对象 b9 不存在` |
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
| `make_curl_provider(cfg)` | 走 curl 调任意 OpenAI 兼容端点 |

**换模型只改环境变量，代码一行不动**：

```bash
# DeepSeek
TB_API_BASE=https://api.deepseek.com/v1  TB_MODEL=deepseek-chat
# 通义千问（DashScope 兼容模式）
TB_API_BASE=https://dashscope.aliyuncs.com/compatible-mode/v1  TB_MODEL=qwen-plus
# 本地 Ollama
TB_API_BASE=http://localhost:11434/v1  TB_MODEL=qwen2.5-coder
```

> 密钥通过环境变量交给 curl，不出现在命令行参数里（`ps` 看不到）。

---

## 目录

```
mini-toolbridge/
├── include/tb/toolbridge.hpp   全部公开接口
├── src/toolbridge.cpp          组件库 / 七层校验 / 组合件 / SVG 执行器 / 管道
├── src/llm.cpp                 ILlmProvider 两种实现 + prompt 渲染 + 回复解析
├── apps/tb.cpp                 命令行：library / validate / render / ask
├── examples/                   clock.json（钟表）gear.json（齿轮）bad.json（反例）
└── docs/design.md              设计还原与面试问答
```

---

## 边界与改进方向

| 项 | 现状 |
|---|---|
| 组件库规模 | 5 条基础指令 + 1 个组合件（原场景 700+，**结构同构、规模不同**） |
| 执行器 | SVG 后端；真实系统的执行器是建模软件的接口 |
| 错误恢复 | 只报错，未实现「把错误喂回模型自修正」 |
| 性能 | 未测；700 条指令的查表若为线性扫描，长指令序列下会有开销 |

---

## 为什么执行器可以换

链路里唯一与具体软件耦合的是 `SvgExecutor::execute()`：

```cpp
if (ins.op == "create_circle") { ... }
```

换成 VAPS / FreeCAD / Unity，只改这一个类，**组件库、校验、LLM 接入、CLI 全都不用动**。
这就是分层的意义 —— 也是这个项目能声称"设计可迁移"的依据。
