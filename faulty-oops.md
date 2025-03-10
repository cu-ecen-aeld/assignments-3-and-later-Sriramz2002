# Kernel Oops Debugging Report: faulty.ko

## **System Log Output**


# echo “hello_world” > /dev/faulty
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Mem abort info:
  ESR = 0x0000000096000045
  EC = 0x25: DABT (current EL), IL = 32 bits
  SET = 0, FnV = 0
  EA = 0, S1PTW = 0
  FSC = 0x05: level 1 translation fault
Data abort info:
  ISV = 0, ISS = 0x00000045
  CM = 0, WnR = 1
user pgtable: 4k pages, 39-bit VAs, pgdp=0000000041bd3000
[0000000000000000] pgd=0000000000000000, p4d=0000000000000000, pud=0000000000000000
Internal error: Oops: 0000000096000045 [#1] SMP
Modules linked in: hello(O) faulty(O) scull(O)
CPU: 0 PID: 154 Comm: sh Tainted: G           O       6.1.44 #1
Hardware name: linux,dummy-virt (DT)
pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
pc : faulty_write+0x10/0x20 [faulty]
lr : vfs_write+0xc8/0x390
sp : ffffffc008e03d20
x29: ffffffc008e03d80 x28: ffffff8001b34f80 x27: 0000000000000000
x26: 0000000000000000 x25: 0000000000000000 x24: 0000000000000000
x23: 0000000000000012 x22: 0000000000000012 x21: ffffffc008e03dc0
x20: 00000055580869b0 x19: ffffff8001bdd200 x18: 0000000000000000
x17: 0000000000000000 x16: 0000000000000000 x15: 0000000000000000
x14: 0000000000000000 x13: 0000000000000000 x12: 0000000000000000
x11: 0000000000000000 x10: 0000000000000000 x9 : 0000000000000000
x8 : 0000000000000000 x7 : 0000000000000000 x6 : 0000000000000000
x5 : 0000000000000001 x4 : ffffffc000787000 x3 : ffffffc008e03dc0
x2 : 0000000000000012 x1 : 0000000000000000 x0 : 0000000000000000
Call trace:
 faulty_write+0x10/0x20 [faulty]
 ksys_write+0x74/0x110
 __arm64_sys_write+0x1c/0x30
 invoke_syscall+0x54/0x130
 el0_svc_common.constprop.0+0x44/0xf0
 do_el0_svc+0x2c/0xc0
 el0_svc+0x2c/0x90
 el0t_64_sync_handler+0xf4/0x120
 el0t_64_sync+0x18c/0x190
Code: d2800001 d2800000 d503233f d50323bf (b900003f) 
---[ end trace 0000000000000000 ]---







## Disassembly of faulty.ko

student@ecen5713-vm1-srja8222:~/a5/assignment-5-Sriramz2002$ ./buildroot/output/host/bin/aarch64-linux-objdump -S ./buildroot/output/target/lib/modules/6.1.44/extra/faulty.ko

./buildroot/output/target/lib/modules/6.1.44/extra/faulty.ko:     file format elf64-littleaarch64


Disassembly of section .text:

0000000000000000 <faulty_write>:
   0:	d2800001 	mov	x1, #0x0                   	// #0
   4:	d2800000 	mov	x0, #0x0                   	// #0
   8:	d503233f 	paciasp
   c:	d50323bf 	autiasp
  10:	b900003f 	str	wzr, [x1]
  14:	d65f03c0 	ret
  18:	d503201f 	nop
  1c:	d503201f 	nop

## Kernel Oops Analysis: faulty.ko

The kernel Oops is caused by a NULL pointer dereference in the faulty.ko module. The exception occurs in **:- 
faulty_write()** when the function attempts to store a value at address 0x0, leading to a segmentation fault. The error 
message shows data abort due to a **level 1 translation fault**, with the instruction [ str wzr, [x1] ]attempting to write 
to a NULL pointer. The call trace confirms that the issue originates in **:- faulty_write()**, which is called via 
ksys_write() during a user-space write operation to /dev/faulty.

### **Analysis of :- faulty_write()**
- The function explicitly sets x1 to 0x0.
- It then attempts to store a value at this location.
- The absence of a NULL check before dereferencing results in a fault.
- Kernel memory management does not allow writing to 0x0, causing execution to stop to prevent corruption.

### **Resolution**
- The issue is resolved by adding check before dereferencing the pointer.
- If the pointer is NULL, an error is returned instead of proceeding with the write operation.
- This prevents an invalid memory reference and ensures controlled error handling instead of system instability.
