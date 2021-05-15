#include <rfb/rfb.h>
typedef struct {
    uint32_t length;	/* Encoded data size. */
} rfbH265Header;

#define sz_rfbH265Header 4

void rfbCleanH265(rfbClientPtr cl)
{
	avcodec_free_context(&cl->enc_ctx);
	av_frame_free(&cl->frame);
	av_packet_free(&cl->pkt);
	sws_freeContext(cl->sws_ctx);
}

// Currently only works with 32bpp (24bpp).
rfbBool
rfbHandleH265(rfbClientPtr cl,
                       int x,
                       int y,
                       int w,
                       int h)
{
    rfbFramebufferUpdateRectHeader rect;
    rfbH265Header hdr;

    if (w % 2 != 0) { w--; } 
    if (h % 2 != 0) { h--; } 

	if (x != 0 || y!= 0 || w != cl->frame->width || h != cl->frame->height) {
		return rfbSendRectEncodingRaw(cl, x, y, w, h);
	}
        
    /* Flush the buffer to guarantee correct alignment for translateFn(). */
    if (cl->ublen > 0) {
        if (!rfbSendUpdateBuf(cl)) {
        	rfbCleanH265(cl);
		    return FALSE;
		}
    }

    // First header.
    rect.r.x = Swap16IfLE(x);
    rect.r.y = Swap16IfLE(y);
    rect.r.w = Swap16IfLE(w);
    rect.r.h = Swap16IfLE(h);
    rect.encoding = Swap32IfLE(rfbEncodingH265);

    memcpy(&cl->updateBuf[cl->ublen], (char *)&rect,sz_rfbFramebufferUpdateRectHeader);
    cl->ublen += sz_rfbFramebufferUpdateRectHeader;

	// Make record how much data will be sent.
	rfbStatRecordEncodingSent(cl, rfbEncodingH265, sz_rfbFramebufferUpdateRectHeader, sz_rfbFramebufferUpdateRectHeader);

	/* make sure the frame data is writable */
	if (av_frame_make_writable(cl->frame) < 0) {
		rfbLog("Unable to make frame writable.");
		rfbCleanH265(cl);
		return FALSE;
	}

	int rgb32_stride[1] = { w * (cl->scaledScreen->bitsPerPixel / 8) };
    
    sws_scale(cl->sws_ctx, (const uint8_t * const *)&cl->screen->frameBuffer, rgb32_stride, 0, cl->frame->height, (uint8_t * const *)cl->frame->data, cl->frame->linesize);

	/* encode the image */    
    /* send the frame to the encoder */
    int ret = avcodec_send_frame(cl->enc_ctx, cl->frame);
    if (ret < 0) {
        rfbLog("Error sending a frame for encoding.\n");
		rfbCleanH265(cl);
		return FALSE;
    }

    while (ret >= 0) {
		// For each input frame/packet, the codec will typically
		// return 1 output frame/packet, but it can also be 0 or more than 1.
        ret = avcodec_receive_packet(cl->enc_ctx, cl->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			empty_rect:
			// Last HEVC header tells client there is no more data to recive.
			hdr.length = Swap32IfLE(0);
			memcpy(&cl->updateBuf[cl->ublen], (char *)&hdr, sz_rfbH265Header);
			cl->ublen += sz_rfbH265Header;

			rfbStatRecordEncodingSent(cl, rfbEncodingH265, sz_rfbH265Header,
				sz_rfbH265Header);

			if (!rfbSendUpdateBuf(cl)) {
				rfbCleanH265(cl);
				return FALSE;
			}

			cl->frame->pts++;

			return TRUE;
		} else if (ret < 0) {
            rfbLog("Error during encoding\n");
			rfbCleanH265(cl);
            return FALSE;
        }

		// Second header. (HEVC Header)
		hdr.length = Swap32IfLE(cl->pkt->size);

		memcpy(&cl->updateBuf[cl->ublen], (char *)&hdr, sz_rfbH265Header);
		cl->ublen += sz_rfbH265Header;

		// Make record how much data will be sent.
		rfbStatRecordEncodingSent(cl, rfbEncodingH265, cl->pkt->size, (w * h * cl->screen->serverFormat.bitsPerPixel / 8));

		// Send data to client. Data may be sent in chunks duo buffer limit.
		uint32_t bytesToSend = cl->pkt->size;
		while (bytesToSend) {
			if (bytesToSend > UPDATE_BUF_SIZE - cl->ublen) {
				memcpy( &cl->updateBuf[cl->ublen],
						&cl->pkt->data[cl->pkt->size - bytesToSend],
						(UPDATE_BUF_SIZE - cl->ublen));

				bytesToSend -= UPDATE_BUF_SIZE - cl->ublen;	
				cl->ublen += UPDATE_BUF_SIZE - cl->ublen;
			} else {
				memcpy( &cl->updateBuf[cl->ublen],
						&cl->pkt->data[cl->pkt->size - bytesToSend],
						bytesToSend);
		
				cl->ublen += bytesToSend;
				bytesToSend -= bytesToSend;
			}

			/* buffer full - flush partial rect and do another nlines */
			if (!rfbSendUpdateBuf(cl)) {
				rfbCleanH265(cl);
				return FALSE;
			}
		}

        av_packet_unref(cl->pkt);
    }
}

rfbBool
rfbInitializeH265(rfbClientPtr cl,
				int x,
				int y,
				int w,
				int h) 
{
    cl->codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    if (!cl->codec) {
		rfbLog("Codec 'HEVC' not found.\n");
		rfbCleanH265(cl);
		return FALSE;
    }

    cl->enc_ctx = avcodec_alloc_context3(cl->codec);
    if (!cl->enc_ctx) {
        rfbLog("Could not allocate video codec context.\n");
        rfbCleanH265(cl);
		return FALSE;
    }

    cl->pkt = av_packet_alloc();
    if (!cl->pkt) {
        rfbLog("Could not allocate video codec packet.\n");
        rfbCleanH265(cl);
		return FALSE;
    }

    cl->enc_ctx->bit_rate = 400000;
    /* resolution must be a multiple of two */
    if (w % 2 != 0) { w--; } 
    cl->enc_ctx->width = w;
    if (h % 2 != 0) { h--; }
    cl->enc_ctx->height = h;
    /* frames per second */
    cl->enc_ctx->time_base = (AVRational){1, 25};
    cl->enc_ctx->framerate = (AVRational){25, 1};

    cl->enc_ctx->gop_size = 10;
    cl->enc_ctx->max_b_frames = 0;
    cl->enc_ctx->pix_fmt = AV_PIX_FMT_GBRP;

	av_opt_set(cl->enc_ctx->priv_data, "tune", "zerolatency", 0);

	/* open it */
    if (avcodec_open2(cl->enc_ctx, cl->codec, NULL) < 0) {
        rfbLog("Could not open codec.\n");
       	rfbCleanH265(cl);
		return FALSE;
    }

    cl->frame = av_frame_alloc();
    if (!cl->frame) {
        rfbLog("Could not allocate video frame\n");
		rfbCleanH265(cl);
        return FALSE;
    }
    cl->frame->format = cl->enc_ctx->pix_fmt;
    cl->frame->width  = cl->enc_ctx->width;
    cl->frame->height = cl->enc_ctx->height;

    if (av_frame_get_buffer(cl->frame, 0) < 0) {
        rfbLog("Could not allocate the video frame data\n");
        rfbCleanH265(cl);
		return FALSE;
    }

	cl->frame->pts = 0;

    /* bits per pixel */
    switch (cl->screen->serverFormat.bitsPerPixel) {
        case 24:
            /* shiftRed + (shiftGreen / 2) + (shiftBlue * 2) */
            switch (cl->screen->serverFormat.redShift +
                    (cl->screen->serverFormat.blueShift * 2) +
                    (cl->screen->serverFormat.greenShift / 2)) {
                case 36:
                    cl->sws_ctx = sws_getContext (w, h, AV_PIX_FMT_RGB24,w, h, AV_PIX_FMT_GBRP, SWS_BILINEAR, NULL, NULL, NULL);
                    break;
                case 20:
                    cl->sws_ctx = sws_getContext (w, h, AV_PIX_FMT_BGR24,w, h, AV_PIX_FMT_GBRP, SWS_BILINEAR, NULL, NULL, NULL);
                    break;
                default:
                    rfbLog("Unable to use HEVC with current pixel format.\n");
                    return FALSE; 
            }
            break;
        case 32:
            switch (cl->screen->serverFormat.redShift +
                    (cl->screen->serverFormat.blueShift * 2) +
                    (cl->screen->serverFormat.greenShift / 2)) {
                case 36:
                    cl->sws_ctx = sws_getContext (w, h, AV_PIX_FMT_RGB32,w, h, AV_PIX_FMT_GBRP, SWS_BILINEAR, NULL, NULL, NULL);
                    break;
                case 20:
                    cl->sws_ctx = sws_getContext (w, h, AV_PIX_FMT_BGR32,w, h, AV_PIX_FMT_GBRP, SWS_BILINEAR, NULL, NULL, NULL);
                    break;
                default:
                    rfbLog("Unable to use HEVC with current pixel format.\n");
                    return FALSE; 
            }
            break;
        default:
            break;
    }

    if (cl->sws_ctx == NULL) {
        rfbLog("Unable to allocate sws context.");
		rfbCleanH265(cl);
        return FALSE;
    }

	cl->rfbSendRectEncodingH265 = &rfbHandleH265;
	return cl->rfbSendRectEncodingH265(cl, x, y, w, h);
}

