# Measurement data and scope

[man-onsets.csv](man-onsets.csv) contains the relative timing of each of 48 MAN note attacks in three configurations on the physical Raspberry Pi 3 Model B. All use the 88.2 kHz DirectJack route, a 32-frame period, a 1,024-frame hardware ring and a 128-frame starting fill. The input was an isolated virtual MIDI stream. All notes were released. The rows are in chronological order within each run, including the first attack.

| Run | p95 (ms) | Max (ms) | Below 4 ms |
| --- | ---: | ---: | ---: |
| `adaptive_auto_pcm` | 4.538 | 15.325 | 42 / 48 |
| `jvm_priority_shared_auto_cpu` | 3.577 | 4.901 | 47 / 48 |
| `jvm_priority_separate_auto_cpu` | 3.594 | 7.031 | 46 / 48 |

`latency_ms` spans complete MIDI reception to the first nonzero PCM frame's estimated DMA time. The intermediate stage columns are relative monotonic timestamps. `pre_emit_to_rust_read_ms` includes work after the JVM's pre-emit timestamp, pipe transit and scheduling of the reader. Separating those costs requires timestamps around the actual write and reader wakeup. `observation_bracket_us` measures the time bracket around the device query, one component of uncertainty in the final estimate. An empty intermediate cell means the instrumentation could not determine that segment. Each p95 is nearest rank at index `ceil(0.95 × count)`, counting from 1.

Use the stage columns for prioritizing work and compare controls under the same configuration. Repeat the runs with controlled workload and boot conditions to assess causality and deadline reliability. The physical Roland listening check was performed on an earlier build. To measure the analog result on your device, use a two-channel reference as described in the [article](../README.md#how-to-measure-your-setup).
