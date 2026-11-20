#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure trigger-to-output delays in a stereo signed 16-bit PCM WAV file."""

import argparse
from bisect import bisect_left, bisect_right
import json
import math
from pathlib import Path
from struct import iter_unpack
import wave


class OnsetDetector:
    """Use absolute amplitude and require continuous quiet before rearming."""

    def __init__(self, threshold, quiet_frames):
        self.threshold = threshold
        self.release = threshold // 2
        self.quiet_frames = quiet_frames
        self.starts = []
        self.ends = []
        self.quiet = 0

    def observe(self, sample, frame):
        level = abs(sample)
        if len(self.starts) == len(self.ends):
            if level >= self.threshold:
                self.starts.append(frame)
                self.quiet = 0
            return

        if level <= self.release:
            self.quiet += 1
            if self.quiet == self.quiet_frames:
                self.ends.append(frame + 1)
        else:
            self.quiet = 0

    def busy_before(self, frame, next_start):
        """Return whether the preceding output onset has yet to rearm."""
        previous = next_start - 1
        return previous >= 0 and (
            previous >= len(self.ends) or self.ends[previous] > frame
        )


def nearest_rank(sorted_values, numerator):
    rank = (len(sorted_values) * numerator + 99) // 100
    return sorted_values[rank - 1]


def measure(
    path,
    *,
    trigger_threshold=4096,
    output_threshold=1024,
    min_ms=0.0,
    max_ms=50.0,
    quiet_ms=5.0,
    channel_skew_frames=0,
    expected_triggers=None,
):
    """Compare reference and output onsets on the recorder's shared clock."""
    if any(not 1 <= threshold <= 32768 for threshold in (trigger_threshold, output_threshold)):
        raise ValueError("thresholds must be PCM magnitudes from 1 to 32768")
    if not all(math.isfinite(value) for value in (min_ms, max_ms, quiet_ms)):
        raise ValueError("times must be finite")
    if min_ms > max_ms or quiet_ms <= 0:
        raise ValueError("use min-ms <= max-ms and positive quiet-ms")
    if not isinstance(channel_skew_frames, int):
        raise ValueError("channel skew must be a signed integer number of frames")
    if expected_triggers is not None and expected_triggers < 1:
        raise ValueError("expected-triggers must be positive")

    with wave.open(str(path), "rb") as recording:
        if (recording.getnchannels(), recording.getsampwidth(), recording.getcomptype()) != (
            2, 2, "NONE"
        ):
            raise ValueError("expected stereo signed 16-bit PCM WAV")
        rate = recording.getframerate()
        # The first allowed frame rounds up and the last allowed frame rounds down.
        first_offset = math.ceil(min_ms * rate / 1000) + channel_skew_frames
        last_offset = math.floor(max_ms * rate / 1000) + channel_skew_frames
        if first_offset > last_offset:
            raise ValueError("the search window contains no complete sample position")
        quiet_frames = max(1, math.ceil(quiet_ms * rate / 1000))
        reference = OnsetDetector(trigger_threshold, quiet_frames)
        output = OnsetDetector(output_threshold, quiet_frames)
        frames = 0
        while data := recording.readframes(8192):
            if len(data) % 4:
                raise ValueError("invalid partial stereo frame")
            for left, right in iter_unpack("<hh", data):
                reference.observe(left, frames)
                output.observe(right, frames)
                frames += 1
        if frames != recording.getnframes():
            raise ValueError("WAV ended before its declared frame count")

    trials = []
    delays = []
    starts = reference.starts
    for index, trigger in enumerate(starts):
        first = trigger + first_offset
        last = trigger + last_offset
        result = {"trigger_frame": trigger}
        overlaps_previous = index > 0 and starts[index - 1] + last_offset >= first
        overlaps_next = index + 1 < len(starts) and starts[index + 1] + first_offset <= last
        if overlaps_previous or overlaps_next:
            result["problem"] = "overlapping_trigger_windows"
        elif first < 0:
            result["problem"] = "recording_started_inside_window"
        elif last >= frames:
            result["problem"] = "recording_ended_inside_window"
        else:
            begin = bisect_left(output.starts, first)
            end = bisect_right(output.starts, last)
            if output.busy_before(first, begin):
                result["problem"] = "output_already_active"
            elif begin == end:
                result["problem"] = "missing_output"
            elif end - begin > 1:
                result["problem"] = "multiple_output_onsets"
                result["candidate_frames"] = output.starts[begin:end]
            else:
                response = output.starts[begin]
                delay_frames = response - trigger - channel_skew_frames
                delay = delay_frames * 1000 / rate
                result.update(output_frame=response, delay_frames=delay_frames, delay_ms=delay)
                delays.append(delay)
        trials.append(result)

    delays.sort()
    summary = None
    if delays:
        summary = {
            "min": delays[0],
            "median": nearest_rank(delays, 50),
            "p95": nearest_rank(delays, 95),
            "p99": nearest_rank(delays, 99),
            "max": delays[-1],
        }
    complete = bool(starts) and len(delays) == len(starts) and (
        expected_triggers is None or expected_triggers == len(starts)
    )
    return {
        "complete": complete,
        "sample_rate_hz": rate,
        "frames": frames,
        "settings": {
            "trigger_threshold": trigger_threshold,
            "output_threshold": output_threshold,
            "search_offsets_frames": [first_offset, last_offset],
            "rearm_quiet_frames": quiet_frames,
            "channel_skew_frames": channel_skew_frames,
        },
        "expected_triggers": expected_triggers,
        "detected_triggers": len(starts),
        "matched_triggers": len(delays),
        "output_onsets_without_match": len(output.starts) - len(delays),
        "summary_ms": summary,
        "trials": trials,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=(
            "Record the reference pulse on channel 1 and the device output on channel 2. "
            "Use sufficient leading and trailing silence. Keep pulses further apart than "
            "the search window and leave both channels quiet between trials. Onsets can "
            "have either polarity. Thresholds are absolute 16-bit sample magnitudes. "
            "A channel rearms after consecutive samples at or below half its threshold. "
            "The JSON reports all detected reference pulses and nearest-rank percentiles "
            "over successful pairs. Exit status 1 means at least one detected pair failed "
            "or the detected trigger count did not meet expectations. No reference pulses "
            "also gives status 1. Invalid input gives status 2."
        ),
    )
    parser.add_argument("wav", type=Path, help="stereo signed 16-bit PCM WAV")
    parser.add_argument("--trigger-threshold", type=int, default=4096, help="channel 1 onset level (default 4096)")
    parser.add_argument("--output-threshold", type=int, default=1024, help="channel 2 onset level (default 1024)")
    parser.add_argument("--min-ms", type=float, default=0, help="earliest allowed output delay (default 0 ms)")
    parser.add_argument("--max-ms", type=float, default=50, help="latest allowed output delay (default 50 ms)")
    parser.add_argument("--quiet-ms", type=float, default=5, help="consecutive quiet time to rearm (default 5 ms)")
    parser.add_argument(
        "--channel-skew-frames", type=int, default=0,
        help="measured channel 2 delay to subtract, in signed recorder frames (default 0)",
    )
    parser.add_argument("--expected-triggers", type=int, help="reference pulse count required for successful run")
    args = parser.parse_args(argv)
    try:
        result = measure(
            args.wav,
            trigger_threshold=args.trigger_threshold,
            output_threshold=args.output_threshold,
            min_ms=args.min_ms,
            max_ms=args.max_ms,
            quiet_ms=args.quiet_ms,
            channel_skew_frames=args.channel_skew_frames,
            expected_triggers=args.expected_triggers,
        )
    except (EOFError, OSError, OverflowError, ValueError, wave.Error) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))
    return 0 if result["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
