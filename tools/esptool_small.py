# esptool with small RAM/flash blocks, for USB-UART bridges (the CH343 of the round board on macOS)
# that corrupt large host->device packets. Needs `pip install esptool`. Usage: same arguments as esptool.
import sys, os
import esptool
from esptool import loader
BLOCK = int(os.environ.get("BLOCK", "256"))
def all_subclasses(c):
    for s in c.__subclasses__():
        yield s
        yield from all_subclasses(s)
for cls in [loader.ESPLoader, *all_subclasses(loader.ESPLoader)]:
    if "ESP_RAM_BLOCK" in cls.__dict__ or cls is loader.ESPLoader:
        cls.ESP_RAM_BLOCK = BLOCK
    cls.FLASH_WRITE_SIZE = BLOCK
loader.ESPLoader.ESP_RAM_BLOCK = BLOCK
esptool._main()
