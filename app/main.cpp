#include "miniocpp/providers.h"
#include "miniocpp/request.h"
#include "miniocpp/response.h"
#include "utils/error.h"
#include "utils/queue.h"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <istream>
#include <memory>
#include <miniocpp/args.h>
#include <miniocpp/client.h>
#include <miniocpp/credentials.h>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <streambuf>
#include <string>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <vector>
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

#define STREAM_FRAME_RATE 25
#define STREAM_DURATION 10.0
#define STREAM_CODEC AV_CODEC_ID_H264
bool end = false;
std::mutex locker;
std::condition_variable cVar;

std::vector<std::thread> thread_pool;
struct membuf : std::streambuf {
  membuf(char const *base, size_t size) {
    char *p(const_cast<char *>(base));
    this->setg(p, p, p + size);
  }
};

struct imemstream : virtual membuf, std::istream {
  imemstream(char const *base, size_t size)
      : membuf(base, size), std::istream(static_cast<std::streambuf *>(this)) {}
};

typedef struct ReadInterval {
  int id;             ///< identifier
  int64_t start, end; ///< start, end in second/AV_TIME_BASE units
  int has_start, has_end;
  int start_is_offset, end_is_offset;
  int duration_frames;
} ReadInterval;

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

static int read_interval_packets(AVCodecContext *codecCtx,
                                 AVFormatContext *inFmtCtx, AVStream *in_stream,
                                 ReadInterval *interval, int64_t *cur_ts,
                                 std::function<void(cv::Mat)> callback) {
  AVFormatContext *fmt_ctx = inFmtCtx;
  AVPacket *pkt = NULL;
  AVFrame *frame = NULL;
  int ret = 0, i = 0, frame_count = 0;
  int64_t start = -INT64_MAX, end = interval->end;
  int has_start = 0, has_end = interval->has_end && !interval->end_is_offset;
  int width, height;
  width = in_stream->codecpar->width;
  height = in_stream->codecpar->height;

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

      /* log_packet(inFmtCtx, pkt, "in"); */

      ret = avcodec_send_packet(codecCtx, pkt);
      if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
        } else if (ret == AVERROR_EOF) {
          std::cout
              << " the decoder has been flushed, and no new packets can be "
                 "sent "
                 "to it( also returned if more than 1 flush packet is sent) "
              << std::endl;
        } else if (ret == AVERROR(EINVAL)) {
          std::cout << "codec not opened, it is an "
                       "encoder, requires flush "
                    << std::endl;
        } else if (ret == AVERROR(ENOMEM)) {
          std::cout << "failed to add packet to internal queue, or similar"
                    << std::endl;
        } else {
          std::cout
              << " another negative error code legitimate decoding errors "
              << std::endl;
        }
        break;
      }

      AVFrame *frame = av_frame_alloc();
      ret = avcodec_receive_frame(codecCtx, frame);
      if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
          /* std::cout << "output is not available in this state - user must try
           * " */
          /*              "to send new input  " */
          /*           << std::endl; */
          av_frame_unref(frame);
          av_freep(frame);
          continue;
        } else if (ret == AVERROR_EOF) {
          std::cout
              << " the codec has been fully flushed, and there will be no "
                 "more output frames"
              << std::endl;
        } else if (ret == AVERROR(EINVAL)) {
          std::cout << "codec not opened, or it is an encoder without the @ref "
                       "AV_CODEC_FLAG_RECON_FRAME flag enabled"
                    << std::endl;
        } else {
          std::cout << "other negative error code legitimate decoding errors"
                    << std::endl;
        }
        break;
      }
      /* if (frame_count % 50 != 0) { */
      /*   continue; */
      /* } */

      std::cout << "decode" << std::endl;
      uint8_t *rValue = frame->data[0];
      uint8_t *gValue = frame->data[1];
      uint8_t *bValue = frame->data[2];

      uint8_t *arr = (uint8_t *)malloc(height * width * 3 * sizeof(uint8_t));

      memcpy(arr, gValue, width * height);
      memcpy(arr + width * height, bValue, width * height);
      memcpy(arr + 2 * width * height, rValue, width * height);

      cv::Mat imageG = cv::Mat(height, width, CV_8UC1, frame->data[0]);
      cv::Mat imageB = cv::Mat(height, width, CV_8UC1, frame->data[1]);
      cv::Mat imageR = cv::Mat(height, width, CV_8UC1, frame->data[2]);
      cv::Mat matRGB;

      cv::merge(std::vector<cv::Mat>{imageB, imageG, imageR}, matRGB);
      /* cv::Mat image = cv::Mat(height, width, CV_8UC3, matRGB.data); */
      /* ret = av_write(AVFormatContext *s, AVPacket *pkt) */
      /* avcodec_send_packet(AVCodecContext * avctx, const AVPacket *avpkt) */

      /* pkt->stream_index = out_stream->index; */
      /* int ret = av_interleaved_write_frame(outFmtCtx, pkt); */
      /* if (ret < 0) { */
      /*   HandleError(ret, "cannot av_interleaved_write_frame"); */
      /* } */
      std::unique_ptr<cv::Mat> ptr = std::make_unique<cv::Mat>(matRGB);

      thread_pool.emplace_back(std::thread(callback, *ptr));
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

int readloop(int argc, char *argv[], std::function<void(cv::Mat)> callback) {
  char *infile;
  AVFormatContext *inFmtCtx;
  const AVInputFormat *inFmt;
  const AVCodec *inCodec;
  AVStream *in_stream;
  AVCodecContext *inCodecCtx;
  int ret;
  int64 cur_ts;
  ReadInterval interval = (ReadInterval){.has_start = 0, .has_end = 0};
  const AVCodec *pCodec;
  AVCodecContext *pCodecCtx;
  int width, height;
  AVDictionary *option = nullptr;

  if (argc < 2) {
    std::cout << "Usage:" << argv[0] << "<in_rtsp_url>" << std::endl;
    goto exitLoop;
  }

  infile = argv[1];
  avformat_network_init();

  inFmtCtx = avformat_alloc_context();

  av_dict_set(&option, "rtsp_transport", "tcp", 0);
  av_dict_set(&option, "re", "", 0);

  inFmt = av_find_input_format("rtsp");
  // open input file context
  ret = avformat_open_input(&inFmtCtx, infile, inFmt, &option);
  if (ret < 0) {
    HandleError(ret, "fail to avforamt_open_input");
    goto exitLoop;
  }

  inFmtCtx->flags |= AVFMT_FLAG_GENPTS;
  // retrive input stream information
  ret = avformat_find_stream_info(inFmtCtx, NULL);
  if (ret < 0) {
    HandleError(ret, "fail to avformat_find_stream_info");
    goto exitLoop;
  }
  av_dump_format(inFmtCtx, 0, infile, 0);

  // find primary video stream
  ret = av_find_best_stream(inFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &inCodec, 1);
  if (ret < -2) {
    std::cerr << "fail to av_find_best_stream: ret=" << ret;
    goto exitLoop;
  }
  std::cout << "first video stream at index " << ret << " found\n";

  // in_stream is video stream of input;
  in_stream = inFmtCtx->streams[ret];
  inCodec = avcodec_find_decoder(in_stream->codecpar->codec_id);
  inCodecCtx = avcodec_alloc_context3(inCodec);
  ret = avcodec_parameters_to_context(inCodecCtx, in_stream->codecpar);
  if (ret < 0) {
    HandleError(ret, "cannot avcodec_open2 inCodecCtx");
    goto exitLoop;
  }

  ret = avcodec_open2(inCodecCtx, inCodec, NULL);
  if (ret < 0) {
    HandleError(ret, "cannot avcodec_open2 inCodecCtx");
    goto exitLoop;
  }

  width = in_stream->codecpar->width;
  height = in_stream->codecpar->height;
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
    HandleError(ret, "cannot start read from input");
    goto exitLoop;
  }
  pCodec = avcodec_find_decoder(in_stream->codecpar->codec_id);
  pCodecCtx = avcodec_alloc_context3(pCodec);
  ret = avcodec_parameters_to_context(pCodecCtx, in_stream->codecpar);
  if (ret < 0) {
    std::cout << ret << std::endl;
    HandleError(ret, "cannot avcodec_parameters_to_context");
    goto exitLoop;
  }

  ret = avcodec_open2(pCodecCtx, pCodec, nullptr);
  if (ret < 0) {
    std::cout << ret << std::endl;
    HandleError(ret, "cannot avcodec_open");
    goto exitLoop;
  }

  cur_ts = in_stream->start_time;
  ret = read_interval_packets(pCodecCtx, inFmtCtx, in_stream, &interval,
                              &cur_ts, callback);
  if (ret < 0) {
    HandleError(ret, "cannot read_interval_packets");
    goto exitLoop;
  }
  ret = av_read_pause(inFmtCtx);

exitLoop:
  avformat_close_input(&inFmtCtx);
  std::lock_guard lk(locker);
  end = true;
  std::cout << "exit loop\n";
  cVar.notify_one();
  return 0;
}

int main(int argc, char *argv[]) {
  // Create S3 base URL.
  std::function<void(cv::Mat)> callback;

  minio::s3::BaseUrl base_url("localhost", false);
  base_url.port = 9000;

  minio::creds::StaticProvider provider("admin", "secret_password");
  minio::s3::Client client(base_url, &provider);
  minio::s3::ListBucketsResponse resp = client.ListBuckets();
  if (200 < resp.status_code || resp.status_code > 299) {
    std::cerr << "could not init client" << std::endl;
    exit(1);
  }
  /* std::queue<std::vector<uchar>> queue; */
  concurrent_queue<std::vector<uchar>> buffCh;
  concurrent_counter counter;

  std::function<void()> minioReadLoop = [&client, &buffCh, &counter]() -> void {
    std::cout << "start minio reader...\n";
    while (1) {
      if (end) {
        return;
      }
      std::cout << "minio waiting...\n";
      std::vector<uchar> buff;
      buffCh.wait_and_pop(buff);
      imemstream stream(reinterpret_cast<const char *>(buff.data()),
                        buff.size());

      minio::s3::PutObjectArgs args(stream, buff.size(), 0);
      int index = counter.get();

      args.bucket = "my-bucket";
      args.object = "my-object-" + std::to_string(index) + ".jpg";
      args.content_type = "image/jpg";

      minio::s3::PutObjectResponse resp = client.PutObject(args);

      // Handle response.
      if (resp) {
        std::cout << "my-object is successfully created" << std::endl;
      } else {
        std::cout << "unable to do put object; " << resp.Error().String()
                  << std::endl;
      }
      std::cout << "published\n";
    }
  };

  callback = [&client, &buffCh](cv::Mat mat) -> void {
    std::vector<uchar> buff;
    std::vector<int> param(2);
    param[0] = cv::IMWRITE_JPEG_QUALITY;
    param[1] = 80; // default(95) 0-100
    bool ok = cv::imencode(".jpg", mat, buff, param);

    buffCh.push(buff);
  };

  std::thread readRTSPThread(readloop, argc, argv, callback);
  // wait for first thread done
  std::thread readMinioThread(minioReadLoop);
  std::unique_lock lk(locker);

  std::cout << "waiting for end\n";
  cVar.wait(lk, [] { return end; });
  std::cout << "end\n";
  std::terminate();
  return 1;
}
