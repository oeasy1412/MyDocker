.PHONY: build run

build:
# 	sh -c "bash init_build.sh"
	@g++ -g -O2 main.cpp -o my-container
	sudo cp -r ./app/* ./my-rootfs/bin
run:
	sudo ./my-container run /bin/bash