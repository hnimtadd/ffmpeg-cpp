clean:
	rm -rf build/*

build: clean
	cmake -B build -S . "-DCMAKE_TOOLCHAIN_FILE=../vcpkg//scripts/buildsystems/vcpkg.cmake"

buildDebug: clean
	cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_TOOLCHAIN_FILE=../vcpkg//scripts/buildsystems/vcpkg.cmake"

bin: build 
	cmake --build build

docker-up:
	docker-compose  -f ./docker/docker-compose.yml --env-file .env up -d

docker-down:
	docker-compose -f ./docker/docker-compose.yml --env-file .env down

PHONY: clean build buildDebug bin docker-up docker-down

