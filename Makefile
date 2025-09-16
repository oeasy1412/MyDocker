# 指定进入容器后使用的shell
DOCKER_SHELL := /bin/sh

CXX       := g++
CXXFLAGS  := -g -O2

SRC_DIR   := src
BUILD_DIR := build
BIN_DIR   := my-rootfs/bin
TARGET    := $(BUILD_DIR)/my-docker

$(TARGET):
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SRC_DIR)/*.cpp -o $(TARGET)

.PHONY: build rund run clean

build: $(TARGET)
	sh -c "bash init_build.sh"
	@echo "\033[32m===== 构建完成 =====\n\033[0m"
rund:
	sudo cp -r app/* $(BIN_DIR)/
	sudo $(TARGET)
run:
	$(TARGET) run $(DOCKER_SHELL)
clean:
	rm -rf $(BUILD_DIR) my-rootfs