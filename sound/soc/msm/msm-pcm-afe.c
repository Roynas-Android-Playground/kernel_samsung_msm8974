/* Copyright (c) 2011-2012, 2017, The Linux Foundation. All rights reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 and
* only version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/


#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>

#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <sound/q6adm.h>
#include <asm/dma.h>
#include <linux/memory_alloc.h>
#include "msm-pcm-afe.h"
#include "msm-pcm-q6.h"


#define MIN_PERIOD_SIZE (128 * 2)
#define MAX_PERIOD_SIZE (128 * 2 * 2 * 6)
#define MIN_NUM_PERIODS 4
#define MAX_NUM_PERIODS 768

static struct snd_pcm_hardware msm_afe_hardware = {
	.info =			(SNDRV_PCM_INFO_MMAP |
				SNDRV_PCM_INFO_BLOCK_TRANSFER |
				SNDRV_PCM_INFO_MMAP_VALID |
				SNDRV_PCM_INFO_INTERLEAVED),
	.formats =              SNDRV_PCM_FMTBIT_S16_LE,
	.rates =                (SNDRV_PCM_RATE_8000 |
				SNDRV_PCM_RATE_16000 |
				SNDRV_PCM_RATE_48000),
	.rate_min =             8000,
	.rate_max =             48000,
	.channels_min =         1,
	.channels_max =         2,
	.buffer_bytes_max =     MAX_PERIOD_SIZE * MAX_NUM_PERIODS,
	.period_bytes_min =     MIN_PERIOD_SIZE,
	.period_bytes_max =     MAX_PERIOD_SIZE,
	.periods_min =          MIN_NUM_PERIODS,
	.periods_max =          MAX_NUM_PERIODS,
	.fifo_size =            0,
};
static enum hrtimer_restart afe_hrtimer_callback(struct hrtimer *hrt);
static enum hrtimer_restart afe_hrtimer_rec_callback(struct hrtimer *hrt);

static void q6asm_event_handler(uint32_t opcode,
		uint32_t token, uint32_t *payload, void *priv)
{
}
static enum hrtimer_restart afe_hrtimer_callback(struct hrtimer *hrt)
{
	struct pcm_afe_info *prtd =
		container_of(hrt, struct pcm_afe_info, hrt);
	struct snd_pcm_substream *substream = prtd->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	if (prtd->start) {
		pr_debug("sending frame to DSP: poll_time: %d\n",
				prtd->poll_time);
		if (prtd->dsp_cnt == runtime->periods)
			prtd->dsp_cnt = 0;
		afe_rt_proxy_port_write(
				(prtd->dma_addr +
				(prtd->dsp_cnt *
				snd_pcm_lib_period_bytes(prtd->substream))),
				snd_pcm_lib_period_bytes(prtd->substream));
		prtd->dsp_cnt++;
		hrtimer_forward_now(hrt, ns_to_ktime(prtd->poll_time
					* 1000));

		return HRTIMER_RESTART;
	} else
		return HRTIMER_NORESTART;
}
static enum hrtimer_restart afe_hrtimer_rec_callback(struct hrtimer *hrt)
{
	struct pcm_afe_info *prtd =
		container_of(hrt, struct pcm_afe_info, hrt);
	struct snd_pcm_substream *substream = prtd->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	if (prtd->start) {
		if (prtd->dsp_cnt == runtime->periods)
			prtd->dsp_cnt = 0;
		afe_rt_proxy_port_read(
			(prtd->dma_addr + (prtd->dsp_cnt
			* snd_pcm_lib_period_bytes(prtd->substream))),
			snd_pcm_lib_period_bytes(prtd->substream));
		prtd->dsp_cnt++;
		pr_debug("sending frame rec to DSP: poll_time: %d\n",
				prtd->poll_time);
		hrtimer_forward_now(hrt, ns_to_ktime(prtd->poll_time
				* 1000));

		return HRTIMER_RESTART;
	} else
		return HRTIMER_NORESTART;
}
static void pcm_afe_process_tx_pkt(uint32_t opcode,
		uint32_t token, uint32_t *payload,
		 void *priv)
{
	struct pcm_afe_info *prtd = priv;
	unsigned long dsp_flags;
	struct snd_pcm_substream *substream = NULL;
	struct snd_pcm_runtime *runtime = NULL;
	uint16_t event;

	if (prtd == NULL)
		return;
	substream =  prtd->substream;
	runtime = substream->runtime;
	pr_debug("%s\n", __func__);
	spin_lock_irqsave(&prtd->dsp_lock, dsp_flags);
	switch (opcode) {
	case AFE_EVENT_RT_PROXY_PORT_STATUS: {
		event = (uint16_t)((0xFFFF0000 & payload[0]) >> 0x10);
			switch (event) {
			case AFE_EVENT_RTPORT_START: {
				prtd->dsp_cnt = 0;
				prtd->poll_time = ((unsigned long)((
						snd_pcm_lib_period_bytes
						(prtd->substream) *
						1000 * 1000)/
						(runtime->rate *
						runtime->channels * 2)));
				pr_debug("prtd->poll_time: %d",
						prtd->poll_time);
				break;
			}
			case AFE_EVENT_RTPORT_STOP:
				pr_debug("%s: event!=0\n", __func__);
				prtd->start = 0;
				snd_pcm_stop(substream, SNDRV_PCM_STATE_SETUP);
				break;
			case AFE_EVENT_RTPORT_LOW_WM:
				pr_debug("%s: Underrun\n", __func__);
				break;
			case AFE_EVENT_RTPORT_HI_WM:
				pr_debug("%s: Overrun\n", __func__);
				break;
			default:
				break;
			}
			break;
	}
	case APR_BASIC_RSP_RESULT: {
		switch (payload[0]) {
		case AFE_SERVICE_CMD_RTPORT_WR:
			pr_debug("write done\n");
			prtd->pcm_irq_pos += snd_pcm_lib_period_bytes
							(prtd->substream);
			snd_pcm_period_elapsed(prtd->substream);
			break;
		default:
			break;
		}
		break;
	}
	default:
		break;
	}
	spin_unlock_irqrestore(&prtd->dsp_lock, dsp_flags);
}

static void pcm_afe_process_rx_pkt(uint32_t opcode,
		uint32_t token, uint32_t *payload,
		 void *priv)
{
	struct pcm_afe_info *prtd = priv;
	unsigned long dsp_flags;
	struct snd_pcm_substream *substream = NULL;
	struct snd_pcm_runtime *runtime = NULL;
	uint16_t event;

	if (prtd == NULL)
		return;
	substream =  prtd->substream;
	runtime = substream->runtime;
	pr_debug("%s\n", __func__);
	spin_lock_irqsave(&prtd->dsp_lock, dsp_flags);
	switch (opcode) {
	case AFE_EVENT_RT_PROXY_PORT_STATUS: {
		event = (uint16_t)((0xFFFF0000 & payload[0]) >> 0x10);
		switch (event) {
		case AFE_EVENT_RTPORT_START: {
			prtd->dsp_cnt = 0;
			prtd->poll_time = ((unsigned long)((
				snd_pcm_lib_period_bytes(prtd->substream)
					* 1000 * 1000)/(runtime->rate
					* runtime->channels * 2)));
			pr_debug("prtd->poll_time : %d", prtd->poll_time);
			break;
		}
		case AFE_EVENT_RTPORT_STOP:
			pr_debug("%s: event!=0\n", __func__);
			prtd->start = 0;
			snd_pcm_stop(substream, SNDRV_PCM_STATE_SETUP);
			break;
		case AFE_EVENT_RTPORT_LOW_WM:
			pr_debug("%s: Underrun\n", __func__);
			break;
		case AFE_EVENT_RTPORT_HI_WM:
			pr_debug("%s: Overrun\n", __func__);
			break;
		default:
			break;
		}
		break;
	}
	case APR_BASIC_RSP_RESULT: {
		switch (payload[0]) {
		case AFE_SERVICE_CMD_RTPORT_RD:
			pr_debug("Read done\n");
			prtd->pcm_irq_pos += snd_pcm_lib_period_bytes
							(prtd->substream);
			snd_pcm_period_elapsed(prtd->substream);
			break;
		default:
			break;
		}
		break;
	}
	default:
		break;
	}
	spin_unlock_irqrestore(&prtd->dsp_lock, dsp_flags);
}

static int msm_afe_playback_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_afe_info *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *dai = rtd->cpu_dai;
	int ret = 0;

	pr_debug("%s: sample_rate=%d\n", __func__, runtime->rate);

	pr_debug("%s: dai->id =%x\n", __func__, dai->id);
	ret = afe_register_get_events(dai->id,
			pcm_afe_process_tx_pkt, prtd);
	if (ret < 0) {
		pr_err("afe-pcm:register for events failed\n");
		return ret;
	}
	pr_debug("%s:success\n", __func__);
	prtd->prepared++;
	return ret;
}

static int msm_afe_capture_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_afe_info *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *dai = rtd->cpu_dai;
	int ret = 0;

	pr_debug("%s\n", __func__);

	pr_debug("%s: dai->id =%x\n", __func__, dai->id);
	ret = afe_register_get_events(dai->id,
			pcm_afe_process_rx_pkt, prtd);
	if (ret < 0) {
		pr_err("afe-pcm:register for events failed\n");
		return ret;
	}
	pr_debug("%s:success\n", __func__);
	prtd->prepared++;
	return 0;
}

/* Conventional and unconventional sample rate supported */
static unsigned int supported_sample_rates[] = {
	8000, 16000, 48000
};

static struct snd_pcm_hw_constraint_list constraints_sample_rates = {
	.count = ARRAY_SIZE(supported_sample_rates),
	.list = supported_sample_rates,
	.mask = 0,
};

static int msm_afe_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_afe_info *prtd = NULL;
	int ret = 0;

	prtd = kzalloc(sizeof(struct pcm_afe_info), GFP_KERNEL);
	if (prtd == NULL) {
		pr_err("Failed to allocate memory for msm_audio\n");
		return -ENOMEM;
	} else
		pr_debug("prtd %x\n", (unsigned int)prtd);

	mutex_init(&prtd->lock);
	spin_lock_init(&prtd->dsp_lock);
	prtd->dsp_cnt = 0;

	mutex_lock(&prtd->lock);

	runtime->hw = msm_afe_hardware;
	prtd->substream = substream;
	runtime->private_data = prtd;
	prtd->audio_client = q6asm_audio_client_alloc(
				(app_cb)q6asm_event_handler, prtd);
	if (!prtd->audio_client) {
		pr_debug("%s: Could not allocate memory\n", __func__);
		mutex_unlock(&prtd->lock);
		kfree(prtd);
		return -ENOMEM;
	}
	hrtimer_init(&prtd->hrt, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		prtd->hrt.function = afe_hrtimer_callback;
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		prtd->hrt.function = afe_hrtimer_rec_callback;

	mutex_unlock(&prtd->lock);

	ret = snd_pcm_hw_constraint_list(runtime, 0,
				SNDRV_PCM_HW_PARAM_RATE,
				&constraints_sample_rates);
	if (ret < 0)
		pr_err("snd_pcm_hw_constraint_list failed\n");
	/* Ensure that buffer size is a multiple of period size */
	ret = snd_pcm_hw_constraint_integer(runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		pr_err("snd_pcm_hw_constraint_integer failed\n");

	return 0;
}

static int msm_afe_close(struct snd_pcm_substream *substream)
{
	int rc = 0;
	struct snd_dma_buffer *dma_buf;
	struct snd_pcm_runtime *runtime;
	struct pcm_afe_info *prtd;
	struct snd_soc_pcm_runtime *rtd = NULL;
	struct snd_soc_dai *dai = NULL;
	int dir = IN;
	int ret = 0;

	pr_debug("%s\n", __func__);
	if (substream == NULL) {
		pr_err("substream is NULL\n");
		return -EINVAL;
	}
	rtd = substream->private_data;
	dai = rtd->cpu_dai;
	runtime = substream->runtime;
	prtd = runtime->private_data;

	mutex_lock(&prtd->lock);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		dir = IN;
		ret =  afe_unregister_get_events(dai->id);
		if (ret < 0)
			pr_err("AFE unregister for events failed\n");
	} else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		dir = OUT;
		ret =  afe_unregister_get_events(dai->id);
		if (ret < 0)
			pr_err("AFE unregister for events failed\n");
	}
	hrtimer_cancel(&prtd->hrt);

	rc = afe_cmd_memory_unmap(runtime->dma_addr);
	if (rc < 0)
		pr_err("AFE memory unmap failed\n");

	pr_debug("release all buffer\n");
	dma_buf = &substream->dma_buffer;
	if (dma_buf == NULL) {
		pr_debug("dma_buf is NULL\n");
			goto done;
	}

	if (dma_buf->area) {
			dma_buf->area = NULL;
		}

	q6asm_audio_client_buf_free_contiguous(dir,
				prtd->audio_client);
done:
	pr_debug("%s: dai->id =%x\n", __func__, dai->id);
	q6asm_audio_client_free(prtd->audio_client);
	mutex_unlock(&prtd->lock);
	prtd->prepared--;
	kfree(prtd);
	runtime->private_data = NULL;

	return 0;
}
static int msm_afe_prepare(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_afe_info *prtd = runtime->private_data;

	prtd->pcm_irq_pos = 0;
	if (prtd->prepared)
		return 0;
	mutex_lock(&prtd->lock);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = msm_afe_playback_prepare(substream);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = msm_afe_capture_prepare(substream);
	mutex_unlock(&prtd->lock);
	return ret;
}
static int msm_afe_mmap(struct snd_pcm_substream *substream,
				struct vm_area_struct *vma)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_afe_info *prtd = runtime->private_data;
	int result = 0;

	pr_debug("%s\n", __func__);
	prtd->mmap_flag = 1;
	if (runtime->dma_addr && runtime->dma_bytes) {
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		result = remap_pfn_range(vma, vma->vm_start,
				runtime->dma_addr >> PAGE_SHIFT,
				runtime->dma_bytes,
				vma->vm_page_prot);
	} else {
		pr_err("Physical address or size of buf is NULL");
		return -EINVAL;
	}
	return result;
}
static int msm_afe_trigger(struct snd_pcm_substream *substream, int cmd)
{
	int ret = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_afe_info *prtd = runtime->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		pr_debug("%s: SNDRV_PCM_TRIGGER_START\n", __func__);
		prtd->start = 1;
		hrtimer_start(&prtd->hrt, ns_to_ktime(0),
					HRTIMER_MODE_REL);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		pr_debug("%s: SNDRV_PCM_TRIGGER_STOP\n", __func__);
		prtd->start = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}
static int msm_afe_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dma_buffer *dma_buf = &substream->dma_buffer;
	struct pcm_afe_info *prtd = runtime->private_data;
	struct audio_buffer *buf;
	int dir, ret;

	pr_debug("%s:\n", __func__);

	mutex_lock(&prtd->lock);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		dir = IN;
	else
		dir = OUT;
	ret = q6asm_audio_client_buf_alloc_contiguous(dir,
			prtd->audio_client,
			runtime->hw.period_bytes_min,
			runtime->hw.periods_max);
	if (ret < 0) {
		pr_err("Audio Start: Buffer Allocation failed rc = %d\n", ret);
		mutex_unlock(&prtd->lock);
		return -ENOMEM;
	}
	buf = prtd->audio_client->port[dir].buf;

	if (buf == NULL || buf[0].data == NULL) {
		mutex_unlock(&prtd->lock);
		return -ENOMEM;
	}

	pr_debug("%s:buf = %p\n", __func__, buf);
	dma_buf->dev.type = SNDRV_DMA_TYPE_DEV;
	dma_buf->dev.dev = substream->pcm->card->dev;
	dma_buf->private_data = NULL;
	dma_buf->area = buf[0].data;
	dma_buf->addr =  buf[0].phys;
	dma_buf->bytes = runtime->hw.buffer_bytes_max;
	if (!dma_buf->area) {
		pr_err("%s:MSM AFE physical memory allocation failed\n",
							__func__);
		mutex_unlock(&prtd->lock);
		return -ENOMEM;
	}
	memset(dma_buf->area, 0, runtime->hw.buffer_bytes_max);
	prtd->dma_addr = (u32) dma_buf->addr;

	mutex_unlock(&prtd->lock);

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);

	ret = afe_cmd_memory_map(dma_buf->addr, dma_buf->bytes);
	if (ret < 0)
		pr_err("fail to map memory to DSP\n");

	return ret;
}
static snd_pcm_uframes_t msm_afe_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct pcm_afe_info *prtd = runtime->private_data;

	if (prtd->pcm_irq_pos >= snd_pcm_lib_buffer_bytes(substream))
		prtd->pcm_irq_pos = 0;

	pr_debug("pcm_irq_pos = %d\n", prtd->pcm_irq_pos);
	return bytes_to_frames(runtime, (prtd->pcm_irq_pos));
}

static struct snd_pcm_ops msm_afe_ops = {
	.open           = msm_afe_open,
	.hw_params	= msm_afe_hw_params,
	.trigger	= msm_afe_trigger,
	.close          = msm_afe_close,
	.prepare        = msm_afe_prepare,
	.mmap		= msm_afe_mmap,
	.pointer	= msm_afe_pointer,
};


static int msm_asoc_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;
	int ret = 0;

	pr_debug("%s\n", __func__);
	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);
	return ret;
}

static int msm_afe_afe_probe(struct snd_soc_platform *platform)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static struct snd_soc_platform_driver msm_soc_platform = {
	.ops		= &msm_afe_ops,
	.pcm_new	= msm_asoc_pcm_new,
	.probe		= msm_afe_afe_probe,
};

static __devinit int msm_afe_probe(struct platform_device *pdev)
{
	pr_debug("%s: dev name %s\n", __func__, dev_name(&pdev->dev));
	return snd_soc_register_platform(&pdev->dev,
				   &msm_soc_platform);
}

static int msm_afe_remove(struct platform_device *pdev)
{
	pr_debug("%s\n", __func__);
	snd_soc_unregister_platform(&pdev->dev);
	return 0;
}

static struct platform_driver msm_afe_driver = {
	.driver = {
		.name = "msm-pcm-afe",
		.owner = THIS_MODULE,
	},
	.probe = msm_afe_probe,
	.remove = __devexit_p(msm_afe_remove),
};

static int __init msm_soc_platform_init(void)
{
	pr_debug("%s\n", __func__);
	return platform_driver_register(&msm_afe_driver);
}
module_init(msm_soc_platform_init);

static void __exit msm_soc_platform_exit(void)
{
	pr_debug("%s\n", __func__);
	platform_driver_unregister(&msm_afe_driver);
}
module_exit(msm_soc_platform_exit);

MODULE_DESCRIPTION("AFE PCM module platform driver");
MODULE_LICENSE("GPL v2");
