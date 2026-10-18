# Nhật Ký Thay Đổi Firmware ESP32 GNSS Gateway (Chế độ Base 4G / Rover)

Tài liệu này ghi lại chi tiết toàn bộ các cập nhật, tối ưu hóa và sửa đổi trong mã nguồn firmware ESP32 (chỉ bao gồm firmware ESP32, không ghi phần server CORS).

---

## 1. Tự động dừng Auto Base khi nạp tọa độ chính xác (`EXECUTE_RAW_COMMANDS`)
* **Tệp thay đổi**: `src/functions/Backend_Agent.cpp`
* **Bối cảnh & Vấn đề**:
  * Khi trạm đang chạy quy trình Auto Base (tạm thời biến thành Rover kéo RTCM từ trạm chuẩn về để đo tọa độ), nếu gặp sự cố (mất FIX, timeout, hoặc người dùng muốn chuyển gấp sang nhập tọa độ thủ công `FIXED_LLA`), ESP32 trước đây sẽ từ chối lệnh cấu hình mới với lỗi: `"stop the running base workflow before applying GNSS commands"`.
  * Người dùng bị kẹt và bắt buộc phải ấn nút "Dừng Auto Base" trên giao diện trước khi được phép nhập tọa độ thủ công.
* **Giải pháp đã triển khai**:
  * Trong khối xử lý lệnh `EXECUTE_RAW_COMMANDS`: kiểm tra nếu `workflowActive() == true`, firmware tự động phát cờ hủy task `requestWorkflowStop()`.
  * Chờ tối đa 1.5 giây (thực tế chỉ 50–100ms) để task Auto Base ngầm kết thúc vòng lặp, đóng socket NTRIP Rover, dọn dẹp semaphore và nhả quyền điều khiển cổng UART GNSS an toàn.
  * Tự động xóa chuỗi tiến trình `autoBaseProgress_`, đưa trạng thái hoạt động về `normalAgentState()`.
  * Sau khi dọn dẹp xong, thực thi ngay các lệnh nạp tọa độ Fixed LLA vào chip GNSS và lưu cấu hình vào Flash NVS.
  * Người dùng có thể chuyển đổi mượt mà giữa Auto Base và Fixed LLA mà không bao giờ bị lỗi xung đột lệnh.

---

## 2. Báo cáo Giám sát Bộ nhớ (RAM Heap & Flash Disk Telemetry)
* **Tệp thay đổi**: `src/functions/Backend_Agent.cpp`
* **Bối cảnh & Vấn đề**:
  * Server CORS cần biết chính xác tình trạng tài nguyên phần cứng của ESP32 để phát hiện kịp thời nguy cơ tràn bộ nhớ RAM (heap fragmentation) hoặc cạn kiệt dung lượng Flash khi lưu trữ nhiều bản ghi cấu hình.
* **Giải pháp đã triển khai**:
  * Trong hàm tạo gói tin trạng thái định kỳ `buildStatusPayload()`:
    * **`system.memory` (RAM Heap)**: Bổ sung các trường tính toán theo nhiều đơn vị:
      * `free_heap_bytes`: Dung lượng RAM còn trống (Bytes).
      * `total_heap_bytes`: Tổng dung lượng RAM Heap khả dụng (Bytes).
      * `used_bytes`, `used_kb`, `used_mb`: Dung lượng RAM đang sử dụng.
      * `total_kb`, `total_mb`: Tổng dung lượng RAM quy đổi ra KB và MB.
      * `percent`: Tỷ lệ phần trăm RAM đang dùng (0 - 100%).
    * **`system.disk` (Bộ nhớ Flash)**: Bổ sung giám sát bộ nhớ Flash vật lý:
      * `type`: `"Flash"`.
      * `total_bytes`, `free_bytes`, `used_bytes`: Dung lượng Flash tính bằng Bytes.
      * `total_mb`, `used_mb`, `total_gb`, `used_gb`: Quy đổi dung lượng sang MB và GB.
      * `percent`: Tỷ lệ phần trăm Flash đang chiếm dụng.

---

## 3. Gửi GGA Định kỳ trong Phiên Rover và Auto Base
* **Tệp thay đổi**: `src/functions/NTRIP_Handler_IP.cpp`
* **Bối cảnh & Vấn đề**:
  * Nhiều NTRIP Caster ngoài (đặc biệt là Caster mạng lưới trạm ảo VRS như Cục Đo Đạc, VNGEONET) yêu cầu thiết bị Rover phải liên tục gửi câu NMEA GGA định kỳ (5–10 giây/lần). Nếu không có GGA gửi lên, Caster sẽ từ chối cấp dòng cải chính RTCM hoặc ngắt kết nối do coi là client đã chết.
* **Giải pháp đã triển khai**:
  * Hàm `sendRoverGgaIfAvailable(Client &client, const bool force = false)`:
    * Bắn câu GGA ngay lập tức khi vừa bắt tay NTRIP thành công (`force = true`).
    * Trong vòng lặp `loopNtripRover()`: Kiểm tra `millis() - roverLastGgaMs >= 5000UL`, tự động lấy câu `$GPGGA` tươi mới nhất từ chip GNSS (trong vòng 3000ms) để bắn lên Caster mỗi 5 giây.
  * Giữ phiên kéo RTCM luôn ổn định xuyên suốt quá trình Auto Base.

---

## 4. Cơ chế Khóa Trạm Phân vùng (Scoped Station Locking)
* **Tệp thay đổi**: `include/functions/Station_Lock.h`, `include/functions/Backend_Agent.h`, `src/functions/Backend_Agent.cpp`
* **Bối cảnh & Vấn đề**:
  * Cần có khả năng khóa xuất dữ liệu RTCM lên từng máy chủ độc lập (ví dụ máy chủ 1 hoặc máy chủ 2) mà không làm gián đoạn máy chủ còn lại hoặc khóa toàn bộ thiết bị.
* **Giải pháp đã triển khai**:
  * Tạo cấu trúc `StationLock` trong `include/functions/Station_Lock.h`:
    * Quản lý bitmask `servers` (server 1, server 2) và cờ `all` (khóa toàn phần).
    * Hỗ trợ lưu trữ trạng thái khóa vào Preferences NVS (`STATION_LOCK`), tự động khôi phục sau khi khởi động lại.
  * Tích hợp xử lý lệnh `LOCK_DEVICE` và `UNLOCK_DEVICE` trong `Backend_Agent.cpp`:
    * Khóa phân vùng: chỉ ngắt kết nối đẩy RTCM của máy chủ được chỉ định, các kênh MQTT, WebSocket và cấu hình chip GNSS vẫn hoạt động bình thường.
    * Báo cáo cấu trúc `system_info.device_lock` với các cờ `scoped`, `all`, `locked_servers` để Dashboard hiển thị chính xác.

---

## 5. Định tuyến Cổng Lệnh GNSS cho Unicore và u-blox
* **Tệp thay đổi**: `src/functions/cmd_handler.cpp`, `src/functions/ubx_cmd_builder.cpp`, `include/functions/ubx_cmd_builder.h`, `include/functions/Unicore_Output.h`
* **Bối cảnh & Vấn đề**:
  * Bộ command builder từ server CORS sinh ra các lệnh theo cổng mặc định (như COM3 cho Unicore, hoặc USB/UART2 cho u-blox). Khi nạp vào ESP32, nếu không đổi đúng cổng UART vật lý đang nối thì module GNSS sẽ không nhận được lệnh.
* **Giải pháp đã triển khai**:
  * **Unicore (UM980/UM982)**: Tự động phát hiện và chuyển đổi các câu lệnh cấu hình log sang cổng `GNSS_UNICORE_PORT` (mặc định COM2) trước khi gửi qua UART.
  * **u-blox (ZED-F9P)**: Tự động chuyển đổi cổng output cấu hình sang UART1 và tính toán lại checksum 2-byte (Fletcher algorithm) của giao thức nhị phân UBX.

---

## 6. Linh hoạt Cổng WebSocket Backend qua NVS
* **Tệp thay đổi**: `src/functions/Backend_Agent.cpp`
* **Bối cảnh & Vấn đề**:
  * Trước đây cổng WebSocket được gán cứng qua macro `BACKEND_WEBSOCKET_PORT`. Khi server đổi cổng (ví dụ qua proxy hoặc reverse port), thiết bị không kết nối được nếu không nạp lại code.
* **Giải pháp đã triển khai**:
  * Trong `setupWebsocket()`: Đọc `BACKEND_PORT` từ Preferences `myPrefs`. Nếu chưa cài đặt thì fallback về cổng mặc định 8000.
  * Có thể đổi cổng trực tiếp từ lệnh: `ATG ESP SET BACKEND PORT <cổng>`.

---

## 7. Đảm bảo An toàn Bộ nhớ Flash NVS khi Lưu Cấu hình
* **Tệp thay đổi**: `src/functions/Backend_Agent.cpp`
* **Bối cảnh & Vấn đề**:
  * Khi serialize JSON cấu hình dịch vụ hoặc kết quả lệnh vào các biến toàn cục, nếu không xóa trắng bộ đệm trước thì có nguy cơ bị dính chuỗi rác cũ từ lần chạy trước khi ghi xuống Flash.
* **Giải pháp đã triển khai**:
  * Bổ sung `.clear()` trước mỗi lần `serializeJson()` cho các biến `lastCommandResult_`, `serviceConfig_`, `baseConfig_`.
  * Đảm bảo tính toàn vẹn của dữ liệu trong Flash NVS qua mọi chu kỳ khởi động.

---

## 8. Tối ưu Hóa Bộ nhớ RAM Heap và Độ chính xác Giám sát Flash
* **Tệp thay đổi**: `src/main.cpp`, `src/functions/Backend_Agent.cpp`
* **Bối cảnh & Vấn đề**:
  * ESP32 chạy đồng thời nhiều tác vụ FreeRTOS (4G TinyGSM, MQTT, NTRIP, Command Handler). Vùng nhớ RAM mặc định bị bộ điều khiển Bluetooth chiếm dụng ~35KB dù thiết bị chỉ dùng 4G SIM / WiFi.
  * Chỉ số Flash trước đó dùng trừ `sketchFree` từ app partition khiến số liệu Flash used bị lệch so với toàn bộ dung lượng chip Flash vật lý (4MB).
* **Giải pháp đã triển khai**:
  * **Giải phóng RAM Bluetooth**: Thêm `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` ngay đầu hàm `setup()` trong `src/main.cpp`. Trả ngay lập tức ~35KB DRAM về FreeRTOS Heap (giảm tỷ lệ RAM used từ ~50% xuống còn ~38-40%, tăng vùng nhớ trống lên ~190KB).
  * **Đo chuẩn xác Flash**: Cập nhật `disk["used_bytes"] = ESP.getSketchSize()` và `disk["free_bytes"] = flashTotal - flashUsed` trong `src/functions/Backend_Agent.cpp`. Báo cáo chính xác dung lượng nhị phân firmware đang chiếm (khoảng 0.96MB / 4.0MB Flash ~ 24%), vùng còn lại 3.04MB sẵn sàng cho phân vùng OTA dự phòng và NVS.

---

## 9. Tự động Nhận diện Loại Chip GNSS (Auto-Detect Chip theo chuẩn agent_universal.py)
* **Tệp thay đổi**: `include/functions/Backend_Agent.h`, `src/functions/Backend_Agent.cpp`, `src/functions/RTCM_Receiver.cpp`, `src/main.cpp`
* **Bối cảnh & Vấn đề**:
  * Các mạch ESP32 mới nạp hoặc chưa từng cấu hình Base có giá trị `SENSOR_TYPE` mặc định là `"UNKNOWN"`.
  * Trên giao diện Dashboard xuất hiện cảnh báo vàng: *"Không thể nhận diện loại chip (hiện tại là: Không xác định). Vui lòng chọn thủ công"*, yêu cầu người dùng phải tự nhớ và chọn tay chip Ublox hay Unicorecomm.
* **Giải pháp đã triển khai (kế thừa đầy đủ logic từ `cors/geodetic/agent_universal.py`)**:
  * Hàm `BackendAgent::detectGnssChip(Stream &stream, bool force)` được kích hoạt ngay lúc boot:
    1. **Active Probe Unicorecomm (UM980, UM982, UB4...)**: Gửi chuỗi lệnh `unlog\r\n` và `version\r\n` qua UART `Serial1`. Quét các từ khóa phản hồi: `UM980`, `UM982`, `UM9`, `Unicore`, `UB4`, `#VERSION`. Nếu khớp, tự động xác định là `Unicorecomm` và lưu NVS.
    2. **Active Probe U-blox (ZED-F9P, NEO-M8P...)**: Gửi gói tin nhị phân UBX poll `UBX-MON-VER` (`\xB5\x62\x0A\x04\x00\x00\x0E\x34`). Nếu nhận lại header phản hồi `0xB5 0x62 0x0A 0x04`, tự động xác định là `Ublox` và lưu NVS.
    3. **Passive Scan NMEA/RTCM**: Lắng nghe dòng dữ liệu NMEA lúc khởi động hoặc trong quá trình nhận luồng RTCM (`RTCM_Receiver.cpp`). Nếu phát hiện câu đặc trưng `$PUBX`, tự động nhận dạng là `Ublox`.
  * Khi nhận diện thành công, chip sẽ tự lưu cấu hình vào Flash NVS, trạm báo thẳng `detected_chip_type` lên CORS Server $\rightarrow$ Biển cảnh báo vàng trên web biến mất hoàn toàn.
