/*
 * Copyright (c) 2018 xiaowang yang 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include "libavutil/thread.h"
#include "libavutil/threadmessage.h"
#include "libavutil/fifo.h"
#include "libswscale/swscale.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

int with_decoding = 1;
int with_hook_frame = 1;
int with_encoding = 0;





static void *grow_array(void *array, int elem_size, int *size, int new_size)
{
    if (new_size >= INT_MAX / elem_size) {
        av_log(NULL, AV_LOG_ERROR, "Array too big.\n");
        return NULL;
    }
    if (*size < new_size) {
        uint8_t *tmp = av_realloc_array(array, new_size, elem_size);
        if (!tmp) {
            av_log(NULL, AV_LOG_ERROR, "Could not alloc buffer.\n");
            return NULL;
        }
        memset(tmp + *size*elem_size, 0, (new_size-*size) * elem_size);
        *size = new_size;
        return tmp;
    }
    return array;
}

#define GROW_ARRAY(array, nb_elems)\
    array = grow_array(array, sizeof(*array), &nb_elems, nb_elems + 1)

typedef struct InputStream {

    AVCodecContext *dec_ctx;
    AVCodec *dec;

    AVStream *st;
    int64_t       start;     /* time when read started */
    /* predicted dts of the next packet read for this stream or (when there are
     * several frames in a packet) of the next frame in current packet (in AV_TIME_BASE units) */
    int64_t       next_dts;
    int64_t       dts;       ///< dts of the last packet read for this stream (in AV_TIME_BASE units)

    int64_t       next_pts;  ///< synthetic pts for the next decode frame (in AV_TIME_BASE units)
    int64_t       pts;       ///< current pts of the decoded frame  (in AV_TIME_BASE units)



    int64_t min_pts; /* pts with the smallest value in a current stream */
    int64_t max_pts; /* pts with the higher value in a current stream */


    int64_t nb_samples; /* number of samples in the last decoded audio frame before looping */


    int saw_first_ts;
    AVRational framerate;

    int data_size;
    int nb_packets;



    //for decode
    int got_output;
    AVFrame* decoded_frame;

    int decoding_needed;

    /* decoded data from this stream goes into all those filters
     * currently video and audio only */


    AVFrame* filter_frame;



} InputStream;


// a wrapper around a single output AVStream
typedef struct OutputStream {
 int file_index;          /* file index */
    int index;               /* stream index in the output file */
    int source_index;        /* InputStream index */
    AVStream *st;            /* stream in the output file */

    int frame_number;
    /* input pts and corresponding output pts
       for A/V sync */


    /* dts of the last packet sent to the muxer */
    int64_t last_mux_dts;
    // the timebase of the packets sent to the muxer
    AVRational mux_timebase;
    AVRational enc_timebase;


    AVCodecContext *enc_ctx;
    AVCodecParameters *ref_par; /* associated input codec parameters with encoders options applied */
    AVCodec *enc;



    AVRational frame_rate;

    AVDictionary *encoder_opts;

    int encoding_needed;
 


} OutputStream;


InputStream **input_streams = NULL;
OutputStream **output_streams = NULL;
AVDictionary *format_opts;
int nb_input_streams = 0;
int nb_output_streams = 0;

AVFormatContext *ic; //input format context
AVFormatContext *oc; //output format context


static int open_input_file(char* filename){
    
    int err, i, ret;



    //allocate and init format context
    ic = avformat_alloc_context();
    ic->video_codec_id     = AV_CODEC_ID_NONE;
    ic->audio_codec_id     =  AV_CODEC_ID_NONE;
    ic->subtitle_codec_id  = AV_CODEC_ID_NONE;
    ic->data_codec_id      = AV_CODEC_ID_NONE;
    ic->flags |= AVFMT_FLAG_NONBLOCK;



    //open input file or url
    av_dict_set(&format_opts, "buffer_size", "1024000", 0);
    av_dict_set(&format_opts, "stimeout", "20000000", 0);
    av_dict_set(&format_opts, "max_delay", "500000", 0);
    av_dict_set(&format_opts, "rtsp_transport", "tcp", 0);
    err = avformat_open_input(&ic, filename, NULL, &format_opts);


    //retrieve more stream info
    ret = avformat_find_stream_info(ic, NULL);




    for (i = 0; i < ic->nb_streams; i++) {
        GROW_ARRAY(input_streams, nb_input_streams);

        AVStream *st = ic->streams[i];
        AVCodecParameters *par = st->codecpar;
        InputStream *ist = av_mallocz(sizeof(*ist));

        input_streams[i] = ist;
        ist->st = st;        
        st->discard  = 0;
        ist->nb_samples = 0;
        ist->min_pts = INT64_MAX;
        ist->max_pts = INT64_MIN;
        ist->next_pts = AV_NOPTS_VALUE;
        ist->next_dts = AV_NOPTS_VALUE;
        ist->saw_first_ts=0;
        ist->decoding_needed = 0;


        ist->dec = avcodec_find_decoder(st->codecpar->codec_id);
        ist->dec_ctx = avcodec_alloc_context3(ist->dec);
        ret = avcodec_parameters_to_context(ist->dec_ctx, par);
        ist->dec_ctx->framerate = st->avg_frame_rate;
        ret = avcodec_parameters_from_context(par, ist->dec_ctx);


        if (with_decoding){
            AVCodec *codec = ist->dec;
            
            ist->dec_ctx->opaque                = ist;
            ist->dec_ctx->thread_safe_callbacks = 1;
            ist->dec_ctx->pkt_timebase = ist->st->time_base;

            ist->got_output = 0;

            avcodec_open2(ist->dec_ctx, codec, NULL);
        }

    }

    av_dump_format(ic, 1, filename, 0);

    return ret;


}

static int init_output_stream_encode(OutputStream *ost)
{
    InputStream *ist = input_streams[ost->source_index];
    AVCodecContext *enc_ctx = ost->enc_ctx;
    AVCodecContext *dec_ctx = NULL;
    int j, ret;

    // Muxers use AV_PKT_DATA_DISPLAYMATRIX to signal rotation. On the other
    // hand, the legacy API makes demuxers set "rotate" metadata entries,
    // which have to be filtered out to prevent leaking them to output files.
    av_dict_set(&ost->st->metadata, "rotate", NULL, 0);

    if (ist) {
        ost->st->disposition          = ist->st->disposition;

        dec_ctx = ist->dec_ctx;

        enc_ctx->chroma_sample_location = dec_ctx->chroma_sample_location;
    } 

    if (enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {

        if (ist && !ost->frame_rate.num)
            ost->frame_rate = ist->framerate;
        if (ist && !ost->frame_rate.num)
            ost->frame_rate = ist->st->r_frame_rate;
        if (ist && !ost->frame_rate.num) {
            ost->frame_rate = (AVRational){25, 1};
            av_log(NULL, AV_LOG_WARNING,
                   "No information "
                   "about the input framerate is available. Falling "
                   "back to a default value of 25fps for output stream #%d:%d. Use the -r option "
                   "if you want a different framerate.\n",
                   ost->file_index, ost->index);
        }
//    
        // reduce frame rate for mpeg4 to be within the spec limits
        if (enc_ctx->codec_id == AV_CODEC_ID_MPEG4) {
            av_reduce(&ost->frame_rate.num, &ost->frame_rate.den,
                      ost->frame_rate.num, ost->frame_rate.den, 65535);
        }
    }

    switch (enc_ctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        if (dec_ctx)
            enc_ctx->bits_per_raw_sample = FFMIN(dec_ctx->bits_per_raw_sample,
                                                 av_get_bytes_per_sample(enc_ctx->sample_fmt) << 3);
        enc_ctx->time_base = av_make_q(1, enc_ctx->sample_rate);
        break;

    case AVMEDIA_TYPE_VIDEO:
        enc_ctx->time_base = av_inv_q(ost->frame_rate);

        enc_ctx->framerate = ost->frame_rate;

        ost->st->avg_frame_rate = ost->frame_rate;


     
        break;

    }

    ost->mux_timebase = enc_ctx->time_base;

    return 0;
}


static OutputStream *new_output_stream(AVFormatContext *oc, enum AVMediaType type, int source_index)
{
    OutputStream *ost;
    AVStream *st = avformat_new_stream(oc, NULL);
    int idx      = oc->nb_streams - 1, ret = 0; 
    int i;

    GROW_ARRAY(output_streams, nb_output_streams);
    ost = av_mallocz(sizeof(*ost));
    output_streams[nb_output_streams - 1] = ost;

    ost->file_index = 0;
    ost->index      = idx;
    ost->st         = st;
    st->codecpar->codec_type = type;
    ost->enc_ctx = avcodec_alloc_context3(ost->enc);
    ost->enc_ctx->codec_type = type;
    ost->ref_par = avcodec_parameters_alloc();
    ost->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ost->source_index = source_index;

    ost->last_mux_dts = AV_NOPTS_VALUE;

    ost->enc_ctx->codec_type = type;

    ost->encoding_needed = 0;





    if(with_encoding){

        ost->encoding_needed = 1;

        ost->st->codecpar->codec_id = av_guess_codec(oc->oformat, NULL, oc->url,
                                                         NULL, ost->st->codecpar->codec_type);
        ost->enc = avcodec_find_encoder(ost->st->codecpar->codec_id);


        AVCodec      *codec = ost->enc;
        AVCodecContext *dec = NULL;
        InputStream *ist;

        ret = init_output_stream_encode(ost);
        if (ret < 0)
            return ret;

        ist = input_streams[ost->source_index];

        if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
            !codec->defaults &&
            !av_dict_get(ost->encoder_opts, "b", NULL, 0) &&
            !av_dict_get(ost->encoder_opts, "ab", NULL, 0))
            av_dict_set(&ost->encoder_opts, "b", "128000", 0);

        // if(ost->enc->type == AVMEDIA_TYPE_VIDEO){
        //     enc_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        // }



        ret = avcodec_open2(ost->enc_ctx, codec, &ost->encoder_opts) ;
        ret = avcodec_parameters_from_context(ost->st->codecpar, ost->enc_ctx);
        ret = avcodec_copy_context(ost->st->codec, ost->enc_ctx);
    


        // copy timebase while removing common factors
        if (ost->st->time_base.num <= 0 || ost->st->time_base.den <= 0)
            ost->st->time_base = av_add_q(ost->enc_ctx->time_base, (AVRational){0, 1});

        // copy estimated duration as a hint to the muxer
        if (ost->st->duration <= 0 && ist && ist->st->duration > 0)
            ost->st->duration = av_rescale_q(ist->st->duration, ist->st->time_base, ost->st->time_base);

        ost->st->codec->codec= ost->enc_ctx->codec;
    }

    return ost;
}







static int open_output_file(const char *filename,const char* format){

    int i, j, err;
   
    InputStream  *ist;


    int format_flags = 0;
    err = avformat_alloc_output_context2(&oc, NULL, format, filename);

    OutputStream *ost;
    AVCodecContext *enc;

    ost = new_output_stream(oc, AVMEDIA_TYPE_VIDEO, 0);


   

    ost = new_output_stream(oc, AVMEDIA_TYPE_AUDIO, 1);




    avio_open2(&oc->pb, filename, AVIO_FLAG_WRITE,NULL,
                              NULL);
   

    return 0;
}


static void write_packet(AVPacket *pkt, OutputStream *ost, int unqueue)
{
    AVFormatContext *s = oc;
    AVStream *st = ost->st;
    int ret;
    av_packet_rescale_ts(pkt, ost->mux_timebase, ost->st->time_base);
   
    ost->last_mux_dts = pkt->dts;

    pkt->stream_index = ost->index;


    av_log(NULL,AV_LOG_INFO,"dts:%d,pts:%d\n",pkt->dts,pkt->pts);


    ret = av_interleaved_write_frame(s, pkt);
 
    av_packet_unref(pkt);
}


static void do_streamcopy(InputStream *ist, OutputStream *ost, const AVPacket *pkt)
{

    AVPacket opkt = { 0 };
    av_init_packet(&opkt);
    if (pkt->pts != AV_NOPTS_VALUE)
        opkt.pts = av_rescale_q(pkt->pts, ist->st->time_base, ost->mux_timebase);
    else
        opkt.pts = AV_NOPTS_VALUE;
    if (pkt->dts == AV_NOPTS_VALUE)
        opkt.dts = av_rescale_q(ist->dts, AV_TIME_BASE_Q, ost->mux_timebase);
    else
        opkt.dts = av_rescale_q(pkt->dts, ist->st->time_base, ost->mux_timebase);
    opkt.duration = av_rescale_q(pkt->duration, ist->st->time_base, ost->mux_timebase);
    opkt.flags    = pkt->flags;
    if (pkt->buf) {
        opkt.buf = av_buffer_ref(pkt->buf);
    }
    opkt.data = pkt->data;
    opkt.size = pkt->size;
    av_copy_packet_side_data(&opkt, pkt);
    write_packet(&opkt, ost, 0);

}

static int decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
{
    int ret;

    *got_frame = 0;

    if (pkt) {
        ret = avcodec_send_packet(avctx, pkt);
        // In particular, we don't expect AVERROR(EAGAIN), because we read all
        // decoded frames with avcodec_receive_frame() until done.
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
    }

    ret = avcodec_receive_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN))
        return ret;
    if (ret >= 0)
        *got_frame = 1;

    return 0;
}



#pragma pack(push, 1)

typedef unsigned char  U8;
typedef unsigned short U16;
typedef unsigned int   U32;

typedef struct tagBITMAPFILEHEADER
{

 U16 bfType;
 U32 bfSize;
 U16 bfReserved1;
 U16 bfReserved2;
 U32 bfOffBits;
} BITMAPFILEHEADER;

typedef struct tagBITMAPINFOHEADER
{
 U32 biSize;
 U32 biWidth;
 U32 biHeight;
 U16 biPlanes;
 U16 biBitCount;
 U32 biCompression;
 U32 biSizeImage;
 U32 biXPelsPerMeter;
 U32 biYPelsPerMeter;
 U32 biClrUsed;
 U32 biClrImportant;
} BITMAPINFOHEADER;

typedef struct tagRGBQUAD
{
 U8 rgbBlue;
 U8 rgbGreen;
 U8 rgbRed;
 U8 rgbReserved;
} RGBQUAD;

typedef struct tagBITMAPINFO
{
 BITMAPINFOHEADER bmiHeader;
 RGBQUAD bmiColors[1];
} BITMAPINFO;


typedef struct tagBITMAP
{
 BITMAPFILEHEADER bfHeader;
 BITMAPINFO biInfo;
}BITMAPFILE;

#pragma pack(pop)



//Éú³ÉBMPÍ¼Æ¬(ÎÞÑÕÉ«±íµÄÎ»Í¼):ÔÚRGB(A)Î»Í¼Êý¾ÝµÄ»ù´¡ÉÏ¼ÓÉÏÎÄ¼þÐÅÏ¢Í·ºÍÎ»Í¼ÐÅÏ¢Í·  
static int GenBmpFile(U8 *pData, U8 bitCountPerPix, U32 width, U32 height, const char *filename)
{
    FILE *fp = fopen(filename, "wb");
    if(!fp)
    {
        printf("fopen failed : %s, %d\n", __FILE__, __LINE__);
        return 0;
    }

    U32 bmppitch = ((width*bitCountPerPix + 31) >> 5) << 2;
    U32 filesize = bmppitch*height;

    BITMAPFILE bmpfile;

    bmpfile.bfHeader.bfType = 0x4D42;
    bmpfile.bfHeader.bfSize = filesize + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bmpfile.bfHeader.bfReserved1 = 0;
    bmpfile.bfHeader.bfReserved2 = 0;
    bmpfile.bfHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    bmpfile.biInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmpfile.biInfo.bmiHeader.biWidth = width;
    bmpfile.biInfo.bmiHeader.biHeight = height;
    bmpfile.biInfo.bmiHeader.biPlanes = 1;
    bmpfile.biInfo.bmiHeader.biBitCount = bitCountPerPix;
    bmpfile.biInfo.bmiHeader.biCompression = 0;
    bmpfile.biInfo.bmiHeader.biSizeImage = 0;
    bmpfile.biInfo.bmiHeader.biXPelsPerMeter = 0;
    bmpfile.biInfo.bmiHeader.biYPelsPerMeter = 0;
    bmpfile.biInfo.bmiHeader.biClrUsed = 0;
    bmpfile.biInfo.bmiHeader.biClrImportant = 0;

    fwrite(&(bmpfile.bfHeader), sizeof(BITMAPFILEHEADER), 1, fp);
    fwrite(&(bmpfile.biInfo.bmiHeader), sizeof(BITMAPINFOHEADER), 1, fp);

    U8 *pEachLinBuf = (U8*)malloc(bmppitch);
    memset(pEachLinBuf, 0, bmppitch);
    U8 BytePerPix = bitCountPerPix >> 3;
    U32 pitch = width * BytePerPix;
    if(pEachLinBuf)
    {
        int h,w;
        for(h = height-1; h >= 0; h--)
        {
            for(w = 0; w < width; w++)
            {
                //copy by a pixel  
                pEachLinBuf[w*BytePerPix+0] = pData[h*pitch + w*BytePerPix + 0];
                pEachLinBuf[w*BytePerPix+1] = pData[h*pitch + w*BytePerPix + 1];
                pEachLinBuf[w*BytePerPix+2] = pData[h*pitch + w*BytePerPix + 2];
            }
            fwrite(pEachLinBuf, bmppitch, 1, fp);


        }
        free(pEachLinBuf);
    }

    fclose(fp);

    return 1;
}

//»ñÈ¡BMPÎÄ¼þµÄÎ»Í¼Êý¾Ý(ÎÞÑÕÉ«±íµÄÎ»Í¼):¶ªµôBMPÎÄ¼þµÄÎÄ¼þÐÅÏ¢Í·ºÍÎ»Í¼ÐÅÏ¢Í·£¬»ñÈ¡ÆäRGB(A)Î»Í¼Êý¾Ý  
static U8* GetBmpData(U8 *bitCountPerPix, U32 *width, U32 *height, const char* filename)
{
    FILE *pf = fopen(filename, "rb");
    if(!pf)
    {
        printf("fopen failed : %s, %d\n", __FILE__, __LINE__);
        return NULL;
    }

    BITMAPFILE bmpfile;
    fread(&(bmpfile.bfHeader), sizeof(BITMAPFILEHEADER), 1, pf);
    fread(&(bmpfile.biInfo.bmiHeader), sizeof(BITMAPINFOHEADER), 1, pf);


    if(bitCountPerPix)
    {
        *bitCountPerPix = bmpfile.biInfo.bmiHeader.biBitCount;
    }
    if(width)
    {
        *width = bmpfile.biInfo.bmiHeader.biWidth;
    }
    if(height)
    {
        *height = bmpfile.biInfo.bmiHeader.biHeight;
    }

    U32 bmppicth = (((*width)*(*bitCountPerPix) + 31) >> 5) << 2;
    U8 *pdata = (U8*)malloc((*height)*bmppicth);

    U8 *pEachLinBuf = (U8*)malloc(bmppicth);
    memset(pEachLinBuf, 0, bmppicth);
    U8 BytePerPix = (*bitCountPerPix) >> 3;
    U32 pitch = (*width) * BytePerPix;

    if(pdata && pEachLinBuf)
    {
        int w, h;
        for(h = (*height) - 1; h >= 0; h--)
        {
            fread(pEachLinBuf, bmppicth, 1, pf);
            for(w = 0; w < (*width); w++)
            {
                pdata[h*pitch + w*BytePerPix + 0] = pEachLinBuf[w*BytePerPix+0];
                pdata[h*pitch + w*BytePerPix + 1] = pEachLinBuf[w*BytePerPix+1];
                pdata[h*pitch + w*BytePerPix + 2] = pEachLinBuf[w*BytePerPix+2];
            }
        }
        free(pEachLinBuf);
    }
    fclose(pf);

    return pdata;
}

//ÊÍ·ÅGetBmpData·ÖÅäµÄ¿Õ¼ä  
static void FreeBmpData(U8 *pdata)
{
    if(pdata)
    {
        free(pdata);
        pdata = NULL;
    }
}

typedef struct _LI_RGB
{
    U8 b;
    U8 g;
    U8 r;
}LI_RGB;

static void saveFrameToBmp(uint8_t* pdst,int width,int height,char* name)
{


    LI_RGB* pdata = (LI_RGB*)malloc(sizeof(LI_RGB)*width*height);
    LI_RGB* orig = pdata;
    memset(pdata,0,sizeof(LI_RGB)*width*height);
    for (int i = 0; i < width*height; i++)
    {

        (*pdata).b = *(pdst++);
        (*pdata).g = *(pdst++);
        (*pdata).r = *(pdst++);
        pdata++;
    }
    GenBmpFile((U8*)orig,24,width,height,name);
    FreeBmpData(orig);
    //free(pdata);
}


static struct SwsContext *img_convert_ctx = NULL;
static AVThreadMessageQueue *hook_thread_queue;
static pthread_t hook_thread;
static int hook_thread_queue_size=1000;


static void *hook_thread_proc(void *arg)
{

    AVFrame* frame=NULL;
        static unsigned sws_flags = SWS_BICUBIC;
        int ret = 0;
    av_log(NULL,AV_LOG_ERROR,"start the hook thread\n");
    while(1){

        ret = av_thread_message_queue_recv(hook_thread_queue, &frame,0);

        if(ret<0){
            av_log(NULL, AV_LOG_ERROR, "thread recv error %s\n", strerror(ret));
            continue;
        }

        av_log(NULL,AV_LOG_FATAL," hook a frame \n");

        img_convert_ctx = sws_getCachedContext(img_convert_ctx,
                    frame->width, frame->height, frame->format, frame->width, frame->height,
                    AV_PIX_FMT_BGR24, sws_flags, NULL, NULL, NULL);
  


        if (img_convert_ctx != NULL && frame->format == AV_PIX_FMT_YUVJ420P) {

         
          
            uint8_t *dst_data[4];  
            int dst_linesize[4];  


            int src_w = frame->width;
            int src_h = frame->height;

            ret = av_image_alloc(dst_data, dst_linesize,src_w, src_h, AV_PIX_FMT_BGR24, 1);  
            if (ret< 0) {  
                printf( "Could not allocate destination image\n");  
                
            }else{
                sws_scale(img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                      0, frame->height, dst_data, dst_linesize);
                saveFrameToBmp(dst_data[0],frame->width,frame->height,"test.bmp");

            }





        } else {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");

        }

        av_frame_free(&frame);




    }
    av_log(NULL,AV_LOG_ERROR,"end the thread\n");
    return NULL;

}

static int init_hook_threads(void)
{

    int ret = av_thread_message_queue_alloc(&hook_thread_queue,
                                        hook_thread_queue_size, sizeof(AVFrame));
    if (ret < 0)
        return ret;

    if ((ret = pthread_create(&hook_thread, NULL, hook_thread_proc, NULL))) {
        av_log(NULL, AV_LOG_ERROR, "pthread_create failed: %s. Try to increase `ulimit -v` or decrease `ulimit -s`.\n", strerror(ret));
        av_thread_message_queue_free(&hook_thread_queue);
        return AVERROR(ret);
    }

    return 0;
}





static int hook_the_frame(InputStream *ist, AVFrame *decoded_frame){

    int ret = 0;
    AVFrame *frame = decoded_frame;
  

    unsigned flags = AV_THREAD_MESSAGE_NONBLOCK ;


    static int bb=0;
    bb++;
    int speed= 2;



    if( bb % speed ==0){


        AVFrame *clone = av_frame_alloc();
        int i;
        if (!clone)
            return AVERROR(ENOMEM);
        clone->format = frame->format;
        clone->width  = FFALIGN(frame->width, 16);
        clone->height = FFALIGN(frame->height, 16);
        ret = av_frame_get_buffer(clone, 32);
        if (ret < 0) {
            av_frame_free(&clone);
            return ret;
        }

        ret = av_frame_copy(clone, frame);
        if (ret < 0) {
            av_frame_free(&clone);
            return ret;
        }

        frame = clone;

        ret = av_thread_message_queue_send(hook_thread_queue, &clone, flags);
        if (ret < 0) {

            av_frame_free(&clone);
            av_thread_message_queue_set_err_recv(hook_thread_queue, ret);

        }

    }

    return ret;

}


static int send_frame_to_encoding(OutputStream *ost,
                         AVFrame *in_picture, AVPacket* rpkt){

    AVPacket pkt;
    AVCodecContext *enc = ost->enc_ctx;

    int ret = avcodec_send_frame(enc, in_picture);
    if (ret < 0)
        return -1;

    while (1) {
        ret = avcodec_receive_packet(enc, &pkt);
        if (ret == AVERROR(EAGAIN))
            break;

        pkt.pts = rpkt->dts;

        av_packet_rescale_ts(&pkt, enc->time_base, ost->mux_timebase);

        int frame_size = pkt.size;
        write_packet(&pkt,ost,0);
    }
    return 0;
}


static int decode_video(InputStream *ist, AVPacket *pkt, int *got_output, int64_t *duration_pts, int eof,
                        int *decode_failed)
{
    AVFrame *decoded_frame;
    int i, ret = 0, err = 0;
    int64_t best_effort_timestamp;
    int64_t dts = AV_NOPTS_VALUE;
    AVPacket avpkt;

    // With fate-indeo3-2, we're getting 0-sized packets before EOF for some
    // reason. This seems like a semi-critical bug. Don't trigger EOF, and
    // skip the packet.
    if (!eof && pkt && pkt->size == 0)
        return 0;

    if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);

    decoded_frame = ist->decoded_frame;
    if (ist->dts != AV_NOPTS_VALUE)
        dts = av_rescale_q(ist->dts, AV_TIME_BASE_Q, ist->st->time_base);
    if (pkt) {
        avpkt = *pkt;
        avpkt.dts = dts; // ffmpeg.c probably shouldn't do this
    }



    ret = decode(ist->dec_ctx, decoded_frame, got_output, pkt ? &avpkt : NULL);

    if (ret < 0)
        *decode_failed = 1;



    if (*got_output && ret >= 0) {
        if (ist->dec_ctx->width  != decoded_frame->width ||
            ist->dec_ctx->height != decoded_frame->height ||
            ist->dec_ctx->pix_fmt != decoded_frame->format) {
            av_log(NULL, AV_LOG_DEBUG, "Frame parameters mismatch context %d,%d,%d != %d,%d,%d\n",
                decoded_frame->width,
                decoded_frame->height,
                decoded_frame->format,
                ist->dec_ctx->width,
                ist->dec_ctx->height,
                ist->dec_ctx->pix_fmt);
        }
    }

    if (!*got_output || ret < 0)
        return ret;




    best_effort_timestamp= decoded_frame->best_effort_timestamp;
    *duration_pts = decoded_frame->pkt_duration;



    if(best_effort_timestamp != AV_NOPTS_VALUE) {
        int64_t ts = av_rescale_q(decoded_frame->pts = best_effort_timestamp, ist->st->time_base, AV_TIME_BASE_Q);

        if (ts != AV_NOPTS_VALUE)
            ist->next_pts = ist->pts = ts;
    }


    //todo frame is decoded
    //send_frame_to_filters(ist, decoded_frame);

    if(with_hook_frame){
        hook_the_frame(ist,decoded_frame);
    }

    if(with_encoding){
        send_frame_to_encoding(output_streams[0],decoded_frame,pkt);
    }

fail:

    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}




static int decode_audio(InputStream *ist, AVPacket *pkt, int *got_output,
                        int *decode_failed)
{
    AVFrame *decoded_frame;
    AVCodecContext *avctx = ist->dec_ctx;
    int ret, err = 0;
    AVRational decoded_frame_tb;

    if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);

    decoded_frame = ist->decoded_frame;


    ret = decode(avctx, decoded_frame, got_output, pkt);

    if (ret < 0)
        *decode_failed = 1;

    if (ret >= 0 && avctx->sample_rate <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Sample rate %d invalid\n", avctx->sample_rate);
        ret = AVERROR_INVALIDDATA;
    }


    if (!*got_output || ret < 0)
        return ret;



    ist->next_pts += ((int64_t)AV_TIME_BASE * decoded_frame->nb_samples) /
                     avctx->sample_rate;
    ist->next_dts += ((int64_t)AV_TIME_BASE * decoded_frame->nb_samples) /
                     avctx->sample_rate;


    if (decoded_frame->pts != AV_NOPTS_VALUE) {
        decoded_frame_tb   = ist->st->time_base;
    } else if (pkt && pkt->pts != AV_NOPTS_VALUE) {
        decoded_frame->pts = pkt->pts;
        decoded_frame_tb   = ist->st->time_base;
    }else {
        decoded_frame->pts = ist->dts;
        decoded_frame_tb   = AV_TIME_BASE_Q;
    }

    ist->nb_samples = decoded_frame->nb_samples;
    // err = send_frame_to_filters(ist, decoded_frame);





    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}


int main(int argc, char **argv)
{
    int i, ret;
    int64_t ti;

    char* input_file_name = argv[1];//source rtsp url
    char* output_file_name = argv[2]; //rtmp url
    open_input_file(input_file_name);
    open_output_file(output_file_name,"flv");

    OutputStream *ost;
    InputStream *ist;
    int64_t last_ts;


    if(with_hook_frame)
        init_hook_threads();



    /* open each encoder */
    for (i = 0; i < nb_output_streams; i++) {

        OutputStream *ost = output_streams[i];
        InputStream *ist = input_streams[ost->source_index];
        AVCodecParameters *par_dst = ost->st->codecpar;
        AVCodecParameters *par_src = ost->ref_par;
        AVRational sar;
        int i, ret;
       

        ret = avcodec_parameters_to_context(ost->enc_ctx, ist->st->codecpar);
        avcodec_parameters_from_context(par_src, ost->enc_ctx);
        ret = avcodec_parameters_copy(par_dst, par_src);
     

        if (!ost->frame_rate.num)
            ost->frame_rate = ist->framerate;
        ost->st->avg_frame_rate = ost->frame_rate;

        // copy timebase while removing common factors
        if (ost->st->time_base.num <= 0 || ost->st->time_base.den <= 0)
            ost->st->time_base = av_add_q(av_stream_get_codec_timebase(ost->st), (AVRational){0, 1});

        // copy disposition
        ost->st->disposition = ist->st->disposition;

        


        switch (par_dst->codec_type) {
        case AVMEDIA_TYPE_AUDIO:

            break;
        case AVMEDIA_TYPE_VIDEO:

            sar = par_src->sample_aspect_ratio;
            ost->st->sample_aspect_ratio = par_dst->sample_aspect_ratio = sar;
            ost->st->avg_frame_rate = ist->st->avg_frame_rate;
            ost->st->r_frame_rate = ist->st->r_frame_rate;
            break;
        }

        ost->mux_timebase = ist->st->time_base;




    }


    avformat_write_header(oc, NULL);


    av_dump_format(oc ,0, oc->url, 1);


    int frame_cnt = 0;
    while (frame_cnt<20000) {

        frame_cnt++;

        InputStream *ist;
        AVPacket pkt;
        int ret, i, j;
        int64_t duration;
        int64_t pkt_dts;

        av_read_frame(ic, &pkt);
      


        ist = input_streams[pkt.stream_index];
        ist->data_size += pkt.size;
        ist->nb_packets++;


        if (pkt.dts != AV_NOPTS_VALUE)
            pkt.dts += av_rescale_q(0, AV_TIME_BASE_Q, ist->st->time_base);
        if (pkt.pts != AV_NOPTS_VALUE)
            pkt.pts += av_rescale_q(0, AV_TIME_BASE_Q, ist->st->time_base);




        pkt_dts = av_rescale_q_rnd(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);

        duration = av_rescale_q(0, (AVRational){ 1, 1 }, ist->st->time_base);
        if (pkt.pts != AV_NOPTS_VALUE) {
            pkt.pts += duration;
            ist->max_pts = FFMAX(pkt.pts, ist->max_pts);
            ist->min_pts = FFMIN(pkt.pts, ist->min_pts);
        }

        if (pkt.dts != AV_NOPTS_VALUE)
            pkt.dts += duration;

        pkt_dts = av_rescale_q_rnd(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        

        if (pkt.dts != AV_NOPTS_VALUE)
            last_ts = av_rescale_q(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q);

        int repeating = 0;
        int eof_reached = 0;


        if (!ist->saw_first_ts) {
            ist->dts = ist->st->avg_frame_rate.num ? - ist->dec_ctx->has_b_frames * AV_TIME_BASE / av_q2d(ist->st->avg_frame_rate) : 0;
            ist->pts = 0;
            if (pkt.pts != AV_NOPTS_VALUE) {
                ist->dts += av_rescale_q(pkt.pts, ist->st->time_base, AV_TIME_BASE_Q);
                ist->pts = ist->dts; //unused but better to set it to a value thats not totally wrong
            }
            ist->saw_first_ts = 1;
        }

        if (ist->next_dts == AV_NOPTS_VALUE)
            ist->next_dts = ist->dts;
        if (ist->next_pts == AV_NOPTS_VALUE)
            ist->next_pts = ist->pts;


        if (pkt.dts != AV_NOPTS_VALUE) {
            ist->next_dts = ist->dts = av_rescale_q(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q);
            if (ist->dec_ctx->codec_type != AVMEDIA_TYPE_VIDEO)
                ist->next_pts = ist->pts = ist->dts;
        }


        AVPacket avpkt = pkt;

        while(with_decoding){
            int64_t duration_dts = 0;
            int64_t duration_pts = 0;
            int got_output = 0;
            int decode_failed = 0;

            ist->pts = ist->next_pts;
            ist->dts = ist->next_dts;

            

            switch (ist->dec_ctx->codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    ret = decode_audio    (ist, repeating ? NULL : &avpkt, &got_output,
                                           &decode_failed);
                    break;
                case AVMEDIA_TYPE_VIDEO:

                    ret = decode_video (ist, repeating ? NULL : &avpkt, &got_output, &duration_pts, 0,
                                           &decode_failed);


                    if (!repeating || got_output) {
                        if (pkt.duration) {
                            duration_dts = av_rescale_q(pkt.duration, ist->st->time_base, AV_TIME_BASE_Q);
                        } else if(ist->dec_ctx->framerate.num != 0 && ist->dec_ctx->framerate.den != 0) {
                            int ticks= av_stream_get_parser(ist->st) ? av_stream_get_parser(ist->st)->repeat_pict+1 : ist->dec_ctx->ticks_per_frame;
                            duration_dts = ((int64_t)AV_TIME_BASE *
                                            ist->dec_ctx->framerate.den * ticks) /
                                            ist->dec_ctx->framerate.num / ist->dec_ctx->ticks_per_frame;
                        }

                        if(ist->dts != AV_NOPTS_VALUE && duration_dts) {
                            ist->next_dts += duration_dts;
                        }else
                            ist->next_dts = AV_NOPTS_VALUE;
                    }

                    if (got_output) {
                        if (duration_pts > 0) {
                            ist->next_pts += av_rescale_q(duration_pts, ist->st->time_base, AV_TIME_BASE_Q);
                        } else {
                            ist->next_pts += duration_dts;
                        }
                    }
                    break;
            }


            if (got_output)
                ist->got_output = 1;

            if (!got_output)
                break;

            repeating = 1;
        }



        if(!with_encoding){
            ist->dts = ist->next_dts;
            switch (ist->dec_ctx->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                ist->next_dts += ((int64_t)AV_TIME_BASE * ist->dec_ctx->frame_size) /
                                 ist->dec_ctx->sample_rate;
                break;
            case AVMEDIA_TYPE_VIDEO:
                if (ist->framerate.num) {
                    // TODO: Remove work-around for c99-to-c89 issue 7
                    AVRational time_base_q = AV_TIME_BASE_Q;
                    int64_t next_dts = av_rescale_q(ist->next_dts, time_base_q, av_inv_q(ist->framerate));
                    ist->next_dts = av_rescale_q(next_dts + 1, av_inv_q(ist->framerate), time_base_q);
                } else if (pkt.duration) {
                    ist->next_dts += av_rescale_q(pkt.duration, ist->st->time_base, AV_TIME_BASE_Q);
                } else if(ist->dec_ctx->framerate.num != 0) {
                    int ticks= av_stream_get_parser(ist->st) ? av_stream_get_parser(ist->st)->repeat_pict + 1 : ist->dec_ctx->ticks_per_frame;
                    ist->next_dts += ((int64_t)AV_TIME_BASE *
                                      ist->dec_ctx->framerate.den * ticks) /
                                      ist->dec_ctx->framerate.num / ist->dec_ctx->ticks_per_frame;
                }
                break;
            }
            ist->pts = ist->dts;
            ist->next_pts = ist->next_dts;
            OutputStream *ost = output_streams[pkt.stream_index];
            do_streamcopy(ist, ost, &pkt);   
        }



       

        


    }
   
    return 0;
}


