# Page获取

## 1. 获取Page

1. 首先，获取Page的函数是`buf_page_get_gen`，这个函数是获取Page的通用函数，根据不同的参数，可以获取到不同的Page。
2. buf_block_t 包含buf_page_t，buf_page_t详细记录了Page的元数据，包括页号、页大小、页类型、页状态，LSN等。
3. chunk是Page的容器，一个chunk包含多个Page，chunk的结构是buf_chunk_t，chunk的结构体中包含一个buf_page_t的数组。通过mmap申请大块内存，然后通过chunk_alloc_page分配Page。初始化chunk的函数是`buf_chunk_init`。chunk是在线扩缩容的最小单位。
4. 读取page的时候，首先会通过page_hash 根据page_id进行查找，如果找到，直接返回。否则，从文件中进行读取。
5. 从磁盘读取文件，有一个统一的IO请求入口，分同步和异步，同步的入口是`buf_read_page`，异步的入口是`buf_read_page_low`。继续深入是在osfile.cc中
6. 读取到page后，会把page的fix进行计数，避免被回收。buf_block_t 会放入到mtr的memo中，记录其对象以及加锁的模式。
7.最后，在mtr commit的时候，会释放page的锁，以及buf_block_t的memo。