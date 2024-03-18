#include <cstdlib>
#include <iostream>
#include <stdlib.h>

#include "exampleConfig.h"

#include "calculator/main.h"
/*
 * Simple main program that demontrates how access
 * CMake definitions (here the version number) from source code.
 */
int main(int argc, char *argv[]) {
  if (argc != 3) {
    std::cout << "C++ Boiler Plate v" << PROJECT_VERSION_MAJOR << "."
              << PROJECT_VERSION_MINOR << "." << PROJECT_VERSION_PATCH << "."
              << PROJECT_VERSION_TWEAK << std::endl;
    return 0;
  }

  Calculator calc = Calculator();
  std::cout << calc.Sum(atoi(argv[1]), atoi(argv[2])) << std::endl;
  return 1;
}
