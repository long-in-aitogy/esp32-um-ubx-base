# Hướng dẫn sử dụng thiết bị Base GNSS qua 4G

## Quá trình hoạt động

Tùy theo điều kiện mạng, ESP32 sẽ mất khoảng 15 đến 20 giây để khởi động, kết nối mạng 4G, đăng nhập MQTT broker và NTRIP caster. Khi kết nối và đăng nhập thành công, ESP32 sẽ lấy dữ liệu RTCM từ module GNSS và gửi lên NTRIP caster, đồng thời gửi một bản sao của bản tin này lên một topic MQTT.

Để thông báo trạng thái sức khỏe thiết bị, mỗi 30 giây ESP32 sẽ gửi một bản tin health check lên topic MQTT đã cấu hình. Bản tin này có định dạng JSON như sau:

```json
{
    "uptime_s": 123456,
    "free_heap_bytes": 123456,
    "connected_via": "GSM",
    "rssi_dbm": "-77",
    "mqtt_ok": true,
    "ntrip_ok": true,
    "gnss_data_ok": true,
    "rtcm_types": {
        "1005": 12,
        "1074": 8,
        "1077": 10,
        "1084": 0,
        "1087": 0,
        "1094": 0,
        "1097": 0,
        "1124": 0,
        "1127": 0,
        "1230": 0
    }
}
```

## Định dạng bản tin Health Check:

- `uptime_s`: thời gian hoạt động của ESP32, tính bằng giây. Sẽ reset về 0 khi ESP32 khởi động lại.
- `free_heap_bytes`: dung lượng RAM còn trống, tính bằng byte.
- `connected_via`: phương thức kết nối mạng hiện tại, có thể là `GSM` hoặc `WIFI`.
- `rssi_dbm`: cường độ tín hiệu mạng hiện tại, tính bằng dBm.
- `mqtt_ok`: trạng thái kết nối MQTT, `true` nếu đang kết nối, `false` nếu không.
- `ntrip_ok`: trạng thái kết nối NTRIP, `true` nếu đang kết nối, `false` nếu không.
- `gnss_data_ok`: trạng thái dữ liệu GNSS, `true` nếu đang nhận dữ liệu RTCM từ module GNSS, `false` nếu không.
- `rtcm_types`: object chứa số lần xuất hiện của từng loại bản tin RTCM trong khoảng 30 giây gần nhất, tương ứng với một chu kỳ health check. Firmware hiện theo dõi các loại `1005`, `1074`, `1077`, `1084`, `1087`, `1094`, `1097`, `1124`, `1127` và `1230`. Giá trị bằng `0` nếu loại bản tin không xuất hiện trong chu kỳ hiện tại.

## Gửi lệnh qua MQTT hoặc Serial Monitor:

Thiết bị nhận nội dung (payload) từ Serial (nếu kết nối serial với máy tính hoặc điện thoại) hoặc qua topic lệnh MQTT đã cấu hình. Mỗi lệnh phải bắt đầu bằng `ATG`; các thành phần được ngăn cách bằng khoảng trắng. Từ khóa phân biệt chữ hoa/chữ thường. Giá trị không được chứa khoảng trắng (ví dụ mật khẩu có khoảng trắng hiện chưa được hỗ trợ).

### Cấu trúc chung

```text
ATG <NHÓM_LỆNH> <THAO_TÁC> [THAM_SỐ...]
```

Trong đó:

- `ATG`: tiền tố bắt buộc để firmware nhận diện lệnh.
- `NHÓM_LỆNH`: thành phần cần tác động: `GNSS`, `ESP`, `MQTT`, `NTRIP` hoặc `CONFIG`.
- `THAO_TÁC` và `THAM_SỐ`: phụ thuộc vào từng nhóm lệnh bên dưới.

Các lệnh `SET` lưu giá trị vào bộ nhớ Preferences (NVS) của ESP32. Khi thay đổi thông số kết nối đang hoạt động, nên khởi động lại thiết bị để các kết nối được tạo lại với cấu hình mới.

### GNSS

Các lệnh này cấu hình module GNSS ở chế độ base và được gửi qua UART đến module.

| Cú pháp | Giải thích |
| --- | --- |
| `ATG GNSS BASE SURVEY_IN <thời_gian> <độ_chính_xác>` | Bật chế độ khảo sát vị trí base. `<thời_gian>` là thời gian khảo sát tối thiểu, tính bằng giây; `<độ_chính_xác>` là ngưỡng độ chính xác, tính bằng mét. Ví dụ: `ATG GNSS BASE SURVEY_IN 300 1.0`. |
| `ATG GNSS BASE FIXED <vĩ_độ> <kinh_độ> <độ_cao> <độ_chính_xác>` | Cấu hình vị trí base cố định theo LLA. Vĩ độ và kinh độ ở đơn vị độ thập phân, độ cao và độ chính xác ở mét. Ví dụ: `ATG GNSS BASE FIXED 10.7769 106.7009 12.5 0.5`. |
| `ATG GNSS BASE <RTCM_MSG_TYPE> <PORT>` | Bật loại bản tin RTCM trên cổng GNSS chỉ định. Với Unicore dùng `COM1`, `COM2` hoặc `COM3`; với UBlox dùng `UART1`, `UART2` hoặc `USB`. Ví dụ: `ATG GNSS BASE 1074 COM2`. |

### ESP

| Cú pháp | Giải thích |
| --- | --- |
| `ATG ESP RESTART` | Khởi động lại ESP32 ngay lập tức. |
| `ATG ESP SET CONNECTION 4G` | Chuyển phương thức kết nối mạng sang 4G và khởi động lại ESP32 để áp dụng. |
| `ATG ESP SET CONNECTION WIFI` | Chuyển phương thức kết nối mạng sang Wi-Fi và khởi động lại ESP32 để áp dụng. |
| `ATG ESP SET GNSS TX <GPIO>` | Lưu chân GPIO truyền UART từ ESP32 đến GNSS. Ví dụ: `ATG ESP SET GNSS TX 17`. |
| `ATG ESP SET GNSS RX <GPIO>` | Lưu chân GPIO nhận UART từ GNSS về ESP32. Ví dụ: `ATG ESP SET GNSS RX 16`. |
| `ATG ESP SET WIFI SSID <ssid>` | Lưu tên mạng Wi-Fi cần kết nối. |
| `ATG ESP SET WIFI PASS <mật_khẩu>` | Lưu mật khẩu mạng Wi-Fi. |
| `ATG ESP SET 4G APN <apn>` | Lưu APN của nhà mạng 4G. Ví dụ: `ATG ESP SET 4G APN v-internet`. |
| `ATG ESP SET 4G USER <tên_người_dùng>` | Lưu tên người dùng APN 4G. Ví dụ: `ATG ESP SET 4G USER user`. |
| `ATG ESP SET 4G PASS <mật_khẩu>` | Lưu mật khẩu APN 4G. Ví dụ: `ATG ESP SET 4G PASS password`. |

### MQTT

| Cú pháp | Giải thích |
| --- | --- |
| `ATG MQTT SET SERVER <địa_chỉ>` | Lưu tên miền hoặc địa chỉ IP của MQTT broker. Ví dụ: `ATG MQTT SET SERVER broker.example.com`. |
| `ATG MQTT SET PORT <cổng>` | Lưu cổng MQTT (số nguyên 0–65535). Ví dụ: `ATG MQTT SET PORT 1883`. |
| `ATG MQTT SET USER <tên_người_dùng>` | Lưu tên người dùng đăng nhập MQTT. |
| `ATG MQTT SET PASS <mật_khẩu>` | Lưu mật khẩu đăng nhập MQTT. |
| `ATG MQTT SET PUBTPCHEALTH <topic>` | Lưu topic publish dữ liệu health check. Ví dụ: `ATG MQTT SET PUBTPCHEALTH tdm2402/node-01/health`. |
| `ATG MQTT SET SUBTPCCMD <topic>` | Lưu topic subscribe để nhận lệnh MQTT. Ví dụ: `ATG MQTT SET SUBTPCCMD tdm2402/node-01/cmd`. |

### NTRIP

| Cú pháp | Giải thích |
| --- | --- |
| `ATG NTRIP SET CSTRADDR <địa_chỉ>` | Lưu tên miền hoặc địa chỉ IP của NTRIP caster. |
| `ATG NTRIP SET CSTRPORT <cổng>` | Lưu cổng của NTRIP caster (số nguyên 0–65535), thường là `2101`. |
| `ATG NTRIP SET MNTPNT <mountpoint>` | Lưu mountpoint cần kết nối. Ví dụ: `ATG NTRIP SET MNTPNT VRS_RTCM32`. |
| `ATG NTRIP SET CSTRAUTH <chuỗi_xác_thực>` | Lưu chuỗi xác thực NTRIP theo định dạng base64. Giá trị này thường là base64 của `username:password`. |

### Cấu hình hệ thống

| Cú pháp | Giải thích |
| --- | --- |
| `ATG CONFIG RESET` | Đánh dấu khôi phục cấu hình mặc định, sau đó khởi động lại ESP32. Lần khởi động kế tiếp sẽ xóa toàn bộ cấu hình đã lưu trong Preferences và nạp lại giá trị mặc định. |

> Lưu ý: Firmware hiện không phản hồi trạng thái lệnh qua MQTT. Theo dõi Serial Monitor để kiểm tra log xử lý lệnh; riêng lệnh GNSS sẽ báo lỗi khi thiếu hoặc thừa tham số.