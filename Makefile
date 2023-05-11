
all: app.exe emconfig.json preprocess.xclbin

app.exe: ../../src/host.cpp
	g++ -Wall -g -std=c++11 ../../src/host.cpp -o app.exe \
		-I${XILINX_XRT}/include/ \
		-L${XILINX_XRT}/lib/ -lOpenCL -pthread -lrt -lstdc++
	
preprocess.xo: ../../src/preprocess.cpp
	v++ --hls.jobs 4 -c -t ${TARGET} --config ../../src/u280.cfg -k preprocess -I../../src ../../src/preprocess.cpp -o preprocess.xo 

preprocess.xclbin: ./preprocess.xo
	v++ --hls.jobs 4 -l -t ${TARGET} --config ../../src/u280.cfg ./preprocess.xo -o preprocess.xclbin

emconfig.json:
	emconfigutil --platform xilinx_u280_xdma_201920_3 --nd 1

clean:
	rm -rf preprocess* app.exe *json *csv *log *summary _x xilinx* .run .Xil .ipcache *.jou

# Unless specified, use the current directory name as the v++ build target
TARGET ?= $(notdir $(CURDIR))
