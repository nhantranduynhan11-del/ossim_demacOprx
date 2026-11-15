### Mốc 1: Tích hợp Scheduler + Memory 

**Trạng thái: Hoàn thành tích hợp và kiểm thử cơ bản.**

**Phiên bản code:**
- Branch: [`integration/moc1-14-15nov`](../../tree/integration/moc1-14-15nov)
- Commit: `ec917d7`

#### Kết quả kiểm thử

| Hạng mục | Kết quả |
|---|---|
| Scheduler MLQ | PASS 11/11 test |
| `os_0_mlq_paging` | PASS 5/5 tiến trình, exit 0 |
| `os_1_singleCPU_mlq_paging` | PASS 8/8 tiến trình, exit 0 |
| MM64 paging năm cấp | PASS kiểm thử cơ bản |
| Memory READ/WRITE | PASS kiểm thử cơ bản |
| Xử lý OOM | PASS testcase từ chối cấp phát |

#### Các lỗi đã sửa khi tích hợp

- **Scheduler:** Bổ sung hàm tìm PCB đang chạy theo PID.
- **Syscall 17:** Sửa đường xử lý syscall để tìm PCB thật theo PID.
- **MM64:** Sửa khởi tạo VMA để cấp phát bộ nhớ hoạt động.
- **Virtual Memory:** Sửa lời gọi `vm_map_ram()` và xử lý mã lỗi.
- **READ/WRITE:** Sửa cách tính PGN cho trang 4096B khi dùng MM64.
- **Memory Dump:** Tắt `IODUMP` toàn bộ RAM để tránh tạo log quá lớn khi chạy demo.

#### Những phần chưa hoàn thiện

- Giới hạn user VMA 16 MiB hiện chỉ là giá trị thử nghiệm.
- SWAP-out/SWAP-in thật chưa được kiểm chứng đầy đủ.
- User/kernel separation và địa chỉ 64-bit lớn chưa được kiểm thử toàn diện.
- DMA và kernel memory còn cần hoàn thiện.

### Lưu ý 

- Không ghi đè trực tiếp code từ nhánh cũ lên bản đã tích hợp.
- Giữ lại những thay đổi đã PASS khi phát triển chức năng mới.
- Review phần code thuộc nhiệm vụ của mình.
