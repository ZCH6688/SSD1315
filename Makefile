KERN_DIR = /home/bit0/100ask_imx6ull-sdk/Linux-4.9.88

all:
	make -C $(KERN_DIR) M=`pwd` modules

clean:
	make -C $(KERN_DIR) M=`pwd` clean
	rm -rf modules.order

obj-m += ssd1315.o