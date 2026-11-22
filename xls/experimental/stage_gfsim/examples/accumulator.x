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

pub proc Accumulator {
    input: chan<u32> in,
    output: chan<u32> out,
    sum: u32,
}

impl Accumulator {
    fn new(input: chan<u32> in, output: chan<u32> out) -> Self {
        Self { input, output, sum: u32:0 }
    }

    fn next(self) {
        let (tok, x) = recv(join(), self.input);
        let sum = read(self.sum) + x * x;
        // Use the state-dependent value in several scheduled operations so
        // the next state crosses a stage at the example's target II.
        let updated = (sum ^ u32:7) * u32:3;
        write(self.sum, updated);
        send(tok, self.output, updated);
    }
}
