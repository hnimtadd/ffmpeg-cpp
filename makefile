bin:
	rm -rf build/* && cd build && cmake ../ && cmake --build . 

docker-up:
	docker-compose  -f ./docker/docker-compose.yml --env-file .env up -d

docker-down:
	docker-compose -f ./docker/docker-compose.yml --env-file .env down

PHONY: bin docker-up docker-down

