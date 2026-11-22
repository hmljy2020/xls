# RISC-V 的 GFSIM 仿真提速

451 条指令的稳态仿真从 104.4 ms 降到 0.719 ms，背压场景从 110.8 ms 降到 0.742 ms。两种场景分别提速 145 倍和 149 倍，耗时达到 Verilator 的同一量级。

## 瓶颈

原来的 RISC-V 模型已经生成了 C++ stage 方法，但其中有 207 处 `stage.Interpret()` 调用点。每次调用按名字查找 IR 节点，构造单节点解释器，把操作数放进映射表，再执行该节点。稳态采样中，约 52% 的 GFSIM 用户态周期样本的调用栈经过 `StageContext::Interpret`。寄存器组和内存还通过嵌套的 XLS `Value` 在 stage、proc 状态和 FIFO 之间传递，反复复制容器。[XLS 单节点解释入口](../../../interpreter/ir_interpreter.cc)

## 实现

生成器在读取 `scheduled_proc` 时已知操作、位宽和数据类型，可以把它们直接写成 C++。例如，32 位加法生成 `ClipBits<32>(uint64_t{base} + offset)`，寄存器组使用 `std::array<uint32_t, 32>`。stage 成员、proc 状态和 GFSIM FIFO 都使用这些定宽类型。编译器可以内联位宽处理、折叠常量并消去临时值。XLS `Value` 转换留在输入加载、状态初始化和 trace 输出的边界。[运算生成](../native_expression.cc) · [定宽类型](../native_value.h) · [FIFO](../proc_runtime.h)

生成入口仍为 DSLX → `scheduled_proc` → GFSIM C++。stage 按 `Work()` 收集更新，`Xfer()` 发布下一拍的 stage 数据和 proc 状态。native 后端支持已覆盖的 IR 操作和最多 64 位的标量，数组和元组由这些类型递归组成。模型遇到尚未覆盖的类型或操作时，回退到 XLS `Value` 路径。改动都在实验目录内。[生成器](../generate.cc)

## 测量与验证

| 场景 | 模拟周期 | 原 GFSIM | Native GFSIM | Verilator |
| --- | ---: | ---: | ---: | ---: |
| 稳态 | 1356 | 104.4 ms | 0.719 ms | 0.635 ms |
| 背压 | 1483 | 110.8 ms | 0.742 ms | 0.689 ms |

三者使用同一份 schedule、输入和深度为 2 的外部 FIFO。表中是单核优化构建关闭 trace 后七次交错运行的中位数，计时包括 FIFO 处理，范围是仿真循环。两种场景共 3608 次传输的值和周期均与 Block IR 和 Verilator 逐项一致。复现命令见 [README 的计时章节](../README.md#simulator-runtime-comparison)。
