# Disposable UEFI probe. Never installed.
SRCDIR = .
TARGET = BOOTX64.EFI
OBJS = probe.o
LOCAL_GNUEFI_CFLAGS = -I. "-DEFIAPI=__attribute__((ms_abi))"
include ../Make.common
all: $(TARGET)
$(SHLIB_TARGET): $(OBJS)
	$(LD) $(GNUEFI_LDFLAGS) $(OBJS) -o $@ $(GNUEFI_LIBS)
$(TARGET): $(SHLIB_TARGET)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rodata \
	 -j .rel -j .rela -j .rel.* -j .rela.* -j .rel* -j .rela* \
	 -j .reloc --strip-unneeded $(FORMAT) $< $@
clean:
	rm -f $(TARGET) $(SHLIB_TARGET) $(OBJS)
