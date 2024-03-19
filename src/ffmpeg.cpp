#include <cstdint>
#include <cstdio>
#include <iostream>

#include <ffmpeg/ffmpeg.h>

extern "C" {
#include "libavcodec/codec.h"
#include "libavcodec/packet.h"
#include "libavutil/rational.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>

Client::Client() {}
void Client::ShowImage(char *path) {
  std::string image_path = cv::samples::findFile(path);
  cv::Mat img = cv::imread(image_path, cv::IMREAD_COLOR);
  std::cout << path << std::endl;

  if (img.empty()) {
    std::cout << "Could not read the image: " << image_path << std::endl;
    return;
  }

  imshow("Display window", img);
  int k = cv::waitKey(0); // Wait for a keystroke in the window

  if (k == 's') {
    imwrite(path, img);
  }

  return;
}
static void encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt,
                   FILE *outfile) {
  int ret;

  /* send the frame to the encoder */
  if (frame)
    printf("Send frame %3" PRId64 "\n", frame->pts);

  ret = avcodec_send_frame(enc_ctx, frame);
  if (ret < 0) {
    fprintf(stderr, "Error sending a frame for encoding\n");
    exit(1);
  }

  while (ret >= 0) {
    ret = avcodec_receive_packet(enc_ctx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      return;
    else if (ret < 0) {
      fprintf(stderr, "Error during encoding\n");
      exit(1);
    }

    printf("Write packet %3" PRId64 " (size=%5d)\n", pkt->pts, pkt->size);
    fwrite(pkt->data, 1, pkt->size, outfile);
    av_packet_unref(pkt);
  }
}

#define INBUF_SIZE 4096

void Client::StreamVideo(char *path, char *c_name) {
  const char *inFileName, *outFileName, *codec_name;
  const AVCodec *codec;
  int i, ret, x, y;
  FILE *outFile, *inFile;
  AVFrame *frame;
  AVPacket *pkt;
  AVCodecContext *ctx;
  uint8_t endcode[] = {0, 0, 1, 0xb8};
  codec_name = c_name;
  inFileName = path;
  outFileName = path;

  codec = avcodec_find_encoder_by_name(codec_name);
  if (!codec) {
    fprintf(stderr, "Codec '%s' not found\n", codec_name);
    exit(1);
  }

  ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    fprintf(stderr, "Could not allocate video codec context\n");
    exit(1);
  }

  pkt = av_packet_alloc();
  if (!pkt) {
    exit(1);
  }
  ctx->bit_rate = 400000;
  ctx->width = 352;
  ctx->height = 288;
  ctx->time_base = (AVRational){1, 25};
  ctx->framerate = (AVRational){25, 1};
  ctx->gop_size = 10;
  ctx->max_b_frames = 1;
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;

  if (codec->id == AV_CODEC_ID_H264) {
    av_opt_set(ctx->priv_data, "preset", "slow", 0);
  }
  ret = avcodec_open2(ctx, codec, NULL);
  if (ret < 0) {
    fprintf(stderr, "Could not open codec: %s\n", av_err2str(ret));
    exit(1);
  }
  outFile = fopen(outFileName, "wb");
  if (!outFile) {
    fprintf(stderr, "Could not open %s\n", outFileName);
    exit(1);
  }

  inFile = fopen(inFileName, "rb");
  if (!inFile) {
    fprintf(stderr, "Could not open %s\n", inFileName);
    exit(1);
  }

  frame = av_frame_alloc();
  if (!frame) {
    fprintf(stderr, "Could not allocate video frame\n");
    exit(1);
  }
  frame->format = ctx->pix_fmt;
  frame->width = ctx->width;
  frame->height = ctx->height;

  ret = av_frame_get_buffer(frame, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not allocate the video frame data\n");
    exit(1);
  }

  while (true) {
    fflush(stdout);

    /* Make sure the frame data is writable.
       On the first round, the frame is fresh from av_frame_get_buffer()
       and therefore we know it is writable.
       But on the next rounds, encode() will have called
       avcodec_send_frame(), and the codec may have kept a reference to
       the frame in its internal structures, that makes the frame
       unwritable.
       av_frame_make_writable() checks that and allocates a new buffer
       for the frame only if necessary.
     */
    ret = av_frame_make_writable(frame);
    if (ret < 0) {
      exit(1);
    }
    /* Prepare a dummy image.
       In real code, this is where you would have your own logic for
       filling the frame. FFmpeg does not care what you put in the
       frame.
     */
    /* Y */
    for (y = 0; y < ctx->height; y++) {
      for (x = 0; x < ctx->width; x++) {
        frame->data[0][y * frame->linesize[0] + x] = x + y + i * 3;
      }
    }

    /* Cb and Cr */
    for (y = 0; y < ctx->height / 2; y++) {
      for (x = 0; x < ctx->width / 2; x++) {
        frame->data[1][y * frame->linesize[1] + x] = 128 + y + i * 2;
        frame->data[2][y * frame->linesize[2] + x] = 64 + x + i * 5;
      }
    }

    frame->pts = i;

    /* encode the image */
    encode(ctx, frame, pkt, outFile);
  }

  /* flush the encoder */
  encode(ctx, NULL, pkt, outFile);

  /* Add sequence end code to have a real MPEG file.
     It makes only sense because this tiny examples writes packets
     directly. This is called "elementary stream" and only works for some
     codecs. To create a valid file, you usually need to write packets
     into a proper file format or protocol; see mux.c.
   */
  if (codec->id == AV_CODEC_ID_MPEG1VIDEO ||
      codec->id == AV_CODEC_ID_MPEG2VIDEO) {
    fwrite(endcode, 1, sizeof(endcode), outFile);
  }
  fclose(outFile);
  fclose(inFile);

  avcodec_free_context(&ctx);
  av_frame_free(&frame);
  av_packet_free(&pkt);
}
