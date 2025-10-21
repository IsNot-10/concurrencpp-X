# 协程 awaiter 崩溃与内存泄漏修复复盘（concurrencpp-X）

面向工程复盘与面试呈现，系统整理 `tcp_echo_server` 相关的两类问题：
- 运行期崩溃（`std::bad_function_call` → `std::terminate`）
- 退出阶段内存泄漏（协程状态未释放）

并总结定位方法、根因分析、修复方案、验证步骤与常见坑位。

---

## 背景与目标
- 背景：在优化 echo I/O 路径后，出现两类问题：
  - 稳定性：运行期偶发 `std::bad_function_call` 导致进程 `terminate`
  - 资源释放：退出阶段 Valgrind 报告 488B 堆内存泄漏
- 目标：不改变线程数与总体结构，修复崩溃与泄漏，同时保留既有 I/O 性能优化。

---

## 现象与栈迹
- 崩溃栈：位于 `asio::detail::binder1` 调用某个处理 `std::tuple<bool, std::error_code, size_t>` 的回调；随后抛出 `std::bad_function_call` 并触发 `std::terminate`。
- 泄漏栈：Valgrind 显示退出时仍有 488B 在堆上，调用栈包含 `concurrencpp::lazy_result<void>::run_impl()` 与 `concurrencpp::net::tcp_server::start()`。

---

## 根因分析

### 运行期崩溃（bad_function_call）
- 旧版 awaiter 采用“回调 setter”模型：用 `std::function<void(T)>` 记录结果写入与恢复；
- 当协程对象生命周期结束或 setter 未正确设置时，`asio` 的 binder 会调用一个“空的 std::function`”，引发 `std::bad_function_call` → `std::terminate`；
- 本质问题：结果写入与恢复耦合在一个可空回调里，存在生命周期与空调用风险。

### 退出泄漏（协程状态未释放）
- `lazy_result::run()` 返回 `result<void>`；若以 fire-and-forget 方式调用而不持有并 `wait()`，协程状态对象不会销毁；
- 接受循环与各会话协程都存在该问题，导致退出时仍有协程状态悬而未决。

---

## 修复方案（代码要点）

### awaiter 改造：共享存储 + 分离恢复
- 文件：`include/concurrencpp/net/asio_coro_util.hpp`
- 核心思路：
  - 使用 `std::shared_ptr<std::optional<T>>` 作为结果共享存储；
  - `asio` 完成回调仅负责 `storage->emplace(...)` 写入与 `handle.resume()` 恢复；
  - 在 `await_resume()` 读取共享存储，彻底避免“空函数调用”。
- 统一改造的协程接口：
  - `async_accept`
  - `async_read_some`
  - `async_read`
  - `async_read_until`
  - `async_write`
  - `async_connect`
  - `PeriodTimer::async_await`

### 服务器生命周期管理：持有并等待结果
- 文件：`include/concurrencpp/net/server.hpp`
- 改动要点：
  - 在 `tcp_server::start()` 保存接受循环返回的 `result<void>`：`accept_task_ = run().run();`
  - 在 `tcp_server::run()` 保存每个会话协程的 `result<void>` 到 `sessions_`（加锁保护）；
  - 在 `tcp_server::stop()` 采用优雅停止顺序：
    1) 取消/关闭 `acceptor_`（令 `async_accept` 返回 `operation_aborted`）
    2) 关闭所有活动 `socket`（打断挂起 I/O）
    3) `if (accept_task_) accept_task_.wait();` 与遍历 `sessions_` 逐个 `wait()`，释放协程状态
  - 错误处理保持“返回错误并退出循环”，避免异常越过协程边界导致 `terminate`。

---

## 验证步骤

### 重建与运行
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/examples/tcp_echo_server/tcp_echo_server 9980
```

### Valgrind 复验
```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
  --log-file=valgrind-tcp-9980.log ./build/examples/tcp_echo_server/tcp_echo_server 9980
```

### 快速判定
- 若日志仍出现 `binder1 ... std::function<void(std::tuple<...>)>` 或 `__throw_bad_function_call`：大概率仍在使用旧 awaiter 签名或未完整重建；
- 若退出仍有泄漏：检查是否仍有 fire-and-forget 的 `lazy_result::run()` 未保存其 `result<void>` 并在停止阶段 `wait()`；
- 若运行期抛异常后 `terminate`：检查是否将 I/O 错误改为异常传播而非错误返回与循环退出。

---

## 常见坑位与排雷
- 没有完整重建示例，链接到了旧头文件（尤其是 `asio_coro_util.hpp` 与 `server.hpp`）；
- awaiter 回调仍然使用 setter 模式，导致空函数调用；
- 会话协程中对错误采用抛异常而非退出循环；
- `stop()` 顺序错误：先强杀 socket 再等 accept，易悬挂或竞态；
- 保存 `result<void>` 缺失互斥保护，停止阶段存在数据竞争。

---

## 经验与面试要点
- 长期运行的协程不要 fire-and-forget：必须持有并在关闭阶段 `wait()` 释放状态；
- 将“结果写入”与“协程恢复”分离，通过共享存储消除空回调风险；
- 优雅停止顺序：取消接受 → 关闭 socket → 等待所有结果；
- 性能优化与生命周期管理解耦，既保留优化又确保稳定性；
- 验证闭环要完整：构建一致性、运行稳定性、工具复验（Valgrind）。

---

## 操作小抄
```bash
# 重新配置与构建（全项目/示例）
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# 运行 echo server（端口可改）
./build/examples/tcp_echo_server/tcp_echo_server 9980

# Valgrind 检测（日志输出到项目根目录）
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
  --log-file=valgrind-tcp-9980.log ./build/examples/tcp_echo_server/tcp_echo_server 9980
```

---

## 关联文件
- `include/concurrencpp/net/asio_coro_util.hpp`（awaiter 共享存储模型）
- `include/concurrencpp/net/server.hpp`（生命周期管理、优雅停止）
- `example/tcp_echo_server/`（示例目标，需确认构建链接最新头）
- `valgrind-tcp-9980.log`（运行与泄漏验证日志）

---

## 演进对比：旧 awaiter 是怎样的，为什么会崩溃

### 旧设计（回调 setter，耦合写入与恢复）

示意（简化伪代码）：

```cpp
// 旧版 CallbackAwaiterWithResult<T>
struct CallbackAwaiterWithResultOld {
  std::function<void(T)> setter; // 可空函数，作为“写入+恢复”的载体

  bool await_ready() const { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    // 在某处（或稍后）设置 setter，捕获 h，并在 I/O 完成时调用
    setter = [h](T value){ /* 写入结果（若有） */ h.resume(); };
    // 启动 Asio I/O，将 setter 作为 handler 或通过 binder 间接调用
  }
  T await_resume() { /* 从某处读取结果 */ }
};
```

问题根因：
- setter 可为空（未设置/对象已销毁），但 `asio::detail::binder1` 在 I/O 完成时仍会调用它；
- 空的 `std::function` 被调用 → 抛出 `std::bad_function_call` → 触发 `std::terminate`；
- 写入结果与恢复耦合在同一个 setter 中，且该 setter 可能随 awaiter 生命周期结束而失效，形成生命周期陷阱；
- 典型栈迹表现为 `binder1` 调用 `std::function<void(std::tuple<...>)>`，随后出现 `__throw_bad_function_call`。

时间线示例（崩溃复现路径）：
- t0：协程进入 `await_suspend`，I/O 异步挂起；
- t1：await 对象生命周期结束或 setter 尚未正确设置；
- t2：Asio I/O 完成，binder 调用空 setter → `bad_function_call`；
- t3：由于异常越过协程边界，进程 `std::terminate`。

### 旧设计的额外隐患（泄漏）
- 与崩溃相互独立，但常同时出现：`lazy_result::run()` 返回 `result<void>` 未持有并 `wait()`，导致协程状态不会释放；
- 接受循环和会话协程以 fire-and-forget 使用，退出阶段 Valgrind 报告残留堆块。

---

## 新设计：怎么修改，为什么就好了

### awaiter 改造（共享存储 + 分离恢复）

示意（简化伪代码）：

```cpp
// 新版 CallbackAwaiterWithResult<T>
template <class T>
struct CallbackAwaiterWithResult {
  std::shared_ptr<std::optional<T>> storage = std::make_shared<std::optional<T>>();

  bool await_ready() const { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    // 将 lambda 直接传给 Asio 作为完成回调（非可空 std::function 成员）
    auto cb = [storage = storage, h](/* asio args ... */) {
      storage->emplace(/*构造 T*/);
      h.resume();
    };
    // 启动 Asio I/O，传入 cb
  }
  T await_resume() {
    // 从共享存储读取结果
    return std::move(storage->value());
  }
};
```

关键变化与效果：
- 结果写入与协程恢复分离：不再依赖 awaiter 内部的“可空 setter”；
- 共享存储持久化：即使 awaiter 对象生命周期结束，`storage` 仍存在，I/O 回调写入不会丢失；
- 回调为具体 lambda（由 Asio持有并调用），不会出现“调用空 std::function”的情况；
- `await_resume()` 仅从 `storage` 读取结果，语义简单可靠；
- 同步适配到所有 I/O awaiter：`async_accept/read*_some/read_until/write/connect` 与 `PeriodTimer::async_await`。

### 服务器生命周期（持有并等待结果）
- 在 `tcp_server::start()` 保存接受循环返回的 `result<void>`：`accept_task_ = run().run();`
- 在 `run()` 保存每个会话 `result<void>` 到 `sessions_`（加锁保护）；
- 在 `stop()` 优雅停止：取消/关闭 `acceptor_` → 关闭全部 `socket` → `wait()` 接受循环与所有会话结果；
- 错误路径采用错误返回与循环退出，避免异常越过协程边界导致 `terminate`。

### 为什么就好了（设计不变量）
- 不变量 1：I/O 完成回调不依赖“可空”函数成员；
- 不变量 2：结果写入载体独立于 awaiter 生命周期（共享存储）；
- 不变量 3：协程恢复由回调直接驱动，路径单一，避免空调用；
- 不变量 4：所有长期协程的状态对象在关闭阶段被显式 `wait()` 回收；
- 不变量 5：错误以返回值驱动退出，不以异常传播跨协程边界。

### 对比时间线（修复后路径）
- t0：协程进入 `await_suspend`，将“具体 lambda”注册给 Asio；
- t1：await 对象结束生命周期（不影响 lambda 与共享存储）；
- t2：Asio I/O 完成，lambda `emplace` 结果到 `storage` 并 `resume(h)`；
- t3：协程在 `await_resume` 读取结果并继续执行，无空函数调用风险；
- t4：关闭阶段统一 `wait()` 接受与会话结果，协程状态全部释放。

### 验证信号（快速确认）
- 崩溃消失：日志不再出现 `binder1 ... std::function<void(std::tuple<...>)>` 或 `__throw_bad_function_call`；
- 泄漏消失：Valgrind 报告 `All heap blocks were freed`；
- 行为稳定：读写错误触发循环退出而非异常终止。

docs/awaiters_troubleshooting.md ，重点说明：

- 旧实现是怎样的：使用可空的 std::function<void(T)> 作为“结果写入+恢复”的 setter，I/O 完成由 asio::detail::binder1 间接调用它。
- 为什么会崩溃：setter 可能未设置或随 awaiter 生命周期结束而失效，被 binder 调用时是空函数，触发 std::bad_function_call → std::terminate 。
- 怎么修改：统一改为“共享存储 + 分离恢复”模型，用 std::shared_ptr<std::optional<T>> 持有结果，I/O 回调只负责 emplace 与 handle.resume() ； await_resume() 从存储读取结果。并覆盖 async_accept/read*_some/read_until/write/connect/PeriodTimer::async_await 。
- 为什么就好了：回调不再依赖可空函数成员、结果载体独立于 awaiter 生命周期、恢复路径单一可靠；同时服务器在关闭阶段持有并 wait() 所有 result<void> ，彻底释放协程状态；错误沿返回值驱动退出而非异常跨边界。