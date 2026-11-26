# Memory layout kiểu macOS và thống kê bảng trang (Nhân, Role B)

Phần này trả lời lưu ý số 1 của giáo viên (Section 2.2.1, trang 15 và bảng *MacOS Virtual Memory Regions* trang 31): không dùng lại layout kiểu Linux của các học kỳ trước, mà tổ chức không gian địa chỉ user thành các vùng cho dữ liệu có kích thước tương đồng (small, medium, large).

Code nằm ở `include/mm64.h` (hằng số, macro) và `src/mm64.c` (`init_mm`, các hàm hỗ trợ, `print_mm_layout`). Kiểm thử bằng `make mm64test && ./mm64test`.

## 1. Layout của một process user

Mọi khoảng đều là nửa mở `[start, end)`, tức `addr >= start && addr < end`.

| Vùng | Khoảng địa chỉ | Kích thước | VMA | Dành cho | Căn lề (quantum) |
|---|---|---|---|---|---|
| PAGEZERO | `[0x0000000, 0x0001000)` | 4 KB | không có | không ai được truy cập | – |
| Dự phòng `__TEXT`/`__LINKEDIT` | `[0x0001000, 0x0010000)` | 60 KB | không có | code (simulator không nạp code vào RAM) | – |
| SMALL | `[0x0010000, 0x0100000)` | 960 KB | `vma0` | `size <= 512 B` | 16 B |
| MEDIUM | `[0x0100000, 0x0500000)` | 4 MB | `vma1` | `512 B < size <= 4096 B` | 256 B |
| LARGE | `[0x0500000, 0x1000000)` | 11 MB | `vma2` | `size > 4096 B` | 4096 B (1 trang) |
| Kernel | từ `0xFF00000000000000` | tăng dần | 1 VMA của `krnl->mm` | `kmalloc` | 1 trang |

Danh sách VMA của user là `vma0 → vma1 → vma2`. Mỗi VMA có `sbrk` riêng, bắt đầu tại `vm_start` và chỉ tăng trong `[vm_start, vm_end)`. Mỗi VMA có free-region list riêng, node đầu đã gắn đúng `vmaid`. Kernel vẫn giữ đúng 1 VMA.

## 2. Vì sao thiết kế như vậy

**PAGEZERO.** Giống `__PAGEZERO` của macOS: trang đầu tiên không bao giờ được map, nên mọi truy cập qua con trỏ NULL (hoặc NULL + offset nhỏ) đều bị bắt. Việc chặn nằm ngay trong MMU mô phỏng: `pgtbl_find_pte()` trả `NULL` và tăng `nr_fault` cho mọi địa chỉ user `< 0x1000`, nên không hàm nào (`vm_map_ram`, `pte_set_fpn`, `pte_get_entry`, `vmap_pgd_memset`) map hay đọc được trang 0. `mm64_dma_pin_user_page()` cũng từ chối PAGEZERO, nên DMA không thể ghi vào đó. Trước thay đổi này, biến đầu tiên của mọi process được cấp tại `0x0`, đúng là lỗi "vùng nhớ bắt đầu ở 0x0, chồng lên PAGEZERO" trong đợt review.

**Ba vùng theo kích thước.** Bộ cấp phát của macOS tách *tiny / small / large zone*, mỗi zone có quantum riêng. Lợi ích:

- Các khối cùng cỡ nằm cạnh nhau, nên một lỗ trống do `free` để lại có kích thước gần với các yêu cầu sau. Khối nhỏ không xé vụn vùng dành cho khối lớn, nhờ đó giảm phân mảnh ngoài trong heap.
- Phân mảnh trong bị chặn theo quantum: tối đa 15 B ở SMALL, 255 B ở MEDIUM, 4095 B ở LARGE.
- Tìm chỗ nhanh hơn: chỉ duyệt free list của đúng một vùng.

**Phối hợp với DMA.** Khối LARGE luôn bắt đầu ở biên trang và chiếm trọn các trang. Một buffer N trang ứng với đúng N frame, nên DMA tách theo trang ít đoạn nhất và không có trang nào chia chung với biến khác. Khối SMALL/MEDIUM có thể vắt qua biên trang. Trường hợp đó được `libmem_dma_memcpy` tách theo trang (phần của Thành).

**Ít tốn bảng trang.** Cả ba vùng nằm trong 16 MB đầu, nên dùng chung PGD[0], P4D[0], PUD[0] và một bảng PMD. SMALL và MEDIUM cùng nằm trong vùng 2 MB đầu tiên nên dùng chung một bảng PT; LARGE bắt đầu ở vùng 2 MB thứ ba nên cần thêm một PT.

## 3. Chọn vùng và căn lề (API cho `libmem.c`)

| Hàm / macro | Ý nghĩa |
|---|---|
| `mm64_vmaid_for_size(size)` | `<= 512` → `PAGING64_VMA_SMALL` (0), `<= 4096` → `PAGING64_VMA_MEDIUM` (1), lớn hơn → `PAGING64_VMA_LARGE` (2); `size == 0` → `-1` |
| `mm64_align_size(vmaid, size)` | làm tròn lên theo quantum của vùng: 100 → 112, 300 → 512, 5000 → 8192; `vmaid` sai → 0 |
| `mm64_vma_align(vmaid)` | quantum của vùng (16 / 256 / 4096) |
| `PAGING64_IS_PAGEZERO_ADDR(addr)`, `mm64_is_pagezero_addr(addr)` | 1 nếu `addr < 0x1000` |
| `mm64_range_in_pagezero(start, end)` | 1 nếu `[start, end)` chạm PAGEZERO |
| `PAGING64_PAGEZERO_END`, `PAGING64_SMALL_START` … `PAGING64_LARGE_END` | biên các vùng |
| `print_mm_layout(mm, pid)` | in trạng thái từng vùng (cũng được `print_pgtbl` in ở cuối) |

Ví dụ `liballoc` sau khi Linh nối vào: `vmaid = mm64_vmaid_for_size(size); size = mm64_align_size(vmaid, size); __alloc(proc, vmaid, reg_index, size, &addr);`

`vma_id_of()` đã đổi `<=` thành `<`. Với ba vùng nằm sát nhau, byte `0x100000` thuộc MEDIUM chứ không thuộc SMALL.

## 4. Trạng thái cấp phát trong các vùng

`print_pgtbl()` in thêm một dòng cho mỗi vùng. Trích output thật của `input/os_0_mlq_paging` sau lệnh `alloc 300 0` đầu tiên (bản build bật `IODUMP`):

```
liballoc: allocated 300 bytes at 0x0000000000010000 for reg_index 0
print_pgtbl: PID 1 VA[0x0000000000000000 - 0xffffffffffffffff]
  va=0x0000000000010000 pgn=0x10 [PGD 000|P4D 000|PUD 000|PMD 000|PT 010] SMALL  -> fpn 0 (pa=0x0)
 [PGTBL-STAT] PID 1: tables PGD=1 P4D=1 PUD=1 PMD=1 PT=1 size=20480B | walks=1 pt-mem-access=5 (5.00/walk) pwc-hit=0 faults=0
 [LAYOUT] PID 1 PAGEZERO [0x00000000,0x00001000) never mapped, no access
 [LAYOUT] PID 1 vma0 SMALL  [0x00010000,0x00100000) align=16   sbrk=0x00011000 used=4096/983040B pages=1 swapped=0 free=3796B
 [LAYOUT] PID 1 vma1 MEDIUM [0x00100000,0x00500000) align=256  sbrk=0x00100000 used=0/4194304B pages=0 swapped=0 free=0B
 [LAYOUT] PID 1 vma2 LARGE  [0x00500000,0x01000000) align=4096 sbrk=0x00500000 used=0/11534336B pages=0 swapped=0 free=0B
```

Cách đọc: `used` là số byte giữa `vm_start` và `sbrk`; `pages` là số trang trong đoạn đó đang ở RAM, `swapped` là số trang đang ở SWAP; `free` là tổng byte đang chờ trong free list của vùng. Ở ví dụ trên, `inc_vma_limit` map 1 trang (4096 B), biến dùng 300 B, phần còn lại 3796 B nằm trong free list của SMALL. Cùng input đó trên `main` cũ cấp biến tại `0x0000000000000000`.

## 5. Thống kê bảng trang

Dòng `[PGTBL-STAT]` (hàm `print_pgtbl_stat`) được in sau mỗi lần dump và khi process kết thúc (`free_pcb_pgtbl`):

- `tables` là số bảng mỗi cấp; `size` = tổng số bảng × 4096 B.
- `walks` là số lần dịch địa chỉ; `pt-mem-access` là số entry bảng trang đã đọc.
- `pwc-hit` là số lần trúng page-walk cache (1 truy cập thay vì 5); `faults` là số lần tra cứu gặp trang chưa map, PAGEZERO hoặc địa chỉ non-canonical.

Số đo từ `./mm64test`:

| Tình huống | Số bảng | Dung lượng bảng trang |
|---|---|---|
| Process mới, chưa cấp gì (chỉ PGD) | 1 | 4 KB |
| 3 trang đầu của SMALL | 5 | 20 KB |
| + 1 trang đầu MEDIUM (chung PT với SMALL) | 5 | 20 KB |
| + 1 trang đầu LARGE (thêm 1 PT) | 6 | 24 KB |
| Bảng phẳng 1 cấp cho 57 bit (so sánh) | – | 256 TiB |

| 100 lần dịch địa chỉ | Truy cập bảng | Trung bình | Cache hit |
|---|---|---|---|
| Trong SMALL | 104 | 1,04 | 99 |
| Xen kẽ SMALL / MEDIUM (chung PT) | 104 | 1,04 | 99 |
| Xen kẽ hai vùng xa nhau | 500 | 5,00 | 0 |
| Không có cache (lý thuyết) | 500 | 5,00 | 0 |

Đặt cả ba vùng heap trong 16 MB đầu là có chủ ý: truy cập qua lại giữa SMALL và MEDIUM vẫn trúng page-walk cache, và một process dùng cả ba vùng chỉ tốn 24 KB bảng trang.

## 6. Kiểm thử

`./mm64test`: 72 kiểm tra đạt, 0 lỗi. Các test liên quan tới layout:

| Test | Nội dung |
|---|---|
| T2 | `init_mm` tạo đúng `vma0 → vma1 → vma2` với các biên ở bảng 1, free list gắn đúng `vmaid`; kernel giữ 1 VMA |
| T3 | ngưỡng 512/4096, căn lề 16/256/4096, kiểm tra PAGEZERO |
| T4 | không map được trang 0; không tốn frame hay bảng; truy cập bị đếm là fault |
| T6 | biên nửa mở: trang `0x100000` thuộc MEDIUM, `0x500000` thuộc LARGE; số bảng 5 → 6 |
| T13 | DMA pin từ chối PAGEZERO |
| T17 | dump bảng trang kèm trạng thái ba vùng |

Ngoài ra đã chạy `os_0_mlq_paging`, `os_1_mlq_paging`, `os_1_singleCPU_mlq_paging`, `os_2_mlq_paging`, `os_1_mlq_paging_small_4K`, `os_syscall` và bốn test `khoi_*`, trên một bản build không có Verilator (DMA được thay bằng bản giả lập copy byte qua system bus). Kết quả giống `main` trước thay đổi, chỉ khác địa chỉ cấp phát (bắt đầu tại `0x10000`), kể cả khi ghép với `libmem.c` của nhánh `thanh/warning-fix`. Cần chạy lại bằng `make os` thật trên máy Ubuntu khi tích hợp.

## 7. Giới hạn

- Khi một vùng hết chỗ (ví dụ SMALL đầy 960 KB), yêu cầu thất bại chứ chưa chuyển sang vùng kế tiếp.
- Khoảng `[0x1000, 0x10000)` chỉ để dành, chưa dùng vì simulator không nạp code vào RAM.
- Chưa có vùng stack; có thể đặt phía trên `0x1000000` nếu cần.
