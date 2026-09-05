/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / GIF decoder filter, based on giflib
 *  (https://sourceforge.net/projects/giflib/).
 *
 *  Every frame of the file is decoded, expanded from its palette and
 *  composited onto a persistent canvas following the disposal method of each
 *  frame, then sent as its own packet - a still GIF is simply the one-frame
 *  case. Timing follows the GIF unit of 1/100 s, which is the output timescale.
 *
 *  Output is RGB: an RGBA pid has no adaptation path to writegen in this build
 *  (see the note in test-player/libpng.js), so transparent pixels show what the
 *  canvas already held, which is what compositing does anyway.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <gif_lib.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_GIFDecCtx;

/* giflib reads through a callback; this walks a memory buffer instead of a FILE. */
typedef struct
{
	const u8 *data;
	u32 size;
	u32 pos;
} GIFMemReader;

static int gifdec_read(GifFileType *gif, GifByteType *buf, int len)
{
	GIFMemReader *rd = (GIFMemReader *)gif->UserData;
	u32 avail = rd->size - rd->pos;
	if ((u32)len > avail)
		len = (int)avail;
	if (len > 0)
	{
		memcpy(buf, rd->data + rd->pos, len);
		rd->pos += len;
	}
	return len;
}

static GF_Err gifdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_GIFDecCtx *ctx = (GF_GIFDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));
	/* GIF delays are expressed in hundredths of a second. */
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(100));

	return GF_OK;
}

static Bool gifdec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_GIFDecCtx *ctx = (GF_GIFDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

/* Delay, disposal and transparency live in a graphics control extension
 * attached to each frame, not in the palette. DGifSavedExtensionToGCB reads it;
 * when a frame carries none, the defaults below apply. */
static void gifdec_get_gcb(GifFileType *gif, int idx, GraphicsControlBlock *gcb)
{
	gcb->DisposalMode = DISPOSAL_UNSPECIFIED;
	gcb->UserInputFlag = 0;
	gcb->DelayTime = 0;
	gcb->TransparentColor = NO_TRANSPARENT_COLOR;
	DGifSavedExtensionToGCB(gif, idx, gcb);
}

static GF_Err gifdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	u8 *data;
	u32 size, w, h, canvas_size, f;
	int err = 0;
	GifFileType *gif;
	GIFMemReader rd;
	u8 *canvas = NULL, *saved = NULL;
	u64 cts = 0;
	GF_GIFDecCtx *ctx = (GF_GIFDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	rd.data = data;
	rd.size = size;
	rd.pos = 0;
	gif = DGifOpen(&rd, gifdec_read, &err);
	if (!gif)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[GIFDec] Not a valid GIF file (error %d)\n", err));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	if (DGifSlurp(gif) == GIF_ERROR || gif->ImageCount < 1)
	{
		DGifCloseFile(gif, &err);
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[GIFDec] Failed to decode GIF image\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	gf_filter_pid_drop_packet(ctx->ipid);

	w = (u32)gif->SWidth;
	h = (u32)gif->SHeight;
	canvas_size = w * h * 3;
	if (!w || !h)
	{
		DGifCloseFile(gif, &err);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	canvas = (u8 *)gf_malloc(canvas_size);
	saved = (u8 *)gf_malloc(canvas_size);
	if (!canvas || !saved)
	{
		if (canvas) gf_free(canvas);
		if (saved) gf_free(saved);
		DGifCloseFile(gif, &err);
		return GF_OUT_OF_MEM;
	}

	/* The canvas starts on the background colour, which is a palette index in
	 * the global colour map. */
	memset(canvas, 0, canvas_size);
	if (gif->SColorMap && (gif->SBackGroundColor < gif->SColorMap->ColorCount))
	{
		GifColorType *bg = &gif->SColorMap->Colors[gif->SBackGroundColor];
		u32 i;
		for (i = 0; i < w * h; i++)
		{
			canvas[i * 3] = bg->Red;
			canvas[i * 3 + 1] = bg->Green;
			canvas[i * 3 + 2] = bg->Blue;
		}
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(w * 3));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NB_FRAMES, &PROP_UINT((u32)gif->ImageCount));

	for (f = 0; f < (u32)gif->ImageCount; f++)
	{
		SavedImage *img = &gif->SavedImages[f];
		ColorMapObject *cmap = img->ImageDesc.ColorMap ? img->ImageDesc.ColorMap : gif->SColorMap;
		GraphicsControlBlock gcb;
		GF_FilterPacket *dst_pck;
		u8 *output;
		u32 x, y, delay;
		u32 fx = (u32)img->ImageDesc.Left;
		u32 fy = (u32)img->ImageDesc.Top;
		u32 fw = (u32)img->ImageDesc.Width;
		u32 fh = (u32)img->ImageDesc.Height;

		if (!cmap)
			continue;
		gifdec_get_gcb(gif, (int)f, &gcb);

		/* DISPOSE_PREVIOUS restores what was on the canvas before this frame,
		 * so it has to be kept before drawing. */
		if (gcb.DisposalMode == DISPOSE_PREVIOUS)
			memcpy(saved, canvas, canvas_size);

		for (y = 0; y < fh && (fy + y) < h; y++)
		{
			for (x = 0; x < fw && (fx + x) < w; x++)
			{
				int idx = img->RasterBits[y * fw + x];
				u8 *px;
				if (idx == gcb.TransparentColor)
					continue; /* transparent: the canvas shows through */
				if (idx >= cmap->ColorCount)
					continue;
				px = canvas + ((fy + y) * w + (fx + x)) * 3;
				px[0] = cmap->Colors[idx].Red;
				px[1] = cmap->Colors[idx].Green;
				px[2] = cmap->Colors[idx].Blue;
			}
		}

		dst_pck = gf_filter_pck_new_alloc(ctx->opid, canvas_size, &output);
		if (!dst_pck)
		{
			gf_free(canvas);
			gf_free(saved);
			DGifCloseFile(gif, &err);
			return GF_OUT_OF_MEM;
		}
		memcpy(output, canvas, canvas_size);

		/* Browsers clamp a zero or 1/100 s delay to 10/100; doing the same here
		 * keeps an animation from being played back far too fast. */
		delay = (gcb.DelayTime > 1) ? (u32)gcb.DelayTime : 10;
		gf_filter_pck_set_cts(dst_pck, cts);
		gf_filter_pck_set_duration(dst_pck, delay);
		gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
		gf_filter_pck_send(dst_pck);
		cts += delay;

		/* Disposal applies after the frame has been shown. */
		if (gcb.DisposalMode == DISPOSE_BACKGROUND)
		{
			for (y = 0; y < fh && (fy + y) < h; y++)
			{
				for (x = 0; x < fw && (fx + x) < w; x++)
				{
					u8 *px = canvas + ((fy + y) * w + (fx + x)) * 3;
					px[0] = px[1] = px[2] = 0;
				}
			}
		}
		else if (gcb.DisposalMode == DISPOSE_PREVIOUS)
		{
			memcpy(canvas, saved, canvas_size);
		}
	}

	gf_free(canvas);
	gf_free(saved);
	DGifCloseFile(gif, &err);

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void gifdec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability GIFDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "gif"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "image/gif"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister GIFDecoderRegister = {
	.name = "gifdec",
	GF_FS_SET_DESCRIPTION("GIF image and animation decoder")
		GF_FS_SET_HELP("This filter decodes GIF images and animations using giflib: every frame is composited onto the canvas and output in turn, with the timing carried by the file.")
			.private_size = sizeof(GF_GIFDecCtx),
	SETCAPS(GIFDecCaps),
	.configure_pid = gifdec_configure_pid,
	.process = gifdec_process,
	.process_event = gifdec_process_event,
	.finalize = gifdec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE gifdec_register(GF_FilterSession *session)
{
	return &GIFDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_gifdec(void) {
    gf_filter_auto_register("gifdec", gifdec_register);
}
