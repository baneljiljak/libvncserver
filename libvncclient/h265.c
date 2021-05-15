#include <rfb/rfbclient.h>

typedef struct {
    uint32_t length;    /* Encoded data size. */
} rfbH265Header;

#define sz_rfbH265Header 4

void CleanH265(rfbClient *cl) {
    av_parser_close(cl->parser);
    avcodec_free_context(&cl->dec_ctx);
    av_frame_free(&cl->frame);
    av_packet_free(&cl->pkt);
	sws_freeContext(cl->sws_ctx);
	free(cl->buffer_H265);
}

rfbBool HandleH265(rfbClient *client,
		           int rx,
		           int ry,
		           int rw,
		           int rh)
{
	rfbBool frameDecoded = FALSE;
	rfbH265Header hdr;
    int ret;
    uint32_t bytesToRecive;
    uint32_t bytesToDecode;
    char *buf_ptr;

	do {
		if (!ReadFromRFBServer(client, (char *)&hdr.length, sz_rfbH265Header))
			return FALSE;

		hdr.length = rfbClientSwap32IfLE(hdr.length);
        bytesToRecive = hdr.length;
		
        while (bytesToRecive) {
            if(bytesToRecive > RFB_BUFFER_SIZE) {
                if (!ReadFromRFBServer(client, client->buffer, RFB_BUFFER_SIZE))
                    return FALSE;
                bytesToDecode  = RFB_BUFFER_SIZE;
                bytesToRecive -= RFB_BUFFER_SIZE; 
            } else {
                if (!ReadFromRFBServer(client, client->buffer, bytesToRecive))
                    return FALSE;
                bytesToDecode  = bytesToRecive;
                bytesToRecive -= bytesToRecive;
            }
		    
            buf_ptr = client->buffer;

            /* parser splits data into frames */
            while (bytesToDecode > 0) {
                ret = av_parser_parse2(client->parser, client->dec_ctx,
                        &client->pkt->data, &client->pkt->size, buf_ptr,
                        bytesToDecode, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                
                if (ret < 0) {
                    rfbClientLog("Error while parsing.\n");
                    return FALSE;
                }

                buf_ptr		  += ret;
                bytesToDecode -= ret;

                if (client->pkt->size) {
                    ret = avcodec_send_packet(client->dec_ctx, client->pkt);
                    if (ret < 0) {
                        rfbClientLog("Error sending a packet for decoding\n");
                        return FALSE;
                    }

                    while (ret >= 0) {
                        ret = avcodec_receive_frame(client->dec_ctx, client->frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        } else if (ret < 0) {
                            rfbClientLog("Error during decoding\n");
                            return FALSE;
                        } else {
                            const int rgb32_stride[1] = { rw * 4 };
                            sws_scale(client->sws_ctx, (const uint8_t * const *)client->frame->data, client->frame->linesize, 0, client->frame->height, &client->buffer_H265, rgb32_stride);

                            client->GotBitmap(client, client->buffer_H265, rx, ry, rw, rh);

                            frameDecoded = TRUE;
                        }
                    }			
                }		
            }
		}
	} while(hdr.length);

	if (frameDecoded) {
		return TRUE;	
	} else {
		return SendFramebufferUpdateRequest(client, rx, ry, rw, rh, 0);
	}
}

rfbBool
InitializeH265(rfbClient *cl,
               int x,
               int y,
               int w,
               int h)
{
    cl->pkt = av_packet_alloc();
    if (!cl->pkt) {
    	rfbClientLog("Unable to allocate packet.\n");
	    CleanH265(cl);
		return FALSE;
	}

    cl->codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!cl->codec) {
        rfbClientLog("Codec not found.\n");
	 	CleanH265(cl);
        return FALSE;
    }

    cl->parser = av_parser_init(cl->codec->id);
    if (!cl->parser) {
        rfbClientLog("parser not found.\n");
		CleanH265(cl);
        return FALSE;
    }

    cl->dec_ctx = avcodec_alloc_context3(cl->codec);
    if (!cl->dec_ctx) {
        rfbClientLog("Could not allocate video codec context.\n");
        CleanH265(cl);
		return FALSE;
    }

    cl->dec_ctx->width = w;
    cl->dec_ctx->height = h;
    cl->dec_ctx->pix_fmt = AV_PIX_FMT_GBRP;

	av_opt_set(cl->dec_ctx->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(cl->dec_ctx, cl->codec, NULL) < 0) {
        rfbClientLog("Could not open codec.\n");
        CleanH265(cl);
		return FALSE;
    }

    cl->frame = av_frame_alloc();
    if (!cl->frame) {
        rfbClientLog("Could not allocate video frame.\n");
        CleanH265(cl);
		return FALSE;
    }

    /* bits per pixel */
    switch (cl->format.bitsPerPixel) {
        case 24:
	        cl->buffer_H265 = calloc(3 * w * h, sizeof(uint8_t));
            
            /* shiftRed + (shiftGreen / 2) + (shiftBlue * 2) */
            switch (cl->format.redShift +
                    (cl->format.blueShift * 2) +
                    (cl->format.greenShift / 2)) {
                case 36:
                    cl->sws_ctx = sws_getContext (w, h, AV_PIX_FMT_GBRP,w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
                    break;
                case 20:
                    cl->sws_ctx = sws_getContext (w, h, AV_PIX_FMT_GBRP,w, h, AV_PIX_FMT_BGR24, SWS_BILINEAR, NULL, NULL, NULL);
                    break;
                default:
                    rfbClientLog("Unable to use HEVC with current pixel format.\n");
                    return FALSE; 
            }
            break;
        case 32:
	        cl->buffer_H265 = calloc(4 * w * h, sizeof(uint8_t));

            switch (cl->format.redShift +
                    (cl->format.blueShift * 2) +
                    (cl->format.greenShift / 2)) {
                case 36:
                    cl->sws_ctx = sws_getContext (w, h, AV_PIX_FMT_GBRP,w, h, AV_PIX_FMT_RGB32, SWS_BILINEAR, NULL, NULL, NULL);
                    break;
                case 20:
                    cl->sws_ctx = sws_getContext (w, h, AV_PIX_FMT_GBRP,w, h, AV_PIX_FMT_BGR32, SWS_BILINEAR, NULL, NULL, NULL);
                    break;
                default:
                    rfbClientLog("Unable to use HEVC with current pixel format.\n");
                    return FALSE; 
            }
            break;
        default:
            break;
    }

    cl->HandleH265 = &HandleH265;
    return cl->HandleH265(cl, x, y, w, h);
}

