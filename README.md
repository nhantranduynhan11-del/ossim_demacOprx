# Simple OS Simulator (`ossim_demacOprx`)

**Bài tập lớn Hệ điều hành (CO2018) — Học kỳ 261 — Lớp L01, Nhóm 08 (HCMUT)**

Mô phỏng hệ điều hành **Simple OS** hỗ trợ lập lịch MLQ đa CPU, quản lý bộ nhớ ảo phân trang 5 cấp (MM64) theo bố cục VMA kiểu macOS, cách ly không gian user/kernel qua PID, hoán đổi trang (swapping) và điều khiển truyền DMA bất đồng bộ tích hợp mô hình RTL (Verilator).

---

## Các thành phần đã hoàn thiện

1. **Lập lịch CPU (MLQ Scheduler — `src/sched.c`, `src/queue.c`):**
   - Hàng đợi đa mức ưu tiên `MAX_PRIO = 140` với cơ chế `slot[prio]` và hàng đợi `running_list`.
   - Tra cứu tiến trình đang chạy theo PID qua `find_running_proc_by_pid()`.
   - Hỗ trợ CPU non-blocking: tiến trình chờ DMA được đưa vào `deferred_pcb_list` để CPU chuyển sang chạy tiến trình khác ngay lập tức.

2. **Bố cục bộ nhớ kiểu macOS & VMA (`src/mm64.c`, `src/mm-vm.c`, `src/libmem.c`):**
   - Tách vùng cấm `__PAGEZERO` (`[0x0, 0x1000)`), chặn truy cập ở cả tầng `pg_getval`/`pg_setval` và MMU.
   - Ba VMA người dùng phân loại theo kích thước cấp phát:
     - `VMA 0` (`SMALL`): `[0x00010000, 0x00100000)`, `0 < size <= 512 B`, căn chỉnh `16 B`.
     - `VMA 1` (`MEDIUM`): `[0x00100000, 0x00500000)`, `512 < size <= 4096 B`, căn chỉnh `256 B`.
     - `VMA 2` (`LARGE`): `[0x00500000, 0x01000000)`, `size > 4096 B`, căn chỉnh `4096 B`.
   - Không gian kernel bắt đầu từ `KERNEL_BASE_ADDR` (mặc định `0xff00000000000000`, có thể cấu hình trong file input).

3. **Phân trang 5 cấp (MM64 — `src/mm64.c`, `include/mm64.h`):**
   - Không gian địa chỉ ảo 57-bit (`PGD -> P4D -> PUD -> PMD -> PT`), kích thước trang `4 KB`.
   - Page-walk cache 1 entry và bảng trang kernel dùng chung (`krnl_pgd`).
   - Hỗ trợ cấp phát khung vật lý liên tiếp (`alloc_contig_pages_range()`) và khởi tạo bằng 0 (`mm64_zero_frame()`) cho `kmalloc`.

4. **Cách ly User/Kernel & Swapping (`src/libmem.c`, `src/sys_mem.c`):**
   - Giao tiếp qua PID thông qua `SYSCALL 17` (`__sys_memmap`), không truyền trực tiếp con trỏ `pcb_t*`.
   - Bảng vùng nhớ kernel tách riêng theo từng PID (`krnl_symtbl_head` / `get_krnl_symrg_byid()`) trên không gian `krnl->mm` dùng chung, bảo vệ bởi `kmm_lock`.
   - Sao chép chéo an toàn qua ranh giới (`libkmem_copy_from_user`, `libkmem_copy_to_user`) với thứ tự khóa cố định `mmvm_lock -> kmm_lock`.
   - `pg_getpage()` thực hiện swap theo cơ chế giao dịch (có rollback khi lỗi) và bỏ qua frame đang bị DMA ghim (`!dma_frame_is_pinned(fpn)`).

5. **DMA bất đồng bộ với RTL Co-processor (`src/os.c`, `src/dpi_rtl.cpp`, `src/Vdma_top*.cpp`):**
   - Tích hợp mô hình phần cứng Verilator (`Vdma_top`), dịch và ghim trang người dùng (`mm64_dma_pin_user_page`).
   - Hàng đợi yêu cầu DMA và phát tín hiệu ngắt thật `SIGUSR1` (`cpu_irq_pending`, `dma_handle_pending_irqs`).

---

## Biên dịch & Kiểm thử (Linux / WSL)

### 1. Biên dịch

```bash
make clean && make os
make mm64test
```

### 2. Chạy Unit Test MM64

```bash
./mm64test
```
Kết quả đạt **82/82 test PASS** (bao gồm `alloc_contig_pages_range`, `mm64_zero_frame`, layout 3 VMA và kiểm tra `__PAGEZERO`).

### 3. Chạy các kịch bản mô phỏng

**Bộ test gốc (lập lịch, phân trang, syscall, cấu hình RAM nhỏ):**
```bash
./os sched
./os sched_0
./os sched_1
./os os_0_mlq_paging
./os os_1_mlq_paging
./os os_1_singleCPU_mlq_paging
./os os_1_mlq_paging_small_4K
./os os_2_mlq_paging
./os os_syscall
```

**Bộ test mở rộng (bảo vệ kernel, copy chéo user/kernel, DMA RTL & non-blocking):**
```bash
./os khoi_kernel_protection_test
OSSIM_KERNEL_COPY_TEST=1 ./os khoi_kernel_copy_test
./os khoi_syscall_io_protection_test
./os khoi_dma_data_test
./os khoi_dma_invalid_test
./os khoi_dma_deferred_test
./os khoi_dma_nonblocking_test
```

---

## Cấu trúc thư mục chính

- `include/` — Các file header (`mm64.h`, `libmem.h`, `sched.h`, `os-mm.h`, `dma.h`, `Vdma_top*.h`, ...).
- `src/` — Mã nguồn nhân OS, trình mô phỏng CPU/RAM/Swap, `libmem.c`, `mm64.c`, RTL C++ (`Vdma_top*`, `dpi_rtl.cpp`).
- `input/` & `input/proc/` — Các file cấu hình hệ thống và chương trình kiểm thử.
- `test/` — Unit test `mm64_test.c` và output tham chiếu `mm64_test.output`.
- `REPORT_OS_ASSIGNMENT_skeleton/` — Mã nguồn LaTeX báo cáo bài tập lớn (`main.tex`).

---

## Giới hạn mô phỏng đã biết

- Trường `FPN` trong PTE rộng 13 bit theo định dạng đề bài (tối đa 8192 frame = `32 MiB` RAM với trang `4 KB`).
- Nhánh swap-in và rollback trong `pg_getpage()` đã hoàn thiện về logic, nhưng `vm_map_ram()` hiện trả lỗi OOM khi hết khung RAM lúc cấp phát mới.
- Cấu hình biên `input/os_1_mlq_paging_small_1K` có dung lượng RAM (`1024 B`) nhỏ hơn kích thước một trang (`4096 B`), không tạo đủ 1 khung trang nên hệ thống từ chối cấp phát an toàn.
