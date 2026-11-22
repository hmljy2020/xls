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

## 第二轮优化到 60 µs 量级

对原生模型重新采样时，约 60% 的仿真循环采样栈经过 `SimQueue`。FIFO 断言在条件成立时也会构造诊断流，完整状态快照入队还要分配堆对象。第一步把 FIFO 换成预分配环形缓冲，直接存放定宽数据，并在每拍开始快照可读和可写状态。这样满队列在本拍被读出后，要到下一拍才允许写入。[FIFO 实现](../proc_runtime.h)

- **减少计算。** 生成器把标量化后的寄存器组和内存写入还原为按索引更新，保留越界写入和多写入优先级的语义。它把纯计算移到最早使用它的 stage，并根据 IR 中的常量比较生成专门分支。RISC-V 的分支判别量是 opcode，已知值可以在 C++ 编译期折叠，其余值走通用分支。接收、发送和状态更新仍在各自的调度 stage 发生。[数组重写](../native_rewrites.cc) · [stage 生成](../generate.cc)
- **减少搬运。** 下游先运行并读完自己的输入，上游才复用空出的槽位。待发布的有效位用位图记录，`Xfer()` 提交下一拍的有效状态。若反馈约束保证状态值一直可读到最后一次使用，后续 stage 直接读原状态。大的状态在两块缓冲间切换，`Xfer()` 只改变当前缓冲索引。[stage 生成](../generate.cc) · [状态缓冲](../proc_runtime.h)
- **减少每拍调度开销。** 生成的网络直接连接具体 proc 和 FIFO。默认驱动逐拍调用生成的 `Work()` 与 `Xfer()`，关闭 trace 时生成的热路径不包含 trace 转换。用 `--gfsim_step` 可让同一网络通过通用 GFSIM 时钟驱动运行，便于对照。[网络生成](../generate.cc) · [驱动入口](../runtime.cc)

| 场景 | 模拟周期 | 编译驱动 | GFSIM Step | Verilator | 相对 Verilator |
| --- | ---: | ---: | ---: | ---: | ---: |
| 稳态 | 1356 | 60.8 µs | 86.2 µs | 634.5 µs | 10.43× |
| 背压 | 1483 | 63.5 µs | 91.2 µs | 685.9 µs | 10.81× |

沿用同一份 451 条指令的 schedule 和测试输入。表中为单核优化构建关闭 trace 后 11 次交错运行的中位数，包含 FIFO 处理。打开 trace 后，两种驱动的 stage 计数一致，3608 次传输的完整数据和周期均与 Block IR 和 Verilator 逐项匹配。另重跑了三个 DSLX 示例、短 RISC-V 负载和 128 位回退用例。[计时和复现命令](../README.md#simulator-runtime-comparison)
