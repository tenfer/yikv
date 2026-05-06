# 约束
1. container 容器类通过 [`Allocator`](../alloc/allocator.h) 分配 arena 内内存（典型为 `FtAllocator`）；数据体在 mmap 的可恢复区域中，而非随意 `malloc` 堆指针。
2. 配合文件映射 arena 时，数据可持久化到文件
3. 对象内存布局以 offset 为主，mmap 恢复后基址变化仍可通过 offset 访问

# container
 - hashmap
 - list
 - vector
 - string
 - roaringbitmap
 - usage: 见 [`USAGE.md`](USAGE.md)（含 HashMap `publish()` 与延迟回收语义）

# container 特点
1. 为了追求性能，使用场景是单写多读
2. 接口设计尽量和STL保持一致
3. 代码尽可能简洁，不要使用过多黑魔法

# 设计取舍
为了减少container的复杂性，使用固定地址mmap，使用高位地址避免和堆栈的地址冲突，这样做的好处允许使用指针，但依然保证可移植。





