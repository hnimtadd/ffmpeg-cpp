#include <iostream>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>

using namespace cv;

int main(int argc, char *argv[]) {
  if (argc == 1) {
    std::cout << "Usage: " << std::endl
              << "\t- ./main /image/path" << std::endl;
    return 0;
  }
  std::string image_path = samples::findFile(argv[1]);
  Mat img = imread(image_path, IMREAD_COLOR);

  if (img.empty()) {
    std::cout << "Could not read the image: " << image_path << std::endl;
    return 1;
  }

  imshow("Display window", img);
  int k = waitKey(0); // Wait for a keystroke in the window

  if (k == 's') {
    imwrite(argv[1], img);
  }

  return 0;
}
