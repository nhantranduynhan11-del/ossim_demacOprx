# **Phân công viết report · BTL Simple OS** 

Report chiếm **3 điểm** , nộp chung với source trước **29/11** . Mỗi người viết phần mình đã code, cộng thêm các câu Question liên quan. Khôi ghép bản cuối. 

**Đề yêu cầu Khung sườn** 

**7 câu hỏi Từng người Lịch Quy tắc** 

### **Đề yêu cầu report có gì** 

## **Gantt** 

**Scheduling** 

Vẽ biểu đồ Gantt: process nào chạy trên CPU nào, ở time slot nào. 

## **Cấp phát** 

**Memory management** 

Trạng thái cấp phát trong vùng dữ liệu. Chứng minh user và kernel giao tiếp qua **PID** , không truyền PCB. 

## **Số liệu** 

**Multi-level paging** 

Sơ đồ dịch địa chỉ nhiều cấp, **số lần truy cập bộ nhớ** và **dung lượng bảng trang** , giải thích lợi ích thiết kế. 

## **7 câu** 

##### **Questionnaire** 

Trả lời **tất cả** câu Question trong đề. Thiếu câu nào là mất điểm câu đó. 

**Thêm theo warning:** giải thích thiết kế layout macOS (Small / Medium / Large, trang 31) và cơ chế DMA bất đồng bộ bằng ngắt. Hai phần này giảng viên đã nhắc riêng nên phải có mục riêng trong report. 

### **Khung sườn report (** main.tex **)** 

Khung đã dựng từ mẫu học kỳ trước, đổi bìa và xóa nội dung cũ. Mỗi chỗ cần viết có một dòng chữ xám \viet{Tên}{việc cần viết} . Viết xong thì xóa dòng đó. 

**Bìa và Lời mở đầu** Đã sửa: ossim_demacOprx, GVHD Nguyễn Tuấn Huy, L01 nhóm 08, tháng 11/2026. Chỉ cần rà lại lời mở đầu cuối cùng 

**Khôi** 

**Khôi** 

**Giới thiệ** 

**à hâ ô** 

**1** 

|**1**|**Giới thiệu và phân công**<br>1.1 Môi trường và cách chạy · 1.2 Bảng phân công (mỗi người tự điền<br>dòng của mình)|**Khôi**|
|---|---|---|
|**2**|**Scheduling**<br>2.1 Hiện thực MLQ · 2.2 Gantt · 2.3 Trả lời Q3, Q2|**Khôi**|
|**3**|**Memory Layout**<br>3.1 Bảng bố cục vùng nhớ (khung bảng có sẵn) · 3.2 Lý do thiết kế<br>kiểu macOS|**Nhân**|
|**4**|**Virtual Memory Management**<br>4.1 ALLOC và FREE · 4.2 Phân vùng theo kích thước · 4.3 Trạng thái<br>cấp phát · 4.4 Trả lời Q4, Q6|**Linh**|
|**5**|**Multi-level Paging**<br>5.1 Sơ đồ phân giải địa chỉ · 5.2 Thống kê truy cập và kích thước bảng<br>trang · 5.3 Lợi ích · 5.4 Trả lời Q5|**Nhân**|
|**6**|**MMU and User/Kernel Spaces**<br>6.1 Giao tiếp qua PID · 6.2 Truy cập bộ nhớ user/kernel · 6.3<br>Swapping · 6.4 Trả lời Q1|**Thành**|
|**7.1**|**Co-processor: bất đồng bộ và ngắt**<br>Sơ đồ giao việc→chạy tiếp→ SIGUSR1 →ISR, test chứng minh CPU<br>không bị chặn|**Khôi**|
|**7.2**|**Co-processor: bus addressing**<br>DMA kéo trang từ SWAP, ghim trang, chia đoạn theo trang|**Thành**|
|**8**|**Synchronization**<br>8.1 Tài nguyên dùng chung và khóa · 8.2 Trả lời Q7|**Thành**|
|**9**|**Kết luận**<br>Đã làm được gì, hạn chế, hướng mở rộng|**Linh**|
|**Chỉ sử**<br>Mỗi sec|**a section của mình**<br>tion có comment% ===n. TÊN —<br>**Hình đặt trong**figures/<br>Tên có tiền tố người làm:khoi_,n|han_,|
|NGƯỜI V<br>khá<br>đ|IẾT===. Không sửa section người<br>ểt á h<br>đột khi<br>hé<br>linh_,thanh_. Ví dụ<br>fi<br>/kh i<br>h d<br>tt||



khác để tránh xung đột khi ghép. 

figures/khoi_sched_gantt.png . 

#### **Biên dịch bằng XeLaTeX** 

Trên Overleaf chọn Compiler: XeLaTeX để hiện đúng tiếng Việt. Khung đã biên dịch thử, không lỗi. 

#### **Không chép nội dung học kỳ trước** 

Mẫu cũ dùng bitmap, layout Linux và chưa có co-processor. Đề kỳ này khác, phải viết theo code của nhóm. 

### **7 câu Question trong đề** 

|**Câu**|**Đề hỏi (tóm tắt)**|**Mục**<br>**trong**<br>**report**|**Người**<br>**viết**|
|---|---|---|---|
|**Q1**|Ưu, nhược điểm của một giao diện system call<br>thống nhất cho nhiều thành phần (file, bộ nhớ,<br>I/O): thiết kế OS, hiệu năng, xử lý lỗi, tính di động|6.4|**Thành**|
|**Q2**|System call chạy quá lâu thì OS phát hiện và xử lý<br>thế nào|2.3|**Khôi**|
|**Q3**|Lợi ích của từng chính sách trong MLQ|2.3|**Khôi**|
|**Q4**|Vì sao kết hợp phân đoạn với phân trang, cách<br>này khắc phục hạn chế của từng kỹ thuật ra sao|4.4|**Linh**|
|**Q5**|Lợi ích của việc mở rộng phân trang phân cấp lên<br>N cấp|5.4|**Nhân**|
|**Q6**|Ưu, nhược điểm của phân trang so với cấp phát<br>liên tục|4.4|**Linh**|
|**Q7**|Không đồng bộ thì chuyện gì xảy ra; minh họa<br>bằng ví dụ, nếu có, trong các thao tác bộ nhớ<br>kernel|8.2|**Thành**|



Mỗi câu nên có: trả lời lý thuyết ngắn gọn, rồi **liên hệ với chính code nhóm** (tên hàm, output test). Giảng viên chấm cao phần liên hệ hơn phần chép lý thuyết. 

### **Từng người viết gì** 

### **Khôi** 

### **Nhân** 

Mục 3, 5 · Q5 

Mục 1, 2, 7.1 · Q2, Q3 · ghép bản cuối 

###### **Nội dung** 

###### **Nội dung** 

- Mục 1: môi trường build, cấu hình, bảng phân công 

- Mục 2: cách cài enqueue , dequeue , get_mlq_proc , cơ chế slot[] 

- Mục 7.1: sơ đồ CPU giao việc → chạy tiếp → co_processor chép → SIGUSR1 → handle_dma_irq ; vì sao handler chỉ đặt cờ; deferred_pcb_list 

###### **Bằng chứng cần đưa vào** 

- Gantt cho sched , sched_0 , sched_1 từ output thật 

- Output test DMA mới: process B được dispatch ngay sau khi A gửi DMA, rồi mới có dòng log ISR 

###### **Thêm** 

- Mục 3: hình layout PAGEZERO, Small, Medium, Large, kernel; lý do chọn ranh giới 512 / 4096 và căn chỉnh 16 / 256 / 4096; liên hệ bảng macOS trang 31 

- Mục 5: tách một địa chỉ thật thành 5 chỉ số PGD, P4D, PUD, PMD, PT như ví dụ trang 17 của đề 

- Giải thích cấp phát bảng trang theo nhu cầu (chỉ tạo bảng con khi dùng tới) 

###### **Bằng chứng cần đưa vào** 

   - Output print_pgtbl và 

   - print_pgtbl_stat : số bảng đã cấp, dung lượng, số lần truy cập bộ nhớ cho một lần dịch địa chỉ 

   - So sánh dung lượng: bảng phẳng so với 5 cấp cấp phát theo nhu cầu 

- Gom bản nháp, thống nhất định dạng, đánh số hình và bảng, xuất PDF 

### **Linh** 

Mục 4, 9 · Q4, Q6 

###### **Nội dung** 

- Mục 4: luồng ALLOC (tìm vùng trống → nới sbrk → map trang), luồng FREE (trả vào vm_freerg_list ) 

- Cách liballoc chọn vùng theo size; vì sao __free chỉ chặn khi frame của vùng đang bị DMA ghim 

- Mục 9: kết luận hạn chếhướng mở 

### **Thành** 

Mục 6, 7.2, 8 · Q1, Q7 

###### **Nội dung** 

- Mục 6: user chỉ đọc ghi địa chỉ user; dữ liệu sang kernel đi qua 

- copy_from_user / copy_to_user ; syscall nhận **PID** , kernel tự tìm PCB 

- Swap: peek victim → swap có kiểm tra lỗi → remove; bỏ qua frame đang bịDMA ghim 

Mục 9: kết luận, hạn chế, hướng mở rộng 

bị g 

###### **Bằng chứng cần đưa vào** 

- Bảng trạng thái các vùng sau từng lệnh alloc / free của một test ( os_1_mlq_paging hoặc test 3 vùng mới) 

- Ví dụ size 512, 513, 4097 rơi đúng vùng; free rồi alloc lại dùng lại vùng trống 

- Mục 7.2: ghim trang, kéo trang từ SWAP lên trước khi ghim, chia đoạn theo trang 

- Mục 8: danh sách tài nguyên dùng chung và khóa tương ứng 

**Bằng chứng cần đưa vào** 

- Output một lệnh read / write vào địa chỉ kernel bị từ chối 

- Output DMA vắt qua nhiều trang: số đoạn được submit 

- Q7: kịch bản 2 CPU swap đúng frame đang được DMA chép (đã có sẵn trong report kiểm tra 24/11, mục 

