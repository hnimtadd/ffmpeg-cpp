clean:
	rm -rf build/*

cleanOut: 
	@rm -rf out/*.ts

build: clean
	cmake -B build -S . "-DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake"

buildDebug: clean
	cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake"

bin: build 
	@ cmake --build build

run: bin
	@ ./build/main 1 libx264 ./assets/sample_mp4.mp4 hls index.m3u8

stream:
	@ ./scripts/stream.sh

docker-up:
	docker-compose  -f ./docker/docker-compose.yml --env-file .env up -d

docker-down:
	docker-compose -f ./docker/docker-compose.yml --env-file .env down

clean-media:
	rm -rf out/*

PHONY: clean build buildDebug bin run docker-up docker-down clean-media 

