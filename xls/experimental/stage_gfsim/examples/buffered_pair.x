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
#![feature(channel_attributes)]

proc Producer {
    input: chan<u32> in,
    output: chan<u32> out,
}

impl Producer {
    fn new(input: chan<u32> in, output: chan<u32> out) -> Self {
        Self { input, output }
    }

    fn next(self) {
        let (tok, x) = recv(join(), self.input);
        send(tok, self.output, x + u32:3);
    }
}

proc Consumer {
    input: chan<u32> in,
    output: chan<u32> out,
}

impl Consumer {
    fn new(input: chan<u32> in, output: chan<u32> out) -> Self {
        Self { input, output }
    }

    fn next(self) {
        let (tok, x) = recv(join(), self.input);
        send(tok, self.output, x * x);
    }
}

pub proc BufferedPair {}

impl BufferedPair {
    fn new(input: chan<u32> in, output: chan<u32> out) -> Self {
        let (send_mid, recv_mid) =
            #[channel(depth=2, bypass=false)]
            chan<u32>("mid");
        Producer::new(input, send_mid).spawn();
        Consumer::new(recv_mid, output).spawn();
        Self {}
    }

    fn next(self) {}
}
