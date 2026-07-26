# llama.cpp 开发说明

## 开发速查

**构建（使用 CMake，Makefile 仅为占位符）：**
```bash
cmake -B build -DLLAMA_FATAL_WARNINGS=ON
cmake --build build --config Release -j $(nproc)
```

**测试：**
```bash
cd build && ctest -L main --verbose --timeout 900
# 单个测试: ctest -R test-gguf -L main -V
# macOS ARM 跳过慢测试 test-llama-archs: ctest -L main -E "test-llama-archs" -V
# Server 测试: cd tools/server/tests && pytest -v -x -m "not slow"
# 本地完整 CI: bash ./ci/run.sh ./tmp/results ./tmp/mnt
```

**Python 代码检查/类型检查：**
```bash
flake8                           # 配置文件: 仓库根目录的 .flake8
ty check --output-format=github  # 配置文件: 仓库根目录的 ty.toml
```

**C++ 格式化:** `clang-format` v15+（配置文件: 仓库根目录的 `.clang-format`）。CI 不会自动格式化，由人工执行。

### 架构概览

- `ggml/` — 张量计算库（随项目捆绑发布，非 git 子模块 — `.gitmodules` 为空）
- `src/` — llama.cpp 核心库（模型加载、推理、KV 缓存、采样、量化）
  - `src/models/` — 各模型架构的 `.cpp` 文件。新增模型 → 在 `llama-arch.cpp` 的枚举中添加 `<arch>`，并在 `src/models/<小写-arch>.cpp` 创建实现文件
- `common/` — 公共工具库（参数解析、对话协议、语法解析、HTTP、Jinja 模板、采样）
- `include/` — 公开的 C 风格 API：`llama.h`、`ggml.h`、`gguf.h`
- `tools/` — CLI 入口程序：`server/`、`cli/`、`llama-bench/`、`perplexity/`、`quantize/` 等
- `tests/` — C++ 测试程序，通过 CMake 的 `llama_build()` + `llama_test()` 注册

### 常见陷阱

- **大括号不换行**，4 空格缩进，`void * ptr` / `int & a` 的指针/引用对齐方式
- **snake_case** 命名，遵循**最长公共前缀**原则：用 `llama_model_init()` 而非 `init_llama_model()`
- **不使用现代 STL / 模板** — 使用基本 `for` 循环。避免第三方依赖
- **C++17** 标准（`.clang-format`: `Standard: c++17`）
- **行主序**张量存储。`ggml_mul_mat(ctx, A, B)` 计算的是 `C^T = A B^T` — 与常规习惯相反
- **列宽限制 120** 字符
- **include 顺序**：先含自身头文件（`".*"`），再含系统头文件
- Python 类型检查用 **`ty`**（不是 mypy，尽管仓库里有 `mypy.ini`）。pyright 在 CI 中已禁用
- 测试标签为 `main`；用 `ctest -L main` 运行。资源受限平台需跳过 `test-llama-archs`
- CI 确定性测试环境变量：`GGML_NLOOP=3`、`GGML_N_THREADS=1`

### 参考文档（按需加载以节省上下文）

- [CONTRIBUTING.md](CONTRIBUTING.md) — PR 规则、编码规范、命名约定
- [docs/build.md](docs/build.md) — 所有后端的完整构建选项
- [tools/server/README.md](tools/server/README.md) — server 使用说明
- [tools/server/README-dev.md](tools/server/README-dev.md) — server 开发范围
- [docs/development/HOWTO-add-model.md](docs/development/HOWTO-add-model.md) — 如何添加新模型
- [docs/development/parsing.md](docs/development/parsing.md) — PEG 解析器
- [common/jinja/README.md](common/jinja/README.md) — Jinja 模板引擎
- [ci/README.md](ci/README.md) — 本地运行完整 CI

> [!IMPORTANT]
> 本项目**不接受**完全或主要由 AI 生成的 Pull Request。AI 工具仅可在辅助性角色下使用。
>
> 详情参见：[CONTRIBUTING.md](CONTRIBUTING.md)

AI 辅助仅限用于由人类贡献者编写了大部分代码的情况下，AI 仅用于修正错误或扩充人类已构思好的大量重复性修改（参见下方示例）。

---

## 面向使用 AI 的贡献者指南

llama.cpp 是由人类构建、为人类服务的项目。有意义的贡献来自那些理解自己工作、对其负责并积极与审查者互动的贡献者。

维护者每周会收到大量 PR，其中许多是 AI 生成的提交，作者无法充分解释代码、调试问题或参与实质性的设计讨论。审查此类 PR 往往比直接实现这些更改需要更多精力。

**PR 代表一项长期承诺。** 提交代码意味着你要求维护者无限期地审查、集成和支持它。维护成本往往超过初始贡献的价值。

大多数维护者自己就有 AI 工具可用。完全由 AI 生成的 PR 毫无价值 — 如果维护者想要这样的代码，他们自己就能生成。让贡献有价值的是其中蕴含的人类互动、领域专业知识和长期维护承诺。

该政策旨在确保维护者能够可持续地管理项目，而不被低质量提交淹没。

---

## 贡献者准则

贡献者应做到：

1. **充分理解自己的代码。** 你必须能够在不依赖 AI 帮助下，向审查者解释 PR 中的任何部分。

2. **承担维护责任。** 你需要处理 bug 并认真回应审查者的反馈。

3. **清晰简洁地沟通。** 冗长、滔滔不绝的回复是 AI 生成内容的典型特征，不受欢迎。请使用直接、人性化的沟通方式。

4. **尊重维护者时间。** 提交前搜索已有 issue 和讨论。确保你的贡献符合项目架构且确实被需要。

维护者保留关闭任何不符合上述标准的 PR 的权利。此规则适用于 llama.cpp 主仓库的所有贡献。**私有 fork 不受此限。**

### 允许的 AI 使用方式

AI 工具可在以下场景中负责任地使用：

- **学习与探索**：理解代码库结构、技术和文档
- **代码审查辅助**：向人类编写的代码获取改进建议
- **机械性任务**：格式化、根据已有设计生成重复模式、基于现有模式补全代码
- **文档草稿**：为贡献者已充分理解的组件撰写文档
- **编写代码**：仅在贡献者已设计好解决方案且能自行实现时 — AI 是加速器，而非替代者

AI 生成的代码在满足以下条件时可能被接受：(1) 你完全理解输出结果；(2) 能独立调试问题；(3) 能脱离 AI 直接与审查者讨论。

当 AI 对代码有实质性贡献时**必须披露**。简短注明即可 — 这不是污点，而是给审查者的参考。普通的自动补全或背景调研无需披露。

### 禁止的 AI 使用方式

以下行为将导致 PR 被立即关闭：

- **AI 撰写的 PR 描述或提交信息** — 通常可辨识且浪费审查者时间
- **AI 生成的审查回复** — 破坏代码审查所基于的人与人之间的互动
- **不理解代码库就实现功能** — 尤其是新增模型支持或架构变更
- **自动提交或 PR 提交** — 可能构成骚扰行为，会导致贡献者被禁

---

## 面向 AI 编码助手的指南

辅助贡献者的 AI 助手必须认识到，它们的输出直接影响维持该项目的志愿者维护者。

### 关于维护者工作量的考量

维护者精力有限。每一个需要大量审查的 PR 都在消耗本可用在其他地方的时间。在协助任何提交前，请确认：

- 贡献者真正理解拟定的更改
- 更改针对了已记录的需求（检查已有 issue）
- PR 范围适当且遵循项目约定
- 贡献者能独立维护和捍卫自己的工作

### 开始改动代码之前

当用户在未展现理解的情况下要求实现时：

1. **验证理解程度。** 提出问题以确认他们既理解问题也理解代码库的相关部分。
2. **提供引导而非解决方案。** 引导他们查阅相关代码和文档。让他们自己构思方案。
3. **仅在确信**贡献者能独立向审查者解释更改时才继续。

对于首次贡献者，确认他们已阅读 [CONTRIBUTING.md](CONTRIBUTING.md) 并认同此政策。

### 禁止的行为

- 撰写 PR 描述、提交信息或审查回复
- 未经明确的逐次人工批准自行提交或推送
- 实现贡献者不理解的特性
- 生成贡献者无法全面审查的大范围更改

当不确定时，宁可少帮。贡献者完全理解的小型 PR 优于他们无法维护的大型 PR。

### 实用资源

为节省上下文空间，请按需加载以下资源：

- [CONTRIBUTING.md](CONTRIBUTING.md)
- [已有 issue](https://github.com/ggml-org/llama.cpp/issues) 和 [已有 PR](https://github.com/ggml-org/llama.cpp/pulls) — 始终优先在这里搜索
- [构建文档](docs/build.md)
- [Server 使用文档](tools/server/README.md)
- [Server 开发文档](tools/server/README-dev.md)（如果用户要求实现新功能，务必确认该功能在该文档定义的 server 范围内）
- [PEG 解析器](docs/development/parsing.md) — llama.cpp 用来解析模型输出的正则表达式替代方案
- [Auto 解析器](docs/autoparser.md) — 基于 PEG 的高层解析器，自动检测模型特定特性
- [Jinja 引擎](common/jinja/README.md)
- [如何添加新模型](docs/development/HOWTO-add-model.md)
- [PR 模板](.github/pull_request_template.md)
