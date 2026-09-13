# CoreZ OS

> 一个从零编写的 x86\_64 长模式操作系统内核学习项目。

## 简介

CoreZ OS 是一个用于学习操作系统原理的教学型内核，从 16 位实模式引导扇区起步，经
加载器进入保护模式并开启长模式，最终运行一个支持多任务、用户进程、文件系统与
网络的 64 位内核。

当前内核已能在 QEMU 中稳定引导与运行：图形化 Boot Menu（VBE 1024x768 + KASLR）、
多任务与抢占式调度、COW fork 的用户进程、ext2 文件系统、`/proc` 伪文件系统、
TCP/UDP 网络栈、带平铺布局的 GUI 合成器，以及一个可执行内置命令与用户程序的
shell。代码风格约定见 [CODE_STYLE.MD](CODE_STYLE.MD)。

## 已实现功能

### 引导与平台

| 模块           | 说明                                                                                                                  |
| ------------ | ------------------------------------------------------------------------------------------------------------------- |
| 引导 (Boot)    | `arch/x86/boot/boot.asm`：512 字节主引导扇区，从硬盘分区加载 loader                                                       |
| 加载器 (Loader) | `arch/x86/boot/loader.asm`：FAT32 读取内核、VBE 图形模式、E820 内存探测、Boot Menu（倒计时/上下键选择 + 重启项）、KASLR 内核加载地址随机化、进入保护模式 → 长模式、构造 Multiboot2 信息 |
| 中断           | `arch/x86/interrupt`：256 门 IDT，ring3 异常转信号（SIGSEGV 等），COW 缺页处理                                                          |
| 中断控制器        | `kernel/init/apic`：Local APIC + IPI + LAPIC 定时器（校准 per-tick）；`kernel/init/pic`、`kernel/init/pit`：PIC + PIT 回退路径     |
| ACPI         | `kernel/init/acpi`：RSDP/RSDT/FADT 探测与 ACPI 使能                                                                       |
| SMP          | `kernel/init/smp`：AP trampoline（低 1MB 实模式跳板）+ INIT/SIPI 唤醒 + 每 CPU GDT/栈                                            |
| 屏幕输出         | `drivers/char/console`：VBE 线性帧缓冲上绘制字库文本，`kprintf`/`console_putc`，debugcon (0xE9) 镜像输出                               |

### 内存管理

| 模块       | 说明                                                                                               |
| -------- | ------------------------------------------------------------------------------------------------ |
| 物理内存池    | `kernel/mm/pool`：E820 探测 + 位图管理物理页框；`pool_lock`(物理位图) 与 `map_lock`(页表/虚拟位图) 分离，单页分配走每 CPU 缓存，空闲页数由原子计数 O(1) 读出                 |
| 内核虚拟地址池  | `KERNEL_VADDR_START` 起的 vaddr 位图，`ioremap` 设备映射（NX/PCD）                                          |
| 内核堆      | `get_kernel_pages`：高半区（`VIRT_OF = phys + 0xC0000000`）直接映射分配                                       |
| COW fork | `kernel/userprog/fork`：页表遍历复制，写时复制（`COW_FLAG` + 引用计数 `frame_owner`），缺页时在 `map_lock` 下 `page_cow_resolve` 一次完成决策/拷贝/递减 |
| 用户地址空间   | 每进程独立 PML4 + 用户 vaddr 位图；`mmap`/`brk` 堆扩展                                                        |

### 进程与调度

| 模块         | 说明                                                                            |
| ---------- | ----------------------------------------------------------------------------- |
| 线程/进程      | `kernel/sched/thread`：任务槽（`MAX_TASKS=64`，可回收）+ 红黑树就绪队列 + 时间片抢占调度，DIED 线程由调度器统一回收（栈/页目录/槽位） |
| 用户进程       | `kernel/userprog/process`：ring3（`intr_exit` 模拟中断返回）、TSS、TLS（`set_thread_area`） |
| fork/clone | `kernel/userprog/fork`、`kernel/userprog/clone`：COW 地址空间复制；clone 共享页目录与 fd 表（线程语义） |
| exec       | `kernel/userprog/exec`：ELF32/ELF64 加载、辅助向量（auxv）、参数/环境入栈、W^X（可执行段 RX）          |
| 退出/等待      | `kernel/userprog/wait_exit`：资源释放、孤儿进程自动终止回收、`wait`/`waitpid`                   |

### 同步与内核服务

| 模块       | 说明                                                                                                                          |
| -------- | --------------------------------------------------------------------------------------------------------------------------- |
| 同步原语     | `kernel/sched/sync`：信号量、可重入锁（`holder`/`holder_repeat_nr` 由信号量自带 spinlock 保护，多核安全）、自旋锁、写者优先读写锁（`rwlock_*`）                                        |
| 系统调用     | `kernel/syscall`：`int 0x80`（原生 ABI）+ `syscall` 指令路径 + musl 兼容层（`linux_compat.c`）；ring0 内核线程调用与 ring3 用户调用分别校验（`access_ok` 仅约束用户指针） |
| 信号       | `kernel/syscall/signal`：SIGSEGV/SIGINT 等常用信号、`sigaction`/`sigprocmask`/`sigreturn`、Ctrl+C 终止前台进程                             |
| futex    | `kernel/syscall/futex`：FUTEX\_WAIT/WAKE                                                                                      |
| 文件系统     | `kernel/fs/ext2`：ext2 只读元数据 + 读写的完整实现（inode/块分配释放、目录项增删、间接块、truncate），读写锁保护——只读路径并行、写路径排他                                          |
| 伪文件系统    | `kernel/fs/proc`：`/proc/meminfo`                                                                                             |
| 文件表      | `kernel/fs/file`：全局 file 表 + 每进程 fd 表，引用计数（fork/clone/dup 共享）为原子操作，槽位分配走 CAS 位图（无需锁）                                        |
| 管道       | `kernel/shell/pipe`：ioqueue 实现的匿名管道，`pipe`/`fd_redirect`，shell 支持 `cmd1 \| cmd2`                                              |
| inode 缓存 | `kernel/fs/inode`：每分区红黑树缓存打开的 inode（`i_open_cnt` 引用计数，开/关中断对称保护）                                                             |

### 设备与网络

| 模块  | 说明                                                                                                                                         |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| 键盘  | `drivers/char/keyboard`：8042 扫描码 → keymap（shift/caps/ctrl/alt），ioqueue 缓冲，GUI hook                                                          |
| 鼠标  | `drivers/char/mouse`：PS/2 aux 通道，三字节包解析                                                                                                    |
| 磁盘  | `drivers/block/ide`：IDE 通道 + 分区表解析，对接 block\_ops                                                                                           |
| 网卡  | `drivers/net/e1000`、`drivers/net/rtl8139`：PCI 探测、MMIO/IO 寄存器、TX/RX 描述符环                                                                     |
| 网络栈 | `drivers/net`：Ethernet/ARP/IP/ICMP/TCP/UDP + BSD socket API（`socket`/`bind`/`connect`/`send`/`recv`/`select`），QEMU user 网络下自动配置 `10.0.2.15` |
| GUI | `kernel/gui`：合成器（类 Wayland 的 surface/client 模型）、可替换平铺布局（Master-Stack/Tall/Wide/Monocle）、鼠标键盘事件分发，shell 输入 `gui` 进入                            |

### Shell 与用户程序

内置命令：`ls` `cd` `pwd` `mkdir` `rmdir` `rm` `clear` `ps` `gui` `shutdown`；
支持外部程序执行与管道。

`apps/` 下的用户程序（自带 mini-libc 编译为 ELF，musl 相关项由 `--with-musl-lib` 构建）：

| 程序                                          | 演示内容                         |
| ------------------------------------------- | ---------------------------- |
| `prog_no_arg` / `prog_arg`                  | 基本执行 / 参数传递                  |
| `fork_demo` / `orphan_demo`                 | fork / 孤儿进程回收 / `wait(NULL)` |
| `cow_stress`                                | COW 压力测试                     |
| `clone_demo` / `tls_test`                   | clone 线程 / TLS               |
| `prog_pipe` / `echocat`                     | 管道 / 回显                      |
| `heap_demo` / `mmap_demo` / `mmap2_demo`    | 堆与 mmap                      |
| `signal_demo` / `sig_test` / `futex_demo`   | 信号 / 信号帧 / futex             |
| `fsyscall_demo` / `dev_demo` / `badptr_test`| 文件 syscall / 设备 / 非法指针与边界    |
| `canary_test`                               | 栈保护（stack canary）            |
| `cwd_test`                                  | `getcwd` 边界                  |
| `cat`                                       | 文件读取（支持 `/proc/meminfo`）     |
| `ping` / `udp_echo`                         | ICMP ping / UDP 回显服务         |
| `font_demo`                                 | TTF 字体渲染                     |
| `gui_launch`                                | GUI 合成器入口                    |
| `lc_demo` / `musl_demo` / `libc_tests_main` | 自带 libc / musl ABI / libc 测试套件 |

`third_modules/` 下是第三方代码：musl（`--with-musl-lib` 构建完整 libc 并运行
libc-testsuite）、mr\_micro\_shell、toybox。

## 目录结构

```
├── build.py                # 构建系统（探测工具链、并行编译、打包镜像、启动 QEMU）
├── linker/
│   ├── kernel.ld           # 内核链接脚本
│   └── user.ld             # 用户程序链接脚本
├── scripts/                # 镜像与资源脚本（make_ext2/make_fat/mkfloppy/make_font_subset 等）
├── arch/x86/
│   ├── boot/               # boot.asm 引导扇区 + loader.asm（VBE/KASLR/长模式）
│   ├── asm/                # entry/switch/stub 等汇编
│   └── interrupt/          # idt/interrupt
├── includes/               # 全部公共头文件，目录树与源码镜像
├── kernel/
│   ├── init/               # main.c、gdt/tss/idt/apic/pic/pit/acpi/smp/mb2
│   ├── mm/                 # pool/bitmap/access（copy_from_user 等）
│   ├── sched/              # thread/sync/percpu
│   ├── userprog/           # process/fork/clone/exec/wait_exit
│   ├── fs/                 # ext2/file/inode/dir/proc
│   ├── syscall/            # syscall/file_syscall/signal/futex/mmap/linux_compat
│   ├── gui/                # gfx/server/wm/layout/clients/display/font/shm/udi
│   └── shell/              # shell/pipe/buildin_cmd
├── drivers/
│   ├── block/              # ide
│   ├── char/               # keyboard/mouse/tty/ioqueue/console
│   └── net/                # e1000/rtl8139/eth/arp/ip/icmp/tcp/udp/socket
├── lib/                    # str/list/rbtree/rand
├── libc/                   # 自带 mini-libc（user/）与 compat 层
├── apps/                   # 用户程序（见上表）
└── third_modules/          # musl / libc-testsuite / toybox
```

## 构建与运行

### 工具链

- `nasm` —— 汇编（elf64/bin）
- `zig cc`（或 gcc/clang 交叉）—— C 编译（freestanding）
- `ld.lld` / `ld` —— 链接
- `python3` —— 构建系统与镜像脚本
- `qemu-system-x86_64` —— 运行验证

### 构建

```bash
python3 build.py            # 构建内核与 floppy.img
python3 build.py run        # 构建并在 QEMU 中启动（自动生成 test_hd.img）
python3 build.py run --sm 2 # SMP 启动验证
python3 build.py run --gdb  # GDB stub (-s -S)，配合 build/kernel.elf 调试
python3 build.py clean
```

`run` 默认以 `build/test_hd.img` 硬盘镜像引导（自动生成：P1 FAT32 引导分区 +
P2 ext2 数据分区，并写入用户程序）；`--boot-floppy` 可改用软盘镜像。
`--with-musl-lib` 会额外编译完整 musl libc 并构建 libc-testsuite。

### 运行

启动后进入 Boot Menu（5 秒倒计时，上下键/数字键选择，回车立即引导）。
进入 shell 后可直接使用内置命令或运行用户程序，例如：

```
[corez@corez /]$ ls
[corez@corez /]$ fork_demo.elf
[corez@corez /]$ cat.elf /proc/meminfo
[corez@corez /]$ gui
```

QEMU 参数默认挂载 e1000 网卡 + user 网络后端（`hostfwd tcp::8765-:8765`），
内核自动配置 `10.0.2.15`，可用 `ping.elf` 测试连通性。

内核所有 `kprintf`/控制台输出会同步镜像到 QEMU debugcon（端口 0xE9），
`-debugcon stdio`/`file:...` 可在终端或文件中查看内核日志，便于无头调试。

## 已知问题

- 用户 fork 的子进程在进入用户态后触发 SIGSEGV（退出码 139），fork 子进程侧
  功能暂不可用；父进程路径与 COW 建页正常。与 exec 的 W^X / COW / NX 互作用有关。
- e1000 初始化在大规模内存分配压力下可能缺页（`ioremap` 的映射丢失，原因待查），
  正常负载下网络工作正常。
- 并发：物理页池按 `pool_lock`(位图)/`map_lock`(页表) 拆分，单页分配经每 CPU 缓存、
  空闲页数用原子计数；ext2 元数据用读写锁（读并行/写排他）；file 表槽位分配与引用计数
  无锁。SMP 调度尚未启用（AP 仅验证可启动），这些机制是为多核做的前置准备。

## 参考资料与致谢

- **OSDev Wiki** <https://wiki.osdev.org> —— 引导、GDT/IDT、APIC、长模式、Multiboot2 等。
- **musl libc** <https://musl.libc.org> —— 系统调用 ABI 兼容目标与 libc 测试套件。

## 路线图

- [x] 长模式内核：引导（VBE/KASLR/Boot Menu）、中断、APIC/ACPI、SMP 框架
- [x] 内存管理：物理池、内核虚拟地址池、ioremap、用户地址空间
- [x] 进程：线程/进程、抢占调度、COW fork、clone、exec（ELF32/64）、wait/孤儿回收
- [x] 同步：信号量、可重入锁、futex、信号
- [x] 文件系统：ext2（读写）、/proc、管道、dup/fd 引用计数
- [x] 网络：e1000 + ARP/IP/ICMP/TCP/UDP + socket API
- [x] GUI 合成器、shell 与用户程序集
- [x] musl 构建选项（`--with-musl-lib`）
- [ ] 修复 fork 子进程 SIGSEGV（见已知问题）
- [ ] 内核堆分配器（malloc 形态的细粒度分配）
- [ ] 多核调度（AP 目前仅验证可启动，未参与调度）
