# 代码风格


## 文件与包含
| 项目 | 规范 |
|------|------|
| 头文件后缀 | `.hpp`（模板/内联实现可放 `.hpp`） |
| 源文件后缀 | `.cpp` |
| 防重复包含 | 统一 `#pragma once`（现代编译器全支持） |
| 头文件包含顺序 | 1. C 标准库 → 2. C++ 标准库 → 3. 第三方库 → 4. 本项目头文件（组内字母序，组间空一行） |
| 前置声明 | 头文件中优先使用前置声明减少编译依赖（如 `class XxxYyy;`） |


## 命名
| 实体 | 规范 | 示例 |
|------|------|------|
| 类/结构体/枚举类型/类型别名 | `XxxYyy`（大驼峰） | `class UserManager;` |
| 函数/方法（含构造/析构/访问器） | `XxxYyy`（大驼峰） | `void CalculateTotal();`<br>`int GetUserId() const;` |
| 普通变量（局部/参数） | `xxx_xxx`（小写+下划线） | `int user_count;` |
| 类成员变量（所有访问权限） | `xxx_xxx_`（尾随下划线） | `std::string name_;` |
| 静态成员变量 | `s_xxx_xxx_`（前缀 `s_` + 尾随下划线） | `static int s_instance_count_;` |
| 全局变量（强烈避免） | `g_xxx_xxx`（前缀 `g_`） | `int g_debug_mode;` |
| 常量 | 全局/枚举值：`XXX_YYY`<br>类内静态常量：`kXxxYyy` | `constexpr int MAX_RETRY = 3;`<br>`static constexpr int kBufferSize = 1024;` |
| 布尔变量 | `is_xxx` / `has_xxx` / `can_xxx` | `bool is_valid;` |
| 命名空间 | 全小写 | `namespace data_processor;` |
| 模板参数 | `T` 或 `Txxx`（大驼峰） | `template <typename TData>` |
| 宏（避免使用） | `XXX_YYY`（全大写） | `#define LOG_ERROR(msg)` → 改用 `constexpr`/`inline` |


## 代码结构与注释
| 项目 | 规范 |
|------|------|
| 类内成员顺序 | public → protected → private（每块内：类型定义 → 构造/析构 → 成员函数 → 成员变量）|
| 注释规范 | 文件头：版权、作者、功能概述、修改记录（可选） |
| 函数/类 | Doxygen 风格 /** ... */（含 @param/@return/@note）|
| 行内注释 | //  后空格，与代码间隔1空格 |
| 禁用 | 注释掉的代码（需保留时附带原因与清理计划）|


## 推荐
| 类别 | 规范 |
|------|------|
| 内存管理 | 优先 `std::unique_ptr`/`shared_ptr`<br> 禁止裸 `new`/`delete`（特殊场景需注释审批）<br> 严格遵循 RAII |
| 空指针 | `nullptr`（禁用 `NULL`/`0`） |
| const 正确性 | 成员函数不修改状态 → 标记 `const`<br>只读参数 → `const T&` |
| 避免污染 | 头文件中 禁止 `using namespace std;` |
| 字符串/容器 | 优先 `std::string_view`（C++17+）<br>优先 `std::vector`/`array` 代替原生数组 |
| 错误处理 | 业务错误 → 异常（`throw`）<br>内部逻辑断言 → `assert`（仅调试） |
| 枚举 | 强类型枚举 `enum class XxxYyy : int` |


## 提交与质量保障
| 环节 | 要求 |
|------|------|
| 代码格式化 | 修改代码后执行代码格式化 <br>（团队统一 `.clang-format`，基于 Google 风格微调） |
| 单元测试 | 新增功能需配套测试（Google Test）<br>测试文件：`xxx_test.cpp` |
| 文档 | 关键模块补充 `README.md` 或设计文档 |


## 日志（Logging）
| 项目 | 规范 |
|------|------|
| 日志入口 | 统一使用 `src/log/log.hpp` 提供的 `LTRACE/LDEBUG/LINFO/LWARN/LERROR/LCRITICAL/LFATAL` 宏；除 `src/log/` 模块外，禁止直接调用 `spdlog::*` |
| 语言 | 日志消息统一使用英文（便于检索与跨团队协作）；业务内容（如 prompt 文本）不受此限制 |
| 消息结构 | 推荐“动词短语 + 逗号分隔字段”的结构：`<Action in English>, key1: {}, key2: {}` |
| 字段格式 | 字段名使用 `lower_snake_case`；统一 `key: {}`，字段间用 `, ` 分隔；避免 `of type: {}` / `error code: {}` 等非结构化表达 |
| 成功/失败表达 | 开始：`Starting ...`；成功：`... succeeded` 或 `Finished ...`；失败：`... failed`（并补充 `error: {}` 或 `error_code: {}`） |

示例：

- `LINFO("Session created successfully, sid: {}", sid);`
- `LERROR("VLLM input build failed, sid: {}, error: {}", sid, e.what());`
- `LERROR("MMU global initialization failed, error_code: {}", ret);`

