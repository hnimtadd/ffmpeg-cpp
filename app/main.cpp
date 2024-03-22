#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/imgcodecs.hpp>
#include <string>
// FFmpeg
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
}
// OpenCV
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

#define STREAM_FRAME_RATE 25
#define STREAM_DURATION 10.0
#define SCALE_FLAGS SWS_BICUBIC

int HandleError(int ret, std::string msg) {
  std::cout << "error: " << msg << "\nret: " << ret
            << "\nmsg: " << av_err2str(ret) << std::endl;
  return 2;
}

//
// wrapper around a single output AVStream
typedef struct OutputStream {
  AVStream *stream;
  AVCodecContext *encCtx;

  /*pts of the next frame that will be generated*/
  int64_t next_pts;

  AVFrame *frame; //
  //
  // temporarily BGRP pix_fmt if the output frame is not BGRP
  AVFrame *tmp_frame;
  AVPacket *tmp_pkt;
  struct SwsContext *sws_ctx;
} OutputStream;

// wrapper around a single output AVStream
typedef struct InputStream {
  AVStream *stream;
  AVFormatContext *fmtCtx;
  AVCodecContext *decCtx;

  /*pts of the next frame that will be generated*/
  int64_t next_pts;

  AVFrame *frame; //
  //
  // temporarily BGRP pix_fmt if the output frame is not BGRP
  AVFrame *tmp_frame;

  AVPacket *tmp_pkt;

  struct SwsContext *sws_ctx;
} InputStream;

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt,
                       const char *tag) {
  AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

  printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s "
         "duration_time:%s stream_index:%d\n",
         tag, av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
         av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
         av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
         pkt->stream_index);
}

static int write_frame(AVFormatContext *fmt_ctx, AVCodecContext *c,
                       AVStream *st, AVFrame *frame, AVPacket *pkt) {
  int ret;

  // send the frame to the encoder
  ret = avcodec_send_frame(c, frame);
  if (ret < 0) {
    return ret;
  }

  // loop to get pkt from frame
  while (ret >= 0) {
    ret = avcodec_receive_packet(c, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    } else if (ret < 0) {
      return ret;
    }
    // rescale output packet timestamp values from codec to stream timebase
    av_packet_rescale_ts(pkt, c->time_base, st->time_base);
    pkt->stream_index = st->index;

    // write the compressed packet to the media output
    ret = av_interleaved_write_frame(fmt_ctx, pkt);
    if (ret < 0) {
      return ret;
    }
  }
  return ret == AVERROR_EOF ? 1 : ret;
};

/* Add and out ouput stream */
static void add_stream(OutputStream *ost, AVFormatContext *oc,
                       const AVCodec **codec, enum AVCodecID codec_id,
                       int width, int height) {
  AVCodecContext *c;
  int i;

  *codec = avcodec_find_encoder(codec_id);
  if (!(*codec)) {
    std::cout << "could not find encoder for " << avcodec_get_name(codec_id)
              << std::endl;
    exit(1);
  }

  ost->stream = avformat_new_stream(oc, NULL);

  if (!ost->stream) {
    std::cout << "could not allocate stream\n";
  }
  exit(1);

  ost->stream->id = oc->nb_streams - 1;
  c = avcodec_alloc_context3(*codec);
  if (!c) {
    std::cout << "could not allocate and encoding context\n";
    exit(1);
  }

  ost->encCtx = c;
  switch ((*codec)->type) {
  case AVMEDIA_TYPE_VIDEO:
    c->codec_id = codec_id;
    c->bit_rate = 4000000;
    c->height = height;
    c->width = width;

    ost->stream->time_base = (AVRational){1, STREAM_FRAME_RATE};
    c->time_base = ost->stream->time_base;
    c->pix_fmt = AV_PIX_FMT_GBRP;

  default:
    std::cout << "unsupport codec" << (*codec)->type << std::endl;
    exit(1);
  }
}

static AVFrame *alloc_frame(enum AVPixelFormat pix_fmt, int width, int height) {
  AVFrame *frame;
  int ret;

  frame = av_frame_alloc();
  if (!frame) {
    return NULL;
  }

  frame->format = pix_fmt;
  frame->width = width;
  frame->height = height;

  ret = av_frame_get_buffer(frame, 0);
  if (ret < 0) {
    std::cout << "could not allocate frame data" << std::endl;
    exit(1);
  }
  return frame;
}

static void open_video(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg) {
  int ret;
  AVCodecContext *c = ost->encCtx;
  AVDictionary *opt = NULL;
  av_dict_copy(&opt, opt_arg, 0);

  /* open the codec */

  ret = avcodec_open2(c, codec, &opt);
  av_dict_free(&opt);
  if (ret < 0) {
    HandleError(ret, "could not open video codec");
    exit(1);
  }

  /* allocate and init a re-usable frame */
  ost->frame = alloc_frame(c->pix_fmt, c->width, c->height);
  if (!ost->frame) {
    std::cout << "could not allocate video frame\n";
    exit(1);
  }

  ost->tmp_frame = NULL;
  if (c->pix_fmt != AV_PIX_FMT_BGR0) {
    ost->tmp_frame = alloc_frame(AV_PIX_FMT_BGR0, c->width, c->height);
    if (!ost->tmp_frame) {
      std::cout << "could not allocate temporary video frame\n";
      exit(1);
    }
  }

  ret = avcodec_parameters_from_context(ost->stream->codecpar, c);
  if (ret < 0) {
    HandleError(ret, "could not copy the stream parameters");
    exit(1);
  }
}

static AVFrame *get_video_frame(OutputStream *ost) {
  AVCodecContext *c = ost->encCtx;
  if (av_compare_ts(ost->next_pts, c->time_base, STREAM_DURATION,
                    (AVRational){1, 1}) > 0) {
    return NULL;
  }

  /* when we pass a frame to the encoder, it may keep a reference to it
   * internally*/
  if (av_frame_make_writable(ost->frame) < 0)
    exit(1);

  if (c->pix_fmt != AV_PIX_FMT_BGR0) {
    /* convert frame pic to the pixel_fmt if needed*/
    if (!ost->sws_ctx) {
      ost->sws_ctx =
          sws_getContext(c->width, c->height, AV_PIX_FMT_BGR0, c->width,
                         c->height, c->pix_fmt, SCALE_FLAGS, NULL, NULL, NULL);
      if (!ost->sws_ctx) {
        std::cout << "could not initialized the conversion context\n";
        exit(1);
      }
      /* fill the tmp_frame with readed frame*/
    }
    sws_scale(ost->sws_ctx, (const uint8_t *const *)ost->tmp_frame->data,
              ost->tmp_frame->linesize, 0, c->height, ost->frame->data,
              ost->frame->linesize);
  } else {
    /*fill the ost->frame with readed frame*/
  }

  ost->frame->pts = ost->next_pts++;
  return ost->frame;
}

static int write_video_frame(AVFormatContext *oc, OutputStream *ost) {
  return write_frame(oc, ost->encCtx, ost->stream, get_video_frame(ost),
                     ost->tmp_pkt);
}

static void close_stream(AVFormatContext *oc, OutputStream *ost) {
  avcodec_free_context(&ost->encCtx);
  av_frame_free(&ost->frame);
  av_frame_free(&ost->tmp_frame);
  av_packet_free(&ost->tmp_pkt);
  sws_freeContext(ost->sws_ctx);
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::cout << "Usage:" << argv[0] << "<infile>"
              << "<out_rtsp_url>" << std::endl;
    return 1;
  }
  const char *infile = argv[1];
  const char *outfile = argv[2];
  AVFormatContext *outFmtCtx, *inFmtCtx;
  /* AVCodecContext *outCodecCtx, *inCodecCtx; */
  const AVCodec *inCodec, *outCodec;
  AVStream *in_stream, *out_stream;

  int flush, ret;
  inFmtCtx = avformat_alloc_context();
  /* outFmt = outFmtCtx->oformat; */

  // open input file context
  ret = avformat_open_input(&inFmtCtx, infile, NULL, NULL);
  if (ret < 0) {
    return HandleError(ret, "fail to avforamt_open_input");
    return 2;
  }
  // retrive input stream information
  ret = avformat_find_stream_info(inFmtCtx, NULL);
  if (ret < 0) {
    std::cerr << "fail to avformat_find_stream_info: ret=" << ret;
    return 2;
  }
  av_dump_format(inFmtCtx, 0, infile, 0);

  // find primary video stream
  ret = av_find_best_stream(inFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &inCodec, 1);
  if (ret < 0) {
    std::cerr << "fail to av_find_best_stream: ret=" << ret;
    return 2;
  }

  // in_stream is video stream of input;
  in_stream = inFmtCtx->streams[ret];

  ret = avformat_alloc_output_context2(&outFmtCtx, NULL, "rtsp", outfile);
  if (ret < 0) {
    return HandleError(ret, "failed to avformat_alloc_output_context2");
  }

  if (!outFmtCtx) {
    std::cout << "could not create output context\n";
    return 2;
  }

  out_stream = avformat_new_stream(outFmtCtx, NULL);
  if (!out_stream) {
    std::cout << "failed to allocating output stream\n";
    return 1;
  }
  out_stream = outFmtCtx->streams[0];

  ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
  if (ret < 0) {
    std::cout << "failed to copy codec parameters\n";
    return 1;
  }
  out_stream->codecpar->codec_tag = 0;
  av_dump_format(outFmtCtx, 0, outfile, 1);
  if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&outFmtCtx->pb, outfile, AVIO_FLAG_WRITE);
    if (ret < 0) {
      return HandleError(ret, "failed to avio_open");
      return 1;
    }
  }

  ret = avformat_write_header(outFmtCtx, NULL);
  if (ret < 0) {
    return HandleError(ret, "failed to avformat_write_header");
  }

  int width = in_stream->codecpar->width;
  int height = in_stream->codecpar->height;
  // print input video stream informataion
  std::cout << "infile: " << infile << "\n"
            << "format: " << inFmtCtx->iformat->name << "\n"
            << "vcodec: " << inCodec->name << "\n"
            << "size:   " << in_stream->codecpar->width << 'x'
            << in_stream->codecpar->height << "\n"
            << "stream index" << in_stream->index << "\n"
            << "fps:    " << av_q2d(in_stream->codecpar->framerate) << "\n"
            << "length: "
            << av_rescale_q(in_stream->duration, in_stream->time_base,
                            {1, 1000}) /
                   1000.
            << " [sec]\n"
            << "frame:  " << in_stream->nb_frames << "\n"
            << std::flush;

  /* inCodec = avcodec_find_decoder(in_stream->codecpar->codec_id); */
  /* inCodecCtx = avcodec_alloc_context3(NULL); */

  /* ret = avcodec_parameters_to_context(inCodecCtx, in_stream->codecpar); */
  /* if (ret < 0) { */
  /*   std::cout << ret << std::endl; */
  /*   return 2; */
  /* } */

  /* ret = avcodec_open2(inCodecCtx, inCodec, nullptr); */
  /* if (ret < 0) { */
  /*   std::cout << ret << std::endl; */
  /*   return 2; */
  /* } */



  AVPacket *pkt = av_packet_alloc();
  if (!pkt) {
    return 2;
  }

  while (true) {
    ret = av_read_frame(inFmtCtx, pkt);
    if (ret < 0) {
      break;
    }

    if (pkt->stream_index != in_stream->index){
      goto continueLoop;
    }

    pkt->stream_index = out_stream->index;
    log_packet(inFmtCtx, pkt, "in");

    av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
    pkt->pos = -1;
    log_packet(outFmtCtx, pkt, "out");

    ret = av_interleaved_write_frame(outFmtCtx, pkt);
    if (ret < 0) {
      return HandleError(ret, "could not write frame to outfmtCtx");
    }

    av_write_trailer(outFmtCtx);

    /* ret = avcodec_send_packet(inCodecCtx, pkt); */
    /* if (ret < 0) { */
    /*   if (ret == AVERROR(EAGAIN)) { */
    /*     HandleError(ret, */
    /*                 "input is not accepted in the current state user must " */
    /*                 "read output with avcodec_receive_frame() (once all " */
    /*                 "output is read, the packet should be resent,  and the " */
    /*                 "call will not fail with EAGAIN).  "); */
    /*   } else if (ret == AVERROR_EOF) { */
    /*     HandleError( */
    /*         ret, " the decoder has been flushed, and no new packets can be " */
    /*              "sent " */
    /*              "to it( also returned if more than 1 flush packet is sent) "); */
    /*     continue; */
    /*   } else if (ret == AVERROR(EINVAL)) { */
    /*     HandleError(ret, "codec not opened, it is an " */
    /*                      "encoder, requires flush "); */
    /*   } else if (ret == AVERROR(ENOMEM)) { */
    /*     HandleError(ret, "failed to add packet to internal queue, or similar"); */
    /*   } else { */
    /*     return HandleError( */
    /*         ret, " another negative error code legitimate decoding errors "); */
    /*   } */
    /*   break; */
    /* } */


    /* AVFrame *frame = av_frame_alloc(); */
    /* ret = avcodec_receive_frame(inCodecCtx, frame); */
    /* if (ret < 0) { */
    /*   if (ret == AVERROR(EAGAIN)) { */
    /*     HandleError(ret, */
    /*                 "output is not available in this state - user must try " */
    /*                 "to send new input  "); */
    /*     av_frame_unref(frame); */
    /*     av_freep(frame); */
    /*     continue; */
    /*   } else if (ret == AVERROR_EOF) { */
    /*     HandleError(ret, */
    /*                 " the codec has been fully flushed, and there will be no " */
    /*                 "more output frames"); */
    /*   } else if (ret == AVERROR(EINVAL)) { */
    /*     HandleError(ret, */
    /*                 "codec not opened, or it is an encoder without the @ref " */
    /*                 "AV_CODEC_FLAG_RECON_FRAME flag enabled"); */
    /*   } else { */
    /*     HandleError(ret, */
    /*                 "other negative error code legitimate decoding errors"); */
    /*   } */
    /*   break; */
    /* } */
    /* if (ret < 0) { */
    /*   HandleError(ret, "cannot write frame"); */
    /*   break; */
    /* } */
    /**/
    /* std::cout << "decode" << std::endl; */
    /* uint8_t *rValue = frame->data[0]; */
    /* uint8_t *gValue = frame->data[1]; */
    /* uint8_t *bValue = frame->data[2]; */
    /* uint8_t *arr = (uint8_t *)malloc(height * width * 3 * sizeof(uint8_t)); */
    /* memcpy(arr, gValue, width * height); */
    /* memcpy(arr + width * height, bValue, width * height); */
    /* memcpy(arr + 2 * width * height, rValue, width * height); */
    /**/
    /* cv::Mat imageG = cv::Mat(height, width, CV_8UC1, frame->data[0]); */
    /* cv::Mat imageB = cv::Mat(height, width, CV_8UC1, frame->data[1]); */
    /* cv::Mat imageR = cv::Mat(height, width, CV_8UC1, frame->data[2]); */
    /* cv::Mat matRGB; */
    /* cv::merge(std::vector<cv::Mat>{imageB, imageG, imageR}, matRGB); */
    /* cv::imshow("press ESC to exit", matRGB); */
    /* if (cv ::waitKey(0) == 27) { */
    /*   break; */
    /* } */
    continueLoop:
    av_packet_unref(pkt);
  }
  /**/
  /* avcodec_free_context(&inCodecCtx); */
  /* avcodec_free_context(&outCodecCtx); */
  av_packet_unref(pkt);
  avformat_close_input(&inFmtCtx);
  avformat_close_input(&outFmtCtx);
  return 0;
}

/* int main(int argc, char *argv[]) { */
/*   const AVOutputFormat *ofmt = NULL; */
/*   AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL; */
/*   AVPacket *pkt = NULL; */
/*   const char *in_filename, *out_filename; */
/*   int ret, i; */
/*   int stream_index = 0; */
/*   int *stream_mapping = NULL; */
/*   int stream_mapping_size = 0; */
/**/
/*   if (argc < 3) { */
/*     printf("usage: %s input output\n" */
/*            "API example program to remux a media file with
 * libavformat and
 * " */
/*            "libavcodec.\n" */
/*            "The output format is guessed according to the file
 * extension.\n" */
/*            "\n", */
/*            argv[0]); */
/*     return 1; */
/*   } */
/**/
/*   in_filename = argv[1]; */
/*   out_filename = argv[2]; */
/**/
/*   pkt = av_packet_alloc(); */
/*   if (!pkt) { */
/*     fprintf(stderr, "Could not allocate AVPacket\n"); */
/*     return 1; */
/*   } */
/**/
/*   if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0))
   < 0) { */
/*     fprintf(stderr, "Could not open input file '%s'",
   in_filename); */
/*     goto end; */
/*   } */
/**/
/*   if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) { */
/*     fprintf(stderr, "Failed to retrieve input stream
   information"); */
/*     goto end; */
/*   } */
/**/
/*   av_dump_format(ifmt_ctx, 0, in_filename, 0); */
/**/
/*   avformat_alloc_output_context2(&ofmt_ctx, NULL, "rtsp",
   out_filename); */
/*   if (!ofmt_ctx) { */
/*     avformat_alloc_output_context2(&ofmt_ctx, NULL, "mpeg",
 * out_filename);
 */
/*     ret = AVERROR_UNKNOWN; */
/*     goto end; */
/*   } */
/**/
/*   stream_mapping_size = ifmt_ctx->nb_streams; */
/*   stream_mapping = */
/*       (int *)av_calloc(stream_mapping_size,
   sizeof(*stream_mapping)); */
/*   if (!stream_mapping) { */
/*     ret = AVERROR(ENOMEM); */
/*     goto end; */
/*   } */
/**/
/*   ofmt = ofmt_ctx->oformat; */
/**/
/*   for (i = 0; i < ifmt_ctx->nb_streams; i++) { */
/*     AVStream *out_stream; */
/*     AVStream *in_stream = ifmt_ctx->streams[i]; */
/*     AVCodecParameters *in_codecpar = in_stream->codecpar; */
/**/
/*     if (in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO) { */
/*       stream_mapping[i] = -1; */
/*       continue; */
/*     } */
/**/
/*     stream_mapping[i] = stream_index++; */
/**/
/*     out_stream = avformat_new_stream(ofmt_ctx, NULL); */
/*     if (!out_stream) { */
/*       fprintf(stderr, "Failed allocating output stream\n"); */
/*       ret = AVERROR_UNKNOWN; */
/*       goto end; */
/*     } */
/**/
/*     ret = avcodec_parameters_copy(out_stream->codecpar,
   in_codecpar); */
/*     if (ret < 0) { */
/*       fprintf(stderr, "Failed to copy codec parameters\n"); */
/*       goto end; */
/*     } */
/*     out_stream->codecpar->codec_tag = 0; */
/*   } */
/*   av_dump_format(ofmt_ctx, 0, out_filename, 1); */
/**/
/*   if (!(ofmt->flags & AVFMT_NOFILE)) { */
/*     ret = avio_open(&ofmt_ctx->pb, out_filename,
   AVIO_FLAG_WRITE); */
/*     if (ret < 0) { */
/*       fprintf(stderr, "Could not open output file '%s'",
   out_filename); */
/*       goto end; */
/*     } */
/*   } */
/**/
/*   ret = avformat_write_header(ofmt_ctx, NULL); */
/*   if (ret < 0) { */
/*     fprintf(stderr, "Error occurred when opening output
   file\n"); */
/*     goto end; */
/*   } */
/**/
/*   while (1) { */
/*     AVStream *in_stream, *out_stream; */
/**/
/*     ret = av_read_frame(ifmt_ctx, pkt); */
/*     if (ret < 0) */
/*       break; */
/**/
/*     in_stream = ifmt_ctx->streams[pkt->stream_index]; */
/*     if (pkt->stream_index >= stream_mapping_size || */
/*         stream_mapping[pkt->stream_index] < 0) { */
/*       av_packet_unref(pkt); */
/*       continue; */
/*     } */
/**/
/*     pkt->stream_index = stream_mapping[pkt->stream_index]; */
/*     out_stream = ofmt_ctx->streams[pkt->stream_index]; */
/*     log_packet(ifmt_ctx, pkt, "in"); */
/**/
/* copy packet  */
/*     av_packet_rescale_ts(pkt, in_stream->time_base,
  out_stream->time_base); */
/*     pkt->pos = -1; */
/*     log_packet(ofmt_ctx, pkt, "out"); */
/**/
/*     ret = av_interleaved_write_frame(ofmt_ctx, pkt); */
/*      pkt is now blank (av_interleaved_write_frame() takes
   ownership of */
/*      * its contents and resets pkt), so that no unreferencing is
   necessary. */
/*      * This would be different if one used av_write_frame(). */
/*     if (ret < 0) { */
/*       fprintf(stderr, "Error muxing packet\n"); */
/*       break; */
/*     } */
/*   } */
/**/
/*   av_write_trailer(ofmt_ctx); */
/* end: */
/*   av_packet_free(&pkt); */
/**/
/*   avformat_close_input(&ifmt_ctx); */
/**/
/*    close output  */
/*   if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE)) */
/*     avio_closep(&ofmt_ctx->pb); */
/*   avformat_free_context(ofmt_ctx); */
/**/
/*   av_freep(&stream_mapping); */
/**/
/*   if (ret < 0 && ret != AVERROR_EOF) { */
/*     fprintf(stderr, "Error occurred: %s\n", av_err2str(ret)); */
/*     return 1; */
/*   } */
/**/
/*   return 0; */
/* } */
