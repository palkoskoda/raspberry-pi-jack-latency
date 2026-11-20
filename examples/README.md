# Run the examples

## Adaptive ALSA queue

The C example negotiates a fixed playback ring, primes just the chosen fill, writes whole periods and increases its fill on `-EPIPE`. A 30-second interval of successful playback lets it reduce the fill by a period. The library code and hardware determine the accepted geometry. Use your device's real card name from `aplay -l`.

On Linux, install your distribution's ALSA development headers, such as `libasound2-dev` on Raspberry Pi OS:

```sh
cc -std=c11 -O2 -Wall -Wextra examples/adaptive_pcm.c -lasound -lm -o adaptive_pcm
./adaptive_pcm --self-test
./adaptive_pcm 'hw:CARD=YOUR_CARD,DEV=0' 10 48000 64 512
```

The arguments are device, duration in seconds, requested rate, requested period and requested buffer capacity in frames. Playback is silent unless you append `--tone`. For the experimental Pi 3 driver use `hw:CARD=DirectJack,DEV=0` and arguments `10 88200 32 1024`. Start with a larger working period on stock hardware and record the negotiated values.

There is also a policy-only build that needs a standard C compiler:

```sh
cc -std=c11 -O2 -Wall -Wextra -DPOLICY_ONLY examples/adaptive_pcm.c -o depth_test
./depth_test
```

The demo keeps synthesis, realtime scheduling and device power policy simple. It reports the negotiated geometry, period writes and observed queue fill, including any underrun recovery. Integrate the fill logic with your renderer's timeline before using it in an instrument.

## Electrical trigger-to-output measurement

Use a stereo PCM16 capture made with one common clock. Put the conditioned event reference on channel 1 and the jack output on channel 2. Start and end with quiet. Leave a gap longer than the response window between reference pulses. Use a dry patch with a clear attack and release.

```sh
python3 examples/measure_loopback.py capture.wav \
  --trigger-threshold 4096 --output-threshold 1024 \
  --max-ms 50 --quiet-ms 5 --expected-triggers 50
python3 -m unittest discover -s examples -p 'test_measure_loopback.py'
```

Thresholds are absolute 16-bit sample magnitudes. The analyzer rearms after samples remain at or below half the threshold for the quiet interval. It reports JSON for each reference pulse and returns a nonzero exit status when a trial is missing, ambiguous or incomplete. Inspect the waveform for clipping, offsets, cross-talk and double triggers. Using two independent recorders can introduce unrelated clocks and offsets. See the [measurement procedure](../README.md#how-to-measure-your-setup) for reference timing and calibration.

To calibrate the two inputs, give both channels the same conditioned pulse and run the analyzer on that capture with `--min-ms -1 --max-ms 1`. Check that the repeated `delay_frames` values are consistent. If channel 2 is consistently two frames later, add `--channel-skew-frames 2` to the actual instrument measurement. Both the search window and reported offsets account for this correction. Keep recorder settings and wiring fixed after calibration.
