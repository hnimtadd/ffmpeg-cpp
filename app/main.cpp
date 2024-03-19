#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <libavutil/pixfmt.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
// FFmpeg
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
// OpenCV
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cout << "Usage:" << argv[0] << "<infile>" << std::endl;
    return 1;
  }
  const char *infile = argv[1];
  int ret;
  AVFormatContext *inctx = nullptr;
  // open input file context
  ret = avformat_open_input(&inctx, infile, nullptr, nullptr);
  if (ret < 0) {
    std::cerr << "fail to avforamt_open_input(\"" << infile
              << "\"): ret=" << ret;
    return 2;
  }

  // retrive input stream information
  ret = avformat_find_stream_info(inctx, nullptr);
  if (ret < 0) {
    std::cerr << "fail to avformat_find_stream_info: ret=" << ret;
    return 2;
  }
  // find primary video stream
  const AVCodec *vcodec;
  ret = av_find_best_stream(inctx, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 1);
  if (ret < 0) {
    std::cerr << "fail to av_find_best_stream: ret=" << ret;
    return 2;
  }
  const int vstrm_idx = ret;
  AVStream *vstrm = inctx->streams[vstrm_idx];

  // print input video stream informataion
  std::cout << "infile: " << infile << "\n"
            << "format: " << inctx->iformat->name << "\n"
            << "vcodec: " << vcodec->name << "\n"
            << "size:   " << vstrm->codecpar->width << 'x'
            << vstrm->codecpar->height << "\n"
            << "fps:    " << av_q2d(vstrm->codecpar->framerate) << "\n"
            << "length: "
            << av_rescale_q(vstrm->duration, vstrm->time_base, {1, 1000}) /
                   1000.
            << " [sec]\n"
            << "frame:  " << vstrm->nb_frames << "\n"
            << std::flush;

  int width = vstrm->codecpar->width;
  int height = vstrm->codecpar->height;
  AVPacket *pkt = av_packet_alloc();
  if (pkt == NULL) {
    return 2;
  }

  const AVCodec *pCodec;
  pCodec = avcodec_find_decoder(vstrm->codecpar->codec_id);

  AVCodecContext *pCodecCtx = avcodec_alloc_context3(pCodec);
  ret = avcodec_parameters_to_context(pCodecCtx, vstrm->codecpar);
  if (ret < 0) {
    std::cout << ret << std::endl;
    return 2;
  }

  ret = avcodec_open2(pCodecCtx, pCodec, nullptr);
  if (ret < 0) {
    std::cout << ret << std::endl;
    return 2;
  }

  while (true) {
    ret = av_read_frame(inctx, pkt);

    if (pkt->stream_index != vstrm->index)
      continue;
    if (ret < 0) {
      if (ret == AVERROR_EOF) {
        std::cout << "read end" << std::endl;
      } else {
        std::cout << "fail to av_read_frame: ret=" << ret;
      }
      break;
    }

    ret = avcodec_send_packet(pCodecCtx, pkt);
    if (ret < 0) {
      if (ret == AVERROR(EAGAIN)) {
        std::cout << "input is not accepted in the current state - user must "
                     "read output with avcodec_receive_frame() (once all "
                     "output is read, the packet should be resent,  and the "
                     "call will not fail with EAGAIN).  "
                  << std::endl;
      } else if (ret == AVERROR_EOF) {
        std::cout
            << " the decoder has been flushed, and no new packets can be sent "
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
        std::cout << " another negative error code legitimate decoding errors "
                  << std::endl;
      }
      break;
    }

    AVFrame *frame = av_frame_alloc();
    ret = avcodec_receive_frame(pCodecCtx, frame);
    if (ret < 0) {
      if (ret == AVERROR(EAGAIN)) {
        std::cout << "output is not available in this state - user must try "
                     "to send new input  "
                  << std::endl;
        av_frame_unref(frame);
        av_freep(frame);
        continue;
      } else if (ret == AVERROR_EOF) {
        std::cout << " the codec has been fully flushed, and there will be no "
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

    std::cout << "decode" << std::endl;

    cv::Mat image(height, width, CV_8UC1, frame->data[0], frame->linesize[0]);

    cv::imshow("press ESC to exit", image);
    if (cv::waitKey(1) == 0x1b)
      break;
  }

  avcodec_free_context(&pCodecCtx);
  av_packet_unref(pkt);
  avformat_close_input(&inctx);
  return 0;
}
