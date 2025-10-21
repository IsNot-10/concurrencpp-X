# 典型用例

## 示例 1：A→B、A→C→D（并行/串行混合）
- 节点：A,B,C,D；边：A→B，A→C，C→D。
- 行为：A 完成后 B 与 C 并行；C 完成后触发 D。

## 示例 2：Serial 与 Parallel 组合器
- `Serial({A,B,C})` 等价于 A→B→C；
- `Parallel({X,Y,Z})` 父后并行启动 X/Y/Z，随后 `when_all` 汇聚到 W。

## 示例 3：错误策略与取消
- 假设 B 抛出异常：
  - `continue_on_error`：记录异常，其他不依赖 B 的分支继续。
  - `cancel_on_error`：取消依赖 B 的尚未启动分支。