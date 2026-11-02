# cityman: the ANX7816 reference clock is PM8994 DIV_CLK3 at 19.2 MHz
# (LG boards use a 27 MHz crystal, the driver default).
ccflags-$(CONFIG_ARCH_MSM8994) += -DXTAL_CLK_DEF=XTAL_19D2M
obj-$(CONFIG_SLIMPORT_ANX7816)	+= slimport7816.o slimport7816_tx_drv.o
