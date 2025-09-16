.PHONY: build run

build:
	sh -c "bash init_build.sh"
	@g++ -g -O2 main.cpp -o my-docker
	sudo cp -r ./app/* ./my-rootfs/bin/
rund:
	sudo ./my-docker
run:
	./my-docker run /bin/sh