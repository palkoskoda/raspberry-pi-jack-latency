// SPDX-License-Identifier: GPL-2.0-only
/* Experimental ALSA playback through the Pi 3 PWM FIFO and a cyclic DMA ring. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define BASE_SAMPLE_RATE 44100
#define PWM_MAX_REPEAT 4
#define PWM_RANGE 560
#define PWM_MIDPOINT (PWM_RANGE / 2)
#define BIAS_RAMP_STEPS 80U
#define PWM_FRACTION_BITS 15
#define PWM_FRACTION_MASK ((1U << PWM_FRACTION_BITS) - 1)
#define PWM_CLOCK (BASE_SAMPLE_RATE * PWM_MAX_REPEAT * PWM_RANGE)
#define DMA_MAX_FRAME_BYTES (2 * PWM_MAX_REPEAT * sizeof(u32))
#define MAX_PCM_FRAMES 4096
#define PWM_CTL 0x00
#define PWM_STA 0x04
#define PWM_DMAC 0x08
#define PWM_RNG1 0x10
#define PWM_DAT1 0x14
#define PWM_FIFO 0x18
#define PWM_RNG2 0x20
#define PWM_DAT2 0x24
#define PWM_CLR_FIFO BIT(6)
#define PWM_FIFO_ERRORS (BIT(8) | BIT(5) | BIT(4) | BIT(3) | BIT(2))
#define PWM_STEREO_STATIC (BIT(0) | BIT(7) | BIT(8) | BIT(15))
#define PWM_STEREO_FIFO (PWM_STEREO_STATIC | BIT(5) | BIT(13))
#define PWM_DMA_REQUEST (BIT(31) | (7 << 8) | 7)

struct direct_jack {
	void __iomem *regs;
	struct clk *clock;
	struct dma_chan *tx;
	struct device *dma_dev;
	u32 *samples;
	dma_addr_t dma_addr;
	dma_cookie_t cookie;
	snd_pcm_uframes_t appl_ptr;
	unsigned int pwm_repeat;
	u32 left_fraction;
	u32 right_fraction;
	bool running;
	bool biased;
};

static const struct snd_pcm_hardware direct_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_NO_REWINDS,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_88200,
	.rate_min = BASE_SAMPLE_RATE,
	.rate_max = 2 * BASE_SAMPLE_RATE,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = MAX_PCM_FRAMES * 4,
	.period_bytes_min = 32 * 4,
	.period_bytes_max = 1024 * 4,
	.periods_min = 2,
	.periods_max = 32,
	.fifo_size = 0,
};

static int direct_open(struct snd_pcm_substream *substream)
{
	substream->runtime->hw = direct_hw;
	return snd_pcm_hw_constraint_integer(substream->runtime,
					     SNDRV_PCM_HW_PARAM_PERIODS);
}

static void direct_period(void *context)
{
	struct snd_pcm_substream *substream = context;
	struct direct_jack *jack = snd_pcm_substream_chip(substream);

	if (READ_ONCE(jack->running))
		snd_pcm_period_elapsed(substream);
}

static void direct_set_bias(struct direct_jack *jack, u32 duty)
{
	writel(duty, jack->regs + PWM_DAT1);
	writel(duty, jack->regs + PWM_DAT2);
}

/* Only the device lifecycle changes the DC bias. PCM stop and XRUN keep
 * both PWM channels transmitting the silent duty with no DMA or FIFO.
 */
static void direct_ramp_bias(struct direct_jack *jack, bool up)
{
	unsigned int n;

	for (n = 1; n <= BIAS_RAMP_STEPS; n++) {
		unsigned int step = up ? n : BIAS_RAMP_STEPS - n;
		unsigned int weight = step * step * (3 * BIAS_RAMP_STEPS - 2 * step);
		u32 duty = DIV_ROUND_CLOSEST(PWM_MIDPOINT * weight,
				BIAS_RAMP_STEPS * BIAS_RAMP_STEPS * BIAS_RAMP_STEPS);

		direct_set_bias(jack, duty);
		usleep_range(800, 1200);
	}
}

static void direct_idle(struct direct_jack *jack)
{
	WRITE_ONCE(jack->running, false);
	writel(PWM_STEREO_STATIC, jack->regs + PWM_CTL);
	writel(0, jack->regs + PWM_DMAC);
}

static void direct_remove_bias(struct direct_jack *jack)
{
	if (!jack->biased)
		return;
	direct_idle(jack);
	direct_ramp_bias(jack, false);
	writel(0, jack->regs + PWM_CTL);
	jack->biased = false;
}

static int direct_prepare(struct snd_pcm_substream *substream)
{
	struct direct_jack *jack = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *rt = substream->runtime;
	struct dma_async_tx_descriptor *descriptor;
	size_t frame_bytes;
	size_t i;

	dmaengine_terminate_sync(jack->tx);
	/* Keep the PWM carrier fixed by repeating 44.1 kHz frames twice as often. */
	jack->pwm_repeat = PWM_MAX_REPEAT * BASE_SAMPLE_RATE / rt->rate;
	frame_bytes = 2 * jack->pwm_repeat * sizeof(*jack->samples);
	for (i = 0; i < rt->buffer_size * 2 * jack->pwm_repeat; i++)
		jack->samples[i] = PWM_MIDPOINT;
	/* ALSA resets hw_ptr and appl_ptr after this callback returns. */
	jack->appl_ptr = 0;
	jack->left_fraction = 0;
	jack->right_fraction = 0;
	descriptor = dmaengine_prep_dma_cyclic(jack->tx, jack->dma_addr,
			rt->buffer_size * frame_bytes,
			rt->period_size * frame_bytes, DMA_MEM_TO_DEV,
			DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!descriptor)
		return -ENOMEM;
	descriptor->callback = direct_period;
	descriptor->callback_param = substream;
	jack->cookie = dmaengine_submit(descriptor);
	return dma_submit_error(jack->cookie);
}

static u32 pwm_duty(u32 target, u32 *fraction)
{
	u32 total = target + *fraction;

	*fraction = total & PWM_FRACTION_MASK;
	return total >> PWM_FRACTION_BITS;
}

static int direct_ack(struct snd_pcm_substream *substream)
{
	struct direct_jack *jack = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *rt = substream->runtime;
	snd_pcm_uframes_t next = READ_ONCE(rt->control->appl_ptr);
	snd_pcm_uframes_t count = next >= jack->appl_ptr ?
		next - jack->appl_ptr : rt->boundary - jack->appl_ptr + next;
	snd_pcm_uframes_t frame = jack->appl_ptr % rt->buffer_size;
	const __le16 *source = (const __le16 *)rt->dma_area;

	if (count > rt->buffer_size)
		return -EINVAL;
	while (count--) {
		s32 left = (s16)le16_to_cpu(source[frame * 2]);
		s32 right = (s16)le16_to_cpu(source[frame * 2 + 1]);
		/* Preserve fractional duty across carrier periods without adding idle dither. */
		const s32 midpoint = (PWM_RANGE / 2) << PWM_FRACTION_BITS;
		u32 target_left = midpoint + left * (PWM_RANGE / 2 - 4);
		u32 target_right = midpoint + right * (PWM_RANGE / 2 - 4);
		size_t out = frame * 2 * jack->pwm_repeat;
		int phase;

		for (phase = 0; phase < jack->pwm_repeat; phase++) {
			/* GPIO40 carries the right channel and GPIO41 the left. */
			jack->samples[out++] =
				pwm_duty(target_right, &jack->right_fraction);
			jack->samples[out++] =
				pwm_duty(target_left, &jack->left_fraction);
		}
		if (++frame == rt->buffer_size)
			frame = 0;
	}
	dma_wmb();
	jack->appl_ptr = next;
	return 0;
}

static int direct_trigger(struct snd_pcm_substream *substream, int command)
{
	struct direct_jack *jack = snd_pcm_substream_chip(substream);

	switch (command) {
	case SNDRV_PCM_TRIGGER_START:
		/* Keep PWEN and the static midpoint while clearing the shared
		 * FIFO. Seed one stereo carrier cycle until DMA delivers PCM.
		 * These two words are outside the ALSA DMA ring.
		 */
		writel(PWM_STEREO_STATIC | PWM_CLR_FIFO, jack->regs + PWM_CTL);
		writel(PWM_FIFO_ERRORS, jack->regs + PWM_STA);
		writel(PWM_MIDPOINT, jack->regs + PWM_FIFO);
		writel(PWM_MIDPOINT, jack->regs + PWM_FIFO);
		writel(PWM_DMA_REQUEST, jack->regs + PWM_DMAC);
		WRITE_ONCE(jack->running, true);
		dma_async_issue_pending(jack->tx);
		writel(PWM_STEREO_FIFO, jack->regs + PWM_CTL);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		direct_idle(jack);
		return dmaengine_terminate_async(jack->tx);
	default:
		return -EINVAL;
	}
}

static int direct_sync_stop(struct snd_pcm_substream *substream)
{
	struct direct_jack *jack = snd_pcm_substream_chip(substream);

	dmaengine_synchronize(jack->tx);
	return 0;
}

static int direct_close(struct snd_pcm_substream *substream)
{
	struct direct_jack *jack = snd_pcm_substream_chip(substream);

	return dmaengine_terminate_sync(jack->tx);
}

static snd_pcm_uframes_t direct_pointer(struct snd_pcm_substream *substream)
{
	struct direct_jack *jack = snd_pcm_substream_chip(substream);
	size_t frame_bytes = 2 * jack->pwm_repeat * sizeof(*jack->samples);
	size_t bytes = substream->runtime->buffer_size * frame_bytes;
	struct dma_tx_state state = {};
	enum dma_status status;

	if (!READ_ONCE(jack->running))
		return 0;
	status = dmaengine_tx_status(jack->tx, jack->cookie, &state);
	if (status == DMA_ERROR || state.residue > bytes)
		return SNDRV_PCM_POS_XRUN;
	return (bytes - state.residue) % bytes / frame_bytes;
}

static const struct snd_pcm_ops direct_ops = {
	.open = direct_open,
	.close = direct_close,
	.prepare = direct_prepare,
	.trigger = direct_trigger,
	.sync_stop = direct_sync_stop,
	.pointer = direct_pointer,
	.ack = direct_ack,
};

static void direct_release_dma(void *context)
{
	struct direct_jack *jack = context;

	dmaengine_terminate_sync(jack->tx);
	if (jack->samples)
		dma_free_coherent(jack->dma_dev, MAX_PCM_FRAMES * DMA_MAX_FRAME_BYTES,
				  jack->samples, jack->dma_addr);
	dma_release_channel(jack->tx);
}

static void direct_release_clock(void *context)
{
	struct direct_jack *jack = context;

	direct_remove_bias(jack);
	clk_disable_unprepare(jack->clock);
	clk_rate_exclusive_put(jack->clock);
}

static int direct_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dma_slave_config config = {};
	struct resource *resource;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct direct_jack *jack;
	unsigned long pwm_clock;
	int result;

	if (!of_machine_is_compatible("raspberrypi,3-model-b") ||
	    !of_property_read_bool(dev->of_node, "arrangerlab,firmware-audio-disabled"))
		return -ENODEV;
	result = snd_devm_card_new(dev, -1, "DirectJack", THIS_MODULE,
				   sizeof(*jack), &card);
	if (result)
		return result;
	jack = card->private_data;
	jack->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &resource);
	if (IS_ERR(jack->regs))
		return PTR_ERR(jack->regs);
	if (readl(jack->regs + PWM_CTL) & (BIT(0) | BIT(8)))
		return dev_err_probe(dev, -EBUSY, "PWM channels already active\n");
	jack->clock = devm_clk_get(dev, NULL);
	if (IS_ERR(jack->clock))
		return PTR_ERR(jack->clock);
	result = clk_set_rate_exclusive(jack->clock, PWM_CLOCK);
	if (result)
		return dev_err_probe(dev, result, "PWM clock rate unavailable\n");
	result = clk_prepare_enable(jack->clock);
	if (result) {
		clk_rate_exclusive_put(jack->clock);
		return result;
	}
	result = devm_add_action_or_reset(dev, direct_release_clock, jack);
	if (result)
		return result;
	pwm_clock = clk_get_rate(jack->clock);
	if (pwm_clock < PWM_CLOCK - PWM_CLOCK / 1000 ||
	    pwm_clock > PWM_CLOCK + PWM_CLOCK / 1000)
		return dev_err_probe(dev, -ERANGE, "PWM clock %lu differs from %u\n",
				     pwm_clock, PWM_CLOCK);
	jack->tx = dma_request_chan(dev, "tx");
	if (IS_ERR(jack->tx))
		return dev_err_probe(dev, PTR_ERR(jack->tx), "PWM DMA unavailable\n");
	result = devm_add_action_or_reset(dev, direct_release_dma, jack);
	if (result)
		return result;
	jack->dma_dev = dmaengine_get_dma_device(jack->tx);
	config.dst_addr = resource->start + PWM_FIFO;
	config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	config.dst_maxburst = 1;
	result = dmaengine_slave_config(jack->tx, &config);
	if (result)
		return result;
	jack->samples = dma_alloc_coherent(jack->dma_dev,
				MAX_PCM_FRAMES * DMA_MAX_FRAME_BYTES, &jack->dma_addr,
				GFP_KERNEL);
	if (!jack->samples)
		return -ENOMEM;
	result = snd_pcm_new(card, "Direct PWM Jack", 0, 1, 0, &pcm);
	if (result)
		return result;
	pcm->private_data = jack;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &direct_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
				      MAX_PCM_FRAMES * 4, MAX_PCM_FRAMES * 4);
	strscpy(card->driver, "DirectJack", sizeof(card->driver));
	strscpy(card->shortname, "Direct PWM Jack", sizeof(card->shortname));
	strscpy(card->longname, "Raspberry Pi 3 direct PWM DMA jack", sizeof(card->longname));
	/* Acquire the carrier only after the clock, DMA and PCM are ready;
	 * earlier probe failures must leave the pins as they were.
	 */
	writel(0, jack->regs + PWM_DMAC);
	writel(PWM_RANGE, jack->regs + PWM_RNG1);
	writel(PWM_RANGE, jack->regs + PWM_RNG2);
	direct_set_bias(jack, 0);
	writel(PWM_STEREO_STATIC, jack->regs + PWM_CTL);
	jack->biased = true;
	direct_ramp_bias(jack, true);
	platform_set_drvdata(pdev, card);
	dev_info(dev, "PWM clock=%lu Hz, PCM rates 44100/88200 Hz, DREQ-backed cyclic DMA\n",
		 pwm_clock);
	return snd_card_register(card);
}

static void direct_shutdown(struct platform_device *pdev)
{
	struct snd_card *card = platform_get_drvdata(pdev);
	struct direct_jack *jack = card->private_data;

	/* Disconnect stops PCM under the stream lock and prevents a restart
	 * while the carrier ramps down for a normal reboot or poweroff.
	 */
	snd_card_disconnect(card);
	dmaengine_terminate_sync(jack->tx);
	direct_remove_bias(jack);
}

static const struct of_device_id direct_ids[] = {
	{ .compatible = "arrangerlab,bcm2835-direct-jack" },
	{}
};
MODULE_DEVICE_TABLE(of, direct_ids);

static struct platform_driver direct_driver = {
	.probe = direct_probe,
	.shutdown = direct_shutdown,
	.driver = {
		.name = "arrangerlab-direct-jack",
		.of_match_table = direct_ids,
	},
};
module_platform_driver(direct_driver);
MODULE_DESCRIPTION("ArrangerLab Pi 3 direct PWM DMA jack prototype");
MODULE_LICENSE("GPL");
