#include "utils/error.h"
#import <iostream>

extern "C" {
#include <libavutil/error.h>
}

int HandleError(int ret, std::string msg) {
  std::cout << "error: " << msg << "\nret: " << ret
            << "\nmsg: " << av_err2str(ret) << std::endl;
  return 2;
}
