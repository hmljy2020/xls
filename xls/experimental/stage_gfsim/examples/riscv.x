// Copyright 2026 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#![feature(explicit_state_access)]
#![feature(generics)]

import xls.examples.riscv_simple;

type CpuState = (u32, u32[32], u8[16]);

pub proc Riscv {
    instruction: chan<u32> in,
    result: chan<CpuState> out,
    state: CpuState,
}

impl Riscv {
    fn new(instruction: chan<u32> in, result: chan<CpuState> out) -> Self {
        Self {
            instruction,
            result,
            state: (u32:0, u32[32]:[0, 1, 2, 0, ...], u8[16]:[0, ...]),
        }
    }

    fn next(self) {
        let (tok, instruction) = recv(join(), self.instruction);
        let (pc, regs, dmem) = read(self.state);
        let updated = riscv_simple::run_instruction(pc, instruction, regs, dmem);
        write(self.state, updated);
        send(tok, self.result, updated);
    }
}
