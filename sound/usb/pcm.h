/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __USBAUDIO_PCM_H
#define __USBAUDIO_PCM_H

void snd_usb_set_pcm_ops(struct snd_pcm *pcm, int stream);
int snd_usb_pcm_suspend(struct snd_usb_stream *as);
int snd_usb_pcm_resume(struct snd_usb_stream *as);

bool snd_usb_pcm_has_fixed_rate(struct snd_usb_substream *as);

int snd_usb_init_pitch(struct snd_usb_audio *chip,
		       const struct audioformat *fmt);
void snd_usb_preallocate_buffer(struct snd_usb_substream *subs);

int snd_usb_audioformat_set_sync_ep(struct snd_usb_audio *chip,
				    struct audioformat *fmt);

#endif /* __USBAUDIO_PCM_H */
