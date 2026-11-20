# DirectJack experimental driver

This out-of-tree ALSA driver feeds the Raspberry Pi 3 Model B analog jack using PWM and cyclic DMA. Its supported machine is checked at probe time. The tested kernel was Raspberry Pi Linux `6.18.50+rpt-rpi-v8` on a Pi 3 Model B Rev 1.2. The PCM device accepts stereo S16_LE at 44.1 or 88.2 kHz. This prototype has board-specific PWM routing and expects the firmware analog driver, other PWM consumers and conflicting overlays to be inactive.

The C source and Makefile here match the verified test-build manifest. The overlay is the board-specific source used by the test setup. These are GPL-2.0-only materials. [License text](COPYING)

## Build and verify

On a test Pi with a compiler, matching kernel build headers and the Device Tree compiler:

```sh
cd driver
make -C "/lib/modules/$(uname -r)/build" M="$PWD" modules
dtc -@ -I dts -O dtb -o arrangerlab-direct-jack.dtbo arrangerlab-direct-jack-overlay.dts
modinfo ./direct_jack.ko
```

For a reversible test, place the compiled overlay in the boot partition's `overlays` directory. Copy the working boot configuration to a separate `tryboot.txt` first. In the test copy disable firmware analog audio with `dtparam=audio=off` and add `dtoverlay=arrangerlab-direct-jack` in an applicable section. Check that later includes and overlays do not turn those conflicting consumers back on. Our test also used a separate command line with `module_blacklist=snd_bcm2835`. Keep access to a normal reboot or a known working SD card. The test flag is one-shot. [Raspberry Pi boot configuration](https://www.raspberrypi.com/documentation/computers/config_txt.html) · [One-shot tryboot](https://www.raspberrypi.com/documentation/hardware/video/config_txt.html#fail-safe-os-updates-tryboot)

After booting the test configuration, check that the firmware analog card did not register and that nothing else owns this PWM instance. Load the matching module on the isolated test boot:

```sh
cat /proc/asound/cards
sudo insmod ./direct_jack.ko
aplay -l
modinfo -F vermagic ./direct_jack.ko
```

Run the [silent ALSA example](../examples/README.md) on `hw:CARD=DirectJack,DEV=0` at 88,200 Hz, 32 frames per period and a 1,024-frame ring. Inspect `hw_params` and `status` for the corresponding playback substream under `/proc/asound`. Confirm the real sample rate and geometry before playing a quiet test tone. A normal reboot returns to the working boot configuration if that configuration remains intact.

## How the driver maps frames

- `.prepare` fills the DMA ring with midpoint duty, resets the fractional accumulators and submits a cyclic descriptor.
- `.ack` encodes newly committed little-endian stereo PCM frames into interleaved PWM duties and publishes the data before the consumer reaches it. The kernel driver advertises that rewinds are unsupported.
- `.trigger` configures the FIFO and starts DMA. `.pointer` derives the PCM position from DMA residue. Period callbacks wake ALSA clients. Scheduling and hardware FIFO movement still limit timing around the pointer.

See [the main article](../README.md#why-the-pi-3-driver-mattered) for the clock and duty quantization rationale. The public repository contains source only. Use headers for the running kernel and test on the exact board before relying on it for a performance.
