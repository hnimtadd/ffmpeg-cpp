bin:
	rm -rf build/* && cd build && cmake ../ && cmake --build . 

run: 
	./build/main ./assets/starry_night.jpg

PHONY: bin run

