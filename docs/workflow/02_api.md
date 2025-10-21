# API 设计与头文件解耦

## 头文件划分
- `include/concurrencpp/workflow/node.h`
- `include/concurrencpp/workflow/graph.h`
- `include/concurrencpp/workflow/scheduler.h`
- `include/concurrencpp/workflow/workflow.h`（门面）
- 可选组合器：`include/concurrencpp/workflow/serial.h`、`parallel.h`

## 核心类型（草案）
```
namespace concurrencpp::workflow {
  enum class ErrorPolicy { continue_on_error, cancel_on_error };

  struct Node {
    std::string name;
    std::function<void()> fn;
    std::shared_ptr<concurrencpp::executor> exec;
    // 状态与钩子略
  };

  class Graph {
   public:
    size_t add_node(Node n);
    void add_edge(size_t u, size_t v);
    // 访问与准备就绪计算
  };

  class Scheduler {
   public:
    explicit Scheduler(std::shared_ptr<concurrencpp::executor> default_exec);
    void on_start(std::function<void(const Node&)> cb);
    void on_complete(std::function<void(const Node&)> cb);
    void on_error(std::function<void(const Node&, const std::exception&)> cb);
    void set_error_policy(ErrorPolicy p);
    void set_timeout(std::chrono::milliseconds);
    concurrencpp::result<void> run(Graph& g, concurrencpp::runtime& rt);
  };

  class Workflow {
   public:
    explicit Workflow(std::shared_ptr<concurrencpp::executor> default_exec = nullptr);
    size_t add_task(std::string name, std::function<void()> fn, std::shared_ptr<concurrencpp::executor> exec = nullptr);
    void add_edge(size_t u, size_t v);
    void set_error_policy(ErrorPolicy p);
    void set_timeout(std::chrono::milliseconds);
    // 钩子注册略
    concurrencpp::result<void> run(concurrencpp::runtime& rt);
    void reset();
  };
}
```

## 组合器（语法糖）
- `Serial({A,B,C})`：内部创建链式边；
- `Parallel({X,Y,Z})`：父后并行启动，使用 `when_all` 汇聚到后续节点；