// SPDX-License-Identifier: GPL-2.0
//
// HDA DSP ALSA Control Driver
//
// Copyright 2022 Cirrus Logic, Inc.
//
// Author: Stefan Binding <sbinding@opensource.cirrus.com>

#include <linux/module.h>
#include <sound/soc.h>
#include <linux/firmware/cirrus/cs_dsp.h>
#include <linux/firmware/cirrus/wmfw.h>
#include "hda_cs_dsp_ctl.h"

#define ADSP_MAX_STD_CTRL_SIZE               512

struct hda_cs_dsp_coeff_ctl {
	struct cs_dsp_coeff_ctl *cs_ctl;
	struct snd_card *card;
	struct snd_kcontrol *kctl;
};

static const char * const hda_cs_dsp_fw_text[HDA_CS_DSP_NUM_FW] = {
	[HDA_CS_DSP_FW_SPK_PROT] = "Prot",
	[HDA_CS_DSP_FW_SPK_CALI] = "Cali",
	[HDA_CS_DSP_FW_SPK_DIAG] = "Diag",
	[HDA_CS_DSP_FW_MISC] =     "Misc",
};

const char * const hda_cs_dsp_fw_ids[HDA_CS_DSP_NUM_FW] = {
	[HDA_CS_DSP_FW_SPK_PROT] = "spk-prot",
	[HDA_CS_DSP_FW_SPK_CALI] = "spk-cali",
	[HDA_CS_DSP_FW_SPK_DIAG] = "spk-diag",
	[HDA_CS_DSP_FW_MISC] =     "misc",
};
EXPORT_SYMBOL_GPL(hda_cs_dsp_fw_ids);

static int hda_cs_dsp_coeff_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	struct hda_cs_dsp_coeff_ctl *ctl = (struct hda_cs_dsp_coeff_ctl *)snd_kcontrol_chip(kctl);
	struct cs_dsp_coeff_ctl *cs_ctl = ctl->cs_ctl;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = cs_ctl->len;

	return 0;
}

static int hda_cs_dsp_coeff_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_cs_dsp_coeff_ctl *ctl = (struct hda_cs_dsp_coeff_ctl *)snd_kcontrol_chip(kctl);
	struct cs_dsp_coeff_ctl *cs_ctl = ctl->cs_ctl;
	char *p = ucontrol->value.bytes.data;
	int ret = 0;

	mutex_lock(&cs_ctl->dsp->pwr_lock);
	ret = cs_dsp_coeff_write_ctrl(cs_ctl, 0, p, cs_ctl->len);
	mutex_unlock(&cs_ctl->dsp->pwr_lock);

	return ret;
}

static int hda_cs_dsp_coeff_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_cs_dsp_coeff_ctl *ctl = (struct hda_cs_dsp_coeff_ctl *)snd_kcontrol_chip(kctl);
	struct cs_dsp_coeff_ctl *cs_ctl = ctl->cs_ctl;
	char *p = ucontrol->value.bytes.data;
	int ret;

	mutex_lock(&cs_ctl->dsp->pwr_lock);
	ret = cs_dsp_coeff_read_ctrl(cs_ctl, 0, p, cs_ctl->len);
	mutex_unlock(&cs_ctl->dsp->pwr_lock);

	return ret;
}

static unsigned int wmfw_convert_flags(unsigned int in)
{
	unsigned int out, rd, wr, vol;

	rd = SNDRV_CTL_ELEM_ACCESS_READ;
	wr = SNDRV_CTL_ELEM_ACCESS_WRITE;
	vol = SNDRV_CTL_ELEM_ACCESS_VOLATILE;

	out = 0;

	if (in) {
		out |= rd;
		if (in & WMFW_CTL_FLAG_WRITEABLE)
			out |= wr;
		if (in & WMFW_CTL_FLAG_VOLATILE)
			out |= vol;
	} else {
		out |= rd | wr | vol;
	}

	return out;
}

static int hda_cs_dsp_add_kcontrol(struct hda_cs_dsp_coeff_ctl *ctl, const char *name)
{
	struct cs_dsp_coeff_ctl *cs_ctl = ctl->cs_ctl;
	struct snd_kcontrol_new kcontrol = {0};
	struct snd_kcontrol *kctl;
	int ret = 0;

	if (cs_ctl->len > ADSP_MAX_STD_CTRL_SIZE) {
		dev_err(cs_ctl->dsp->dev, "KControl %s: length %zu exceeds maximum %d\n", name,
			cs_ctl->len, ADSP_MAX_STD_CTRL_SIZE);
		return -EINVAL;
	}

	kcontrol.name = name;
	kcontrol.info = hda_cs_dsp_coeff_info;
	kcontrol.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	kcontrol.access = wmfw_convert_flags(cs_ctl->flags);
	kcontrol.get = hda_cs_dsp_coeff_get;
	kcontrol.put = hda_cs_dsp_coeff_put;

	/* Save ctl inside private_data, ctl is owned by cs_dsp,
	 * and will be freed when cs_dsp removes the control */
	kctl = snd_ctl_new1(&kcontrol, (void *)ctl);
	if (!kctl) {
		ret = -ENOMEM;
		return ret;
	}

	ret = snd_ctl_add(ctl->card, kctl);
	if (ret) {
		dev_err(cs_ctl->dsp->dev, "Failed to add KControl %s = %d\n", kcontrol.name, ret);
		return ret;
	}

	dev_dbg(cs_ctl->dsp->dev, "Added KControl: %s\n", kcontrol.name);
	ctl->kctl = kctl;

	return 0;
}

int hda_cs_dsp_control_add(struct cs_dsp_coeff_ctl *cs_ctl, struct hda_cs_dsp_ctl_info *info)
{
	struct cs_dsp *cs_dsp = cs_ctl->dsp;
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	struct hda_cs_dsp_coeff_ctl *ctl;
	const char *region_name;
	int ret;

	if (cs_ctl->flags & WMFW_CTL_FLAG_SYS)
		return 0;

	region_name = cs_dsp_mem_region_name(cs_ctl->alg_region.type);
	if (!region_name) {
		dev_err(cs_dsp->dev, "Unknown region type: %d\n", cs_ctl->alg_region.type);
		return -EINVAL;
	}

	ret = scnprintf(name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s %s %.12s %x", info->device_name,
			cs_dsp->name, hda_cs_dsp_fw_text[info->fw_type], cs_ctl->alg_region.alg);

	if (cs_ctl->subname) {
		int avail = SNDRV_CTL_ELEM_ID_NAME_MAXLEN - ret - 2;
		int skip = 0;

		/* Truncate the subname from the start if it is too long */
		if (cs_ctl->subname_len > avail)
			skip = cs_ctl->subname_len - avail;

		snprintf(name + ret, SNDRV_CTL_ELEM_ID_NAME_MAXLEN - ret,
			 " %.*s", cs_ctl->subname_len - skip, cs_ctl->subname + skip);
	}

	ctl = kzalloc(sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return -ENOMEM;

	ctl->cs_ctl = cs_ctl;
	ctl->card = info->card;
	cs_ctl->priv = ctl;

	ret = hda_cs_dsp_add_kcontrol(ctl, name);
	if (ret) {
		dev_err(cs_dsp->dev, "Error (%d) adding control %s\n", ret, name);
		kfree(ctl);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(hda_cs_dsp_control_add);

void hda_cs_dsp_control_remove(struct cs_dsp_coeff_ctl *cs_ctl)
{
	struct hda_cs_dsp_coeff_ctl *ctl = cs_ctl->priv;

	kfree(ctl);
}
EXPORT_SYMBOL_GPL(hda_cs_dsp_control_remove);

int hda_cs_dsp_write_ctl(struct cs_dsp *dsp, const char *name, int type,
			 unsigned int alg, const void *buf, size_t len)
{
	struct cs_dsp_coeff_ctl *cs_ctl;
	struct hda_cs_dsp_coeff_ctl *ctl;
	int ret;

	cs_ctl = cs_dsp_get_ctl(dsp, name, type, alg);
	if (!cs_ctl)
		return -EINVAL;

	ctl = cs_ctl->priv;

	ret = cs_dsp_coeff_write_ctrl(cs_ctl, 0, buf, len);
	if (ret)
		return ret;

	if (cs_ctl->flags & WMFW_CTL_FLAG_SYS)
		return 0;

	snd_ctl_notify(ctl->card, SNDRV_CTL_EVENT_MASK_VALUE, &ctl->kctl->id);

	return 0;
}
EXPORT_SYMBOL_GPL(hda_cs_dsp_write_ctl);

int hda_cs_dsp_read_ctl(struct cs_dsp *dsp, const char *name, int type,
			unsigned int alg, void *buf, size_t len)
{
	struct cs_dsp_coeff_ctl *cs_ctl;

	cs_ctl = cs_dsp_get_ctl(dsp, name, type, alg);
	if (!cs_ctl)
		return -EINVAL;

	return cs_dsp_coeff_read_ctrl(cs_ctl, 0, buf, len);
}
EXPORT_SYMBOL_GPL(hda_cs_dsp_read_ctl);

MODULE_DESCRIPTION("CS_DSP ALSA Control HDA Library");
MODULE_AUTHOR("Stefan Binding, <sbinding@opensource.cirrus.com>");
MODULE_LICENSE("GPL");
