#include "ffmpeg/ffmpeg.h"
int main(int argc, char *argv[]) {
  if (argc != 2) {
    return 0;
  }
  Client c = Client();
  c.ShowImage(argv[1]);
  return 0;
}
