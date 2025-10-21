# 协程与 `concurrencpp` 工具

- `result<T>`：节点提交返回；用于 `get()` 或 `co_await` 等待完成。
- `when_all`/`when_any`：多结果组合；适用于并行分支汇聚与选择器。
- `resume_on(executor)`：在指定执行器恢复协程；适合切换到线程池。
- `generator<T>`：流式产出与背压；后续用于流水线式节点。

## 节点协程包装示例
```
auto async_node = [&](std::function<void()> fn, std::shared_ptr<executor> ex) -> concurrencpp::result<void> {
  co_await concurrencpp::resume_on(ex);
  fn();
  co_return;
};
```

## 并行汇聚示例
```
std::vector<concurrencpp::result<void>> rs;
// 提交多个节点得到 rs
co_await concurrencpp::when_all(std::move(rs));
```