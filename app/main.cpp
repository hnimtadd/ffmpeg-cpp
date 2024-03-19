#include <iostream>
// FFmpeg
extern "C" {
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
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

  avformat_close_input(&inctx);
  return 0;
}
