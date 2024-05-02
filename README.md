## Structure
``` text
.
├── CMakeLists.txt
├── app
│   └── main.cpp
├── include
│   ├── example.h
│   └── exampleConfig.h.in
├── src
│   └── example.cpp
└── tests
    ├── dummy.cpp
    └── main.cpp
```

Sources go in [src/](src/), header files in [include/](include/), and main programs in [app/](app).

## Building

Build by making a build directory (i.e. `build/`), run `cmake` in that dir, and then use `make` to build the desired target, or run pre-defined command in makefile.

Example:

```bash
make bin
```
## Running:
After build the project, binary file at `build/main`. Run this example with:
```bash
./build/main

Usage:    ./build/main <num_stream> <in_format> <in_file> <out_format> <out_file> (-> Read and write)
     or   ./build/main <num_stream> <in_format> <in_file>                         (-> Read only)
```

## .gitignore

The [.gitignore](.gitignore) file is a copy of the [Github C++.gitignore file](https://github.com/github/gitignore/blob/master/C%2B%2B.gitignore),
with the addition of ignoring the build directory (`build/`)

