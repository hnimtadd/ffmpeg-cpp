/* #include <cerrno> */
/* #include <cstdint> */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/highgui.hpp>
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
#include <sys/time.h>

#define STREAM_FRAME_RATE 25
#define STREAM_DURATION 10.0
#define STREAM_CODEC AV_CODEC_ID_H264

time_t get_time() {
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return tv.tv_sec;
}

typedef struct ReadInterval {
  int id;             ///< identifier
  int64_t start, end; ///< start, end in second/AV_TIME_BASE units
  int has_start, has_end;
  int start_is_offset, end_is_offset;
  int duration_frames;
} ReadInterval;
int HandleError(int ret, std::string msg) {
  std::cout << "error: " << msg << "\nret: " << ret
            << "\nmsg: " << av_err2str(ret) << std::endl;
  return 2;
}

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

static int read_interval_packets(AVFormatContext *inFmtCtx, AVStream *in_stream,
                                 AVFormatContext *outFmtCtx,
                                 AVStream *out_stream, ReadInterval *interval,
                                 int64_t *cur_ts) {
  AVFormatContext *fmt_ctx = inFmtCtx;
  AVPacket *pkt = NULL;
  AVFrame *frame = NULL;
  int ret = 0, i = 0, frame_count = 0;
  int64_t start = -INT64_MAX, end = interval->end;
  int has_start = 0, has_end = interval->has_end && !interval->end_is_offset;

  av_log(NULL, AV_LOG_VERBOSE, "Processing read interval ");

  if (interval->has_start) {
    int64_t target;
    if (interval->start_is_offset) {
      if (*cur_ts == AV_NOPTS_VALUE) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not seek to relative position since current "
               "timestamp is not defined\n");
        ret = AVERROR(EINVAL);
        goto end;
      }
      target = *cur_ts + interval->start;
    } else {
      target = interval->start;
    }

    if ((ret = avformat_seek_file(fmt_ctx, -1, -INT64_MAX, target, INT64_MAX,
                                  0)) < 0) {
      av_log(NULL, AV_LOG_ERROR, "Could not seek to position %" PRId64 ": %s\n",
             interval->start, av_err2str(ret));
      goto end;
    }
  }

  pkt = av_packet_alloc();
  if (!pkt) {
    ret = AVERROR(ENOMEM);
    goto end;
  }
  while (!av_read_frame(fmt_ctx, pkt)) {
    if (pkt->stream_index == in_stream->index) {
      AVRational tb = in_stream->time_base;
      int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;

      if (pts != AV_NOPTS_VALUE)
        *cur_ts = av_rescale_q(pts, tb, AV_TIME_BASE_Q);

      if (!has_start && *cur_ts != AV_NOPTS_VALUE) {
        start = *cur_ts;
        has_start = 1;
      }

      if (has_start && !has_end && interval->end_is_offset) {
        end = start + interval->end;
        has_end = 1;
      }

      if (interval->end_is_offset && interval->duration_frames) {
        if (frame_count >= interval->end)
          break;
      } else if (has_end && *cur_ts != AV_NOPTS_VALUE && *cur_ts >= end) {
        break;
      }

      frame_count++;

      log_packet(inFmtCtx, pkt, "in");

      pkt->stream_index = out_stream->index;
      int ret = av_interleaved_write_frame(outFmtCtx, pkt);
      if (ret < 0) {
        HandleError(ret, "cannot av_interleaved_write_frame");
      }

      /* show_packet(w, ifile, pkt, i++); */ // get pkt here
      /* nb_streams_packets[pkt->stream_index]++; */
    }
    av_packet_unref(pkt);
  }
  av_packet_unref(pkt);
  // Flush remaining frames that are cached in the decoder

end:
  av_frame_free(&frame);
  av_packet_free(&pkt);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Could not read packets in interval ");
  }
  return ret;
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::cout << "Usage:" << argv[0] << "<in_rtsp_url>"
              << "<out_out_url>" << std::endl;
    return 1;
  }

  const char *infile, *outfile;
  AVFormatContext *outFmtCtx, *inFmtCtx;
  const AVOutputFormat *outFmt;
  const AVInputFormat *inFmt;
  const AVCodec *inCodec, *outCodec;
  AVStream *in_stream, *out_stream;
  AVCodecContext *inCodecCtx, *outCodecCtx;
  int ret;

  infile = argv[1];
  outfile = argv[2];
  avformat_network_init();

  inFmtCtx = avformat_alloc_context();

  AVDictionary *option = nullptr;
  av_dict_set(&option, "rtsp_transport", "tcp", 0);
  av_dict_set(&option, "re", "", 0);

  inFmt = av_find_input_format("rtsp");
  // open input file context
  ret = avformat_open_input(&inFmtCtx, infile, inFmt, &option);
  if (ret < 0) {
    return HandleError(ret, "fail to avforamt_open_input");
    return 2;
  }

  inFmtCtx->flags |= AVFMT_FLAG_GENPTS;
  // retrive input stream information
  ret = avformat_find_stream_info(inFmtCtx, NULL);
  if (ret < 0) {
    std::cerr << "fail to avformat_find_stream_info: ret=" << ret;
    return 2;
  }
  av_dump_format(inFmtCtx, 0, infile, 0);

  // find primary video stream
  ret = av_find_best_stream(inFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &inCodec, 1);
  if (ret < -2) {
    std::cerr << "fail to av_find_best_stream: ret=" << ret;
    return 2;
  }
  std::cout << "first video stream at index " << ret << " found\n";

  // in_stream is video stream of input;
  in_stream = inFmtCtx->streams[ret];
  inCodec = avcodec_find_decoder(in_stream->codecpar->codec_id);
  inCodecCtx = avcodec_alloc_context3(inCodec);
  ret = avcodec_parameters_to_context(inCodecCtx, in_stream->codecpar);
  if (ret < 0) {
    return HandleError(ret, "cannot avcodec_open2 inCodecCtx");
  }

  ret = avcodec_open2(inCodecCtx, inCodec, NULL);
  if (ret < 0) {
    return HandleError(ret, "cannot avcodec_open2 inCodecCtx");
  }
  outCodec = avcodec_find_encoder(STREAM_CODEC);

  outFmt = av_guess_format("hls", outfile, NULL);
  ret = avformat_alloc_output_context2(&outFmtCtx, outFmt, "hls", outfile);
  if (ret < 0) {
    return HandleError(ret, "failed to avformat_alloc_output_context2");
  }
  if (!outFmtCtx) {
    std::cout << "could not create output context\n";
    return 2;
  }

  outFmtCtx->video_codec_id = STREAM_CODEC;
  out_stream = avformat_new_stream(outFmtCtx, outCodec);
  if (!out_stream) {
    std::cout << "failed to allocating output stream\n";
    return 1;
  } else {
    out_stream->time_base = (AVRational){1, 90000};
  }

  ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
  if (ret < 0) {
    std::cout << "failed to copy codec parameters\n";
    return 1;
  }

  av_dump_format(outFmtCtx, 0, outfile, 1);
  if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&outFmtCtx->pb, outfile, AVIO_FLAG_WRITE);
    if (ret < 0) {
      return HandleError(ret, "failed to avio_open");
      return 1;
    }
  }

  outCodecCtx = avcodec_alloc_context3(outCodec);
  outCodecCtx->pix_fmt = AV_PIX_FMT_YUV422P;
  outCodecCtx->time_base = (AVRational){1, 90000};
  outCodecCtx->height = in_stream->codecpar->height;
  outCodecCtx->width = in_stream->codecpar->width;
  outCodecCtx->framerate = in_stream->codecpar->framerate;

  /* outCodecCtx->colorspace=AVColorSpace(); */
  ret = avcodec_open2(outCodecCtx, outCodec, NULL);
  if (ret < 0) {
    return HandleError(ret, "failed to avcodec_open2, outCodecCtx");
  }
  ret = avcodec_parameters_to_context(outCodecCtx, out_stream->codecpar);
  if (ret < 0) {
    return HandleError(ret,
                       "failed to avcodec_parameters_to_context, outCodecCtx");
  }

  AVDictionary *outOption;
  av_dict_set(&outOption, "hls_flags", "delete_segments", 0);
  ret = avformat_write_header(outFmtCtx, &outOption);
  if (ret < 0) {
    return HandleError(ret, "failed to avformat_write_header");
  }

  int width = in_stream->codecpar->width;
  int height = in_stream->codecpar->height;
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

  ret = av_read_play(inFmtCtx);
  if (ret < 0) {
    return HandleError(ret, "cannot start read from input");
  }

  ReadInterval interval = (ReadInterval){.has_start = 0, .has_end = 0};
  int64_t cur_ts = in_stream->start_time;
  ret = read_interval_packets(inFmtCtx, in_stream, outFmtCtx, out_stream,
                              &interval, &cur_ts);
  if (ret < 0) {
    return HandleError(ret, "cannot read_interval_packets");
  }

exit:
  ret = av_read_pause(inFmtCtx);
  if (ret < 0) {
    return HandleError(ret, "cannot start read from input");
  }
  av_write_trailer(outFmtCtx);
  avformat_close_input(&inFmtCtx);
  avformat_close_input(&outFmtCtx);
  return 0;
}
