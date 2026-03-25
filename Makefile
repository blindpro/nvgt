.PHONY: default build install
default: build
build:
	scons -s
install:
	scons install