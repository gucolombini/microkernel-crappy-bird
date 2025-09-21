    #!/bin/bash
echo "Building Kernel!"
nasm -f elf32 kernel.asm -o kasm.o
gcc -m32 -ffreestanding -fno-stack-protector -nostdlib -c kernel.c -o kc.o
ld -m elf_i386 -T link.ld -o kernel kasm.o kc.o