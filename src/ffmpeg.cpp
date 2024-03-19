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

void Client::StreamVideo(char *path, char *c_name) {}
