#include <fstream>
#include <ios>
#include <iosfwd>
#include <iostream>
#include <opencv2/core/mat.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <vector>

int main(int argc, char *argv[]) {
  if (argc != 2) {
    return 0;
  }
  char *filename = argv[1];
  std::vector<char> buf;
  std::ifstream inFile(filename, std::ios_base::in | std::ios_base::binary);

  // copy from file to buf
  if (!inFile.fail() && !inFile.eof()) {
    inFile.seekg(0, std::ios_base::end);
    std::streampos fileSize = inFile.tellg();
    buf.resize(fileSize);
    inFile.seekg(0, std::ios_base::beg);
    inFile.read(&buf[0], fileSize);
  }

  cv::Mat imgMat = cv::imdecode((cv::InputArray)(buf), cv::IMREAD_COLOR);
  cv::imshow("image", imgMat);

  int k = cv::waitKey(0); // Wait for a keystroke in the window
  inFile.close();
  return 0;
}
