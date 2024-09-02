# MiniHypervisor

## Platform

This project can only be run on Linux operating systems, and virtualization has to
be enabled; if you run it on a virtual machine, you need to enable nested virtualization.
and your CPU needs to support the Vmx extension if you have Intel, or the SVM extension if 
You have AMD CPU.

For compiling, just run the command
```
make
```

And there is command for running
```
./mini_hypervisor --memory 4 --page 2 -- guest guest1.img  guest2.img
```

> [!CAUTION]
If you get an error where you cannot open the /dev/kvm file
That probably means that your CPU does not support that type of instruction.

## Purpose

The purpose of this project is to make a mini virtual machine that will virtualize
terminal (serial I/O port 0xE9), and file system (paraller I/O port 0x0278).
It also can use privileged instructions on the x86 architecture. It can run multiple
guests at the same time/ It can work with files, if you want to see that check guest.c file.
