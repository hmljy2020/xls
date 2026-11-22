# 同一状态元素的跨 stage 条件写入

## 现象

XLS 默认优化把 RISC-V 的 PC 更新拆成了分布在 stage 1 和 stage 2 的
多条带谓词的 `next_value`。GFSIM 生成器遇到同一状态元素的第二条
`next_value` 就报 `UNIMPLEMENTED`。[生成器](../generate.cc)

## 复现

运行 `run_riscv_example.sh --default-pipeline` 生成默认调度产物。
生成器会先遇到 IR `assert`。在诊断副本中移除断言后，仍可单独复现
`next_value` 报错。当前合法指令示例通过 `--skip_passes=next_value_opt`
保留每个状态元素的一条写回。

## 状态时序

当前运行时在写回 stage 推进时释放状态，即使写入谓词为假。若 stage 1
的写入条件为假，实际写回发生在 stage 2，直接放开生成器检查会让下一次
激活提前读到旧 PC。若 stage 1 已写回，旧激活在 stage 2 的无效写入也可能
释放新激活持有的状态。[状态提交](../runtime.cc)

应按同一次 proc 激活处理实际写回和状态何时再次可读，并用带回压的
跨 stage 条件写入例子与 Block IR 对照验证。
