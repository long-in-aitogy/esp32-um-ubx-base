# Base 4G di động và CORS

ESP32 chạy firmware C++, nhận lệnh và gửi trạng thái theo giao thức của
`cors/geodetic/agent_universal.py`. Python agent không chạy trên ESP32.
Mặc định thiết bị dùng 4G; có thể chuyển sang Wi-Fi bằng lệnh cấu hình sẵn có.

## Chức năng

| Chọn trên CORS | Lệnh xuống ESP32 | Hoạt động |
| --- | --- | --- |
| Tọa độ chính xác (`FIXED_LLA`) | `EXECUTE_RAW_COMMANDS` + `original_config` | Gửi nguyên lệnh tọa độ do backend tạo, lưu cấu hình và loại chip. |
| Survey-in (`SURVEY_IN`) | `EXECUTE_RAW_COMMANDS` + `original_config` | Gửi lệnh khảo sát do backend tạo. Unicore dùng thời gian khảo sát; u-blox có thêm ngưỡng chính xác, đúng theo command builder của CORS. |
| Auto base (`AUTO_CORS`) | `TRIGGER_AUTO_BASE` | Chuyển rover, nhận cải chính CORS, đợi RTK FIX/FLOAT ổn định và lấy mẫu, đặt lại base rồi phát RTCM tới caster. |
| Khóa/mở trạm | `LOCK_DEVICE` / `UNLOCK_DEVICE` | Giữ kênh quản lý; ngừng output RTCM theo phạm vi khóa. |

Base di động ở đây là bộ base có thể mang đến điểm đặt mới. Khi phát cải chính,
anten vẫn phải đứng yên. Sau khi chuyển vị trí, chọn lại một trong ba chế độ;
không tiếp tục dùng tọa độ của điểm đặt cũ.

Loại GNSS được chọn từ CORS (`sensor_type`: `Ublox` hoặc `Unicorecomm`), lưu
trong NVS và dùng cho auto base. Cùng một firmware chứa cả hai bộ lệnh.
`UNKNOWN` lúc chưa cấu hình là trạng thái bình thường; firmware chưa tự nhận
diện model bằng truy vấn phần cứng. Nhánh u-blox yêu cầu receiver hỗ trợ
UBX-CFG-TMODE3, như ZED-F9P. Nhánh Unicore dùng bộ lệnh UM980/UM982 trong CORS.

## Kết nối phần cứng

- ESP32 TDM240x: RX GNSS GPIO18, TX GNSS GPIO19; UART GNSS 115200 baud.
- Unicore: nối cổng COM2 của receiver theo cấu hình mặc định. Có thể đặt
  `-DGNSS_UNICORE_PORT=1` hoặc `3` khi biên dịch nếu dây nối dùng cổng khác.
- u-blox: nối UART1, đặt UART1 115200 baud trước khi dùng. Firmware chuyển
  cấu hình output USB/UART2 do backend tạo sang UART1 và tính lại checksum UBX.
- Modem: RX ESP32 GPIO16, TX ESP32 GPIO17; APN theo SIM/nhà mạng.

Firmware không còn gửi lệnh khởi tạo UM980 vô điều kiện lúc boot. Lệnh GNSS
được chọn khi cấu hình base từ CORS. Với Unicore, các lệnh log COM3 do backend
tạo được chuyển sang cổng nối ESP32. Tọa độ và thời gian khảo sát giữ nguyên.

## Đưa thiết bị lên CORS

1. Build và nạp firmware, mở Serial Monitor 115200 baud:

   ```sh
   pio run -e tdm240x_dev_general
   pio run -e tdm240x_dev_general -t upload
   pio device monitor -b 115200
   ```

2. Cấu hình APN và broker MQTT **cùng broker backend CORS đang dùng**. Thay
   các giá trị ví dụ bằng thông tin triển khai thực tế:

   ```text
   ATG ESP SET 4G APN <apn>
   ATG MQTT SET SERVER <mqtt-host>
   ATG MQTT SET PORT 1883
   ATG MQTT SET USER <mqtt-user>
   ATG MQTT SET PASS <mqtt-password>
   ATG ESP SET BACKEND HOST <backend-host>
   ATG ESP SET BACKEND PORT 8000
   ATG ESP SET CONNECTION 4G
   ```

   Lệnh cuối khởi động lại để áp dụng. Kênh WebSocket hiện dùng `ws`, không hỗ
   trợ `wss`; MQTT vẫn là kênh quản lý khi không dùng được WebSocket. Host/port
   phải trỏ tới endpoint thiết bị thực tế của CORS, không phải URL trang web.

3. Lấy serial `esp_...` từ log `[BACKEND] Device Serial`. Trên CORS, tìm thiết
   bị này, nhận/cấp thiết bị và triển khai license theo luồng quản lý hiện có.
   Khi chưa có license, thiết bị vẫn báo trạng thái nhưng không phát RTCM.

4. Chọn đúng chip và một trong ba chế độ. Với auto base, backend phải có tài
   khoản/mountpoint cải chính trong `AUTO_BASE_CORS_*`. Với fixed, độ cao là
   độ cao ellipsoid, không tự thay bằng độ cao thủy chuẩn.

5. Cấu hình dịch vụ caster/mountpoint đích từ CORS. Thiết bị hỗ trợ hai máy
   chủ NTRIP. Theo dõi trạng thái GNSS, tọa độ, RTK FIX, vệ tinh, tốc độ RTCM,
   kết nối từng máy chủ và `last_command_result` trên hệ thống hiện có.

Không cần tạo dashboard hoặc API riêng cho ESP32. Các topic sử dụng:

```text
pi/devices/<serial>/status
pi/devices/<serial>/raw_data
pi/devices/<serial>/command
pi/devices/<serial>/control_ack
```

WebSocket dùng `/ws/pi/<serial>?token=<license>` và các gói `status_update`,
`nmea_update` tương ứng backend hiện có. Firmware nhận cả token `dt_...` do
backend cấp và license số cũ. `DEPLOY_LICENSE` chỉ báo thành công sau khi token
hợp lệ và lưu NVS thành công; token sai không ghi đè token đang dùng.
Không ghi license vào log.

## Khóa trạm

- `LOCK_DEVICE`, payload `{}`: khóa toàn thiết bị, chặn lệnh ngoài khóa/mở và
  yêu cầu dừng workflow đang chạy.
- `LOCK_DEVICE`, payload `{"locked_servers":[2]}`: chỉ chặn máy chủ 2; máy
  chủ 1 và điều khiển GNSS tiếp tục hoạt động.
- `[1,2]`: khóa cả hai output, vẫn cho quản lý GNSS như Python agent.
- `[]` hoặc `UNLOCK_DEVICE`: bỏ khóa. Việc mở khóa không tự bật một máy chủ
  đã bị tắt trong cấu hình dịch vụ.
- Lưu khóa vào NVS, nạp lại sau reboot. Khóa output luôn ưu tiên hơn fail-open.
- `system_info.device_lock` báo `scoped`, `all`, `locked_servers`, để CORS nhận
  biết firmware hỗ trợ khóa theo từng máy chủ.

Các payload trên mô tả nội dung lệnh; gửi qua cơ chế command dispatcher của
CORS để giữ đúng envelope, command ID và quyền người vận hành.

## Kiểm tra

Chạy kiểm tra host, không cần gắn ESP32:

```sh
c++ -std=c++17 -fsanitize=address,undefined -g -Itest/host -Iinclude \
  test/host/base_checks.cpp src/functions/ubx_cmd_builder.cpp \
  -o /tmp/esp32-base-checks
/tmp/esp32-base-checks
python3 -B test/host/check_cors.py ../cors /tmp/esp32-base-checks
```

Kiểm tra này chạy bộ tạo lệnh C++ thật, kiểm tra khóa, checksum, routing UART
và đưa lệnh sinh từ `cors_dashboard/backend/app/command_builder.py` thật qua
bộ routing firmware cho fixed/survey của cả hai loại chip.

Kiểm tra thực địa còn cần: nhận đúng GNSS, survey hoàn tất, auto base đạt FIX,
rover nhận được cải chính, độ đúng tọa độ và khóa/mở từng máy chủ sau reboot.
Trạng thái lệnh thành công hiện xác nhận gửi UART; chưa phải readback xác nhận
receiver đã chấp nhận cấu hình. ESP32 chuyển tiếp RTCM nguyên byte, không giải
mã datum RTCM 1021/1023/1025 như Python agent. Phần chuyển VN2000 hiện có của
ESP32 dùng tham số tích hợp/tỉnh; cần kiểm chứng hệ tọa độ thực tế trước khi
dùng kết quả auto base cho đo đạc.
