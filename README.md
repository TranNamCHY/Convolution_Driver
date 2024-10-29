# Device Driver for FPGA Convolution2D and Maxpooling module.
This project implements device driver for Convolution2D and Maxpooling FPGA module and have been tested on Zedboard (Zynq7000). 

## Usage

First, you need a tool to build a kernel image for your board, you can use petalinux or yocto project, it both works, but i
recommend petalinux because it was well maintained by Xilinx and don't cause two much error. Further more, we need the dma platform
driver of Xilinx installed properly because my driver bases on it. You can find more about how to install and use it at: https://www.hackster.io/whitney-knitter/getting-started-with-the-arty-z7-in-petalinux-2020-2-c49b7a.

At the final step of that guide, instead of using system.bit, you will replace it with my bit stream that include the design of Conv2d and Maxpooling.

In Petalinux, you need to enable the DMA support of Xilinx and also make some changes in the device tree, you can follow this link: https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/18842337/Linux+Soft+DMA+Driver?view=blog.
You just need to follow the guide for axidma test when customize device tree.

After you finish the build process, you should check the built device tree if it has been changed properly. Use dtc to revert-compile the system-top.dtb file to .dtc file and check if the axidmatest has beed included. If you can
boot your board by that image, and the log of boot process has line like "XilinxDMA@40004000 ...", that mean your image is ready to use.

Now, you'll need to compile the driver by yourself, :

First, download the toolchain gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf at link: https://releases.linaro.org/components/toolchain/binaries/4.9-2017.01/arm-linux-gnueabihf/
and extract it. Then clone this project, and modify the config.mk file, replace the "/home/nambcn" with your local path. 

In case you got error "Segmentation fault" when run CPU testbench and just want to recompile "matrix_ultility.c" to run on your PC, just change CROSS_COMPILE to "", and compile by:

```bash
make app
```
Copy libmatrix.so at outputs folder to the folder of CNN_NETWORK, and you can run test_cpu now.

Otherwise, this command will buitl both device driver and matrix_ultility.c for your board.

```bash
make driver app
```

When your compiling succeed, you are ready to use the FPGA Conv2d and Maxpooling now.

Insmod your kernel module:

```bash
sudo insmod driver.ko
```
Copy the libmatrix.so file at outs folder to the same folder of CNN_NETWORK that you have cloned. Now you can run the testbench for FPGA mode:

```bash
python test_fpga.py
```
