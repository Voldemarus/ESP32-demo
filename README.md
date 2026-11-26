
Активация среды 

. /Users/vladimirvodolazkiy/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32


xtensa-esp32-elf-gdb build/blink.elf -ex "set serial baud 115200" -ex "target remote /dev/cu.wchusbserial1130"


Проверка активации 

idf.py --version


