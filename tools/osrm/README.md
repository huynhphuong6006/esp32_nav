# Tự dựng máy chủ chỉ đường OSRM cho xe máy

`motorcycle.lua`: profile OSRM cho xe máy ở Việt Nam, sửa từ `car.lua` của osrm-backend:

- xét thẻ `motorcycle` trước (kể cả `oneway:motorcycle`);
- **không đi `motorway`/`motorway_link`** (xe máy không vào cao tốc);
- tốc độ theo Việt Nam: trunk 60, primary 50, secondary 45, tertiary 40, residential 25 km/h; tối đa 70 km/h;
- cho đi `track`, hẻm ít bị phạt hơn ô tô.

## Cần

- Máy 8 GB RAM trở lên (mini PC, máy cũ) hoặc VPS 4 GB, Docker. Bước extract cần khoảng 6–8 GB RAM: thiếu thì thêm swap (máy 8 GB chạy trong máy ảo 6 GB cần thêm khoảng 12 GB swap). Lần đầu mất từ vài chục phút tới khoảng 1 giờ.
- Vài GB ổ đĩa (dữ liệu sau xử lý khoảng 4 GB).

## Chạy

```
./build.sh            # tải dữ liệu Việt Nam (nếu chưa có), extract + partition + customize
./serve.sh            # chạy osrm-routed ở 127.0.0.1:5000, tự chạy lại cùng Docker
curl 'http://127.0.0.1:5000/route/v1/driving/108.0449,12.6874;108.1200,12.6680?overview=false'
```

Cập nhật dữ liệu: chạy lại `build.sh` rồi `serve.sh` (ví dụ hằng tháng).

## Đưa ra internet cho app

App gọi `https://<địa chỉ>/route/v1/driving/...` và gửi khoá trong header `X-Route-Key`. Nên đặt máy chủ sau một reverse proxy HTTPS kiểm tra khoá, chỉ mở đường dẫn `/route/v1/driving/` và giới hạn lượt gọi. Ví dụ nginx:

```nginx
limit_req_zone $binary_remote_addr zone=route:1m rate=30r/m;
server {
    server_name route.example.com;
    location /route/v1/driving/ {
        if ($http_x_route_key != "KHOA_CUA_BAN") { return 403; }
        limit_req zone=route burst=20 nodelay;
        proxy_pass http://127.0.0.1:5000;
    }
    location / { return 403; }
    # listen 443 ssl + chứng chỉ (certbot --nginx -d route.example.com)
}
```

Máy chủ ở nhà (sau router) thì có thể dùng Cloudflare Tunnel hoặc Tailscale thay cho mở cổng.

Trong app: Cài đặt → Máy chủ chỉ đường → nhập `https://route.example.com` và khoá, hoặc tạo mã QR chứa `navesp://server?url=https%3A%2F%2Froute.example.com&key=KHOA_CUA_BAN` để người được mời quét.
