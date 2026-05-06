# Clipboard manager — roadmap & implementation notes

Stack: C11, libui-ng (vendored). Mục tiêu: ứng dụng quản lý clipboard đa nền tảng (Linux, macOS, Windows).

Thứ tự phase: Phase 1 (core + UI danh sách) → Phase 2 (tray, nền, menu, Settings, About) → Phase 3 (phím tắt toàn cục + paste vào app đang focus). Phase 2 được đưa lên trước Phase 3 vì lifecycle daemon/tray gắn chặt với hotkey và quyền hệ thống; Phase 3 là phần khác biệt nhất giữa các OS.

---

## Phase 1 — Codebase + quản lý clipboard (màn hình chính)

### 1.1 Kiến trúc & tách lớp
- [ ] Module **clipboard store + watcher** (logic, ít phụ thuộc UI): lưu history, giới hạn số mục, API thêm/xóa/lấy.
- [ ] Module **UI libui-ng**: cửa sổ chính, danh sách/bảng, chi tiết, nút Copy/View.
- [ ] Định nghĩa kiểu dữ liệu mỗi item: `id`, nội dung đầy đủ (text), `created_at` (timestamp tuyệt đối), preview (n ký tự đầu — chỉ hiển thị), metadata nguồn (optional).

**Lưu ý**
- Tách watcher/store khỏi UI để khi đổi cách bắt sự kiện clipboard theo OS không phải viết lại giao diện.
- Tránh **vòng lặp clipboard**: khi app tự ghi clipboard (nút Copy), watcher không thêm trùng (so khớp nội dung, debounce, hoặc cờ “bỏ qua lần ghi tiếp theo từ app”).

### 1.2 Theo dõi clipboard (đa nền tảng)
- [ ] Linux: xác định X11 vs Wayland; hook/poll phù hợp từng môi trường.
- [ ] macOS: theo dõi thay đổi pasteboard (ví dụ `changeCount`).
- [ ] Windows: listener hoặc poll định kỳ an toàn.

**Lưu ý**
- **Wayland** có thể hạn chế so với X11; ship theo khả năng từng môi trường, ghi rõ trong README nếu cần.

### 1.3 Màn hình chính — danh sách item
- [ ] Hiển thị từng mục: ứng dụng nguồn (nếu có), preview n ký tự đầu, thời gian dạng “ago” (tính từ `created_at`).
- [ ] Hai hành động trên item: **Copy** (đưa full text lên system clipboard), **View** (màn hình / dialog xem chi tiết full text).

**Lưu ý**
- Trường **ứng dụng nguồn** không luôn có hoặc không chính xác trên mọi OS; UI hiển thị placeholder (ví dụ “—”) khi thiếu.
- “Ago” chỉ là lớp hiển thị; lưu timestamp tuyệt đối trong model.

### 1.4 Giới hạn số mục (logic)
- [ ] Cắt history khi vượt max (config sau này đọc từ Settings; Phase 1 có thể hardcode hoặc hằng số tạm).

**Lưu ý**
- Đồng bộ với Phase 2 Settings khi đã có file cấu hình.

### 1.5 (Tùy chọn sớm) Quyền riêng tư cơ bản
- [ ] Xóa một mục / xóa tất cả từ UI hoặc menu tạm.

**Lưu ý**
- Cân nhắc sau: danh sách đen app, không persist disk (chỉ RAM), v.v.

---

## Phase 2 — Tray, chạy nền, menu chính, Settings, About

### 2.1 Chạy nền & system tray
- [ ] Icon tray; menu tray (tối thiểu: mở cửa sổ, thoát hoàn toàn).
- [ ] Hành vi **đóng cửa sổ**: ẩn xuống tray thay vì thoát (trừ khi user chọn Quit từ tray/menu).

**Lưu ý**
- Kiểm tra **libui-ng** có API tray đủ trên Linux/macOS/Windows; nếu thiếu, cần lớp gọi API native theo OS.
- Người dùng thường kỳ vọng app clipboard “luôn chạy” khi đã bật — tránh mất hotkey vì tưởng đã tắt khi chỉ đóng cửa sổ.

### 2.2 Menu cửa sổ chính (top bar)
- [ ] Mục **Settings** — mở màn hình / dialog cài đặt.
- [ ] Mục **About** — màn hình thông tin tác giả / phiên bản / license.

**Lưu ý**
- Dùng API menu của libui-ng; giữ nhất quán với convention từng nền tảng (macOS application menu nếu có).

### 2.3 Settings — giới hạn số item
- [ ] Điều khiển (spinbox/slider) hoặc ô nhập: số mục tối đa trong history; validate min/max hợp lý.
- [ ] Lưu vào file cấu hình user; áp dụng ngay khi đổi (cắt bớt history nếu cần).

**Lưu ý**
- Đường dẫn config theo chuẩn OS (XDG, Application Support, `%APPDATA%`, v.v.).

### 2.4 Settings — phím tắt (chuẩn bị cho Phase 3)
- [ ] UI ghi nhận tổ hợp phím mong muốn; lưu vào config.
- [ ] Hiển thị conflict / “không đăng ký được” khi OS hoặc app khác đã chiếm shortcut.

**Lưu ý**
- Đăng ký **global** hotkey thực sự có thể thuộc Phase 3; Phase 2 chỉ cần lưu preference và UI ổn định.

### 2.5 About
- [ ] Nội dung tĩnh: tên app, phiên bản, credit lập trình viên, link repo (nếu có).

**Lưu ý**
- Có thể đọc version từ một macro/header build để không phải sửa tay mỗi release.

### 2.6 (Tùy chọn) Khởi động cùng hệ thống
- [ ] Tùy chọn “Start at login” nếu trong phạm vi dự án.

**Lưu ý**
- Cách đăng ký khác nhau theo OS; có thể để sau khi tray ổn định.

---

## Phase 3 — Phím tắt toàn cục + paste vào ô đang focus

### 3.1 Global hotkey
- [ ] Đăng ký phím tắt từ config (Phase 2); mở cửa sổ chính hoặc overlay chọn nhanh (tùy thiết kế).
- [ ] Hoàn tác / fallback khi đăng ký thất bại.

**Lưu ý**
- **macOS**: có thể cần **Accessibility** và/hoặc **Input Monitoring**.
- **Linux**: X11 vs Wayland — shortcut toàn cục và inject paste khác nhau; kiểm thử trên cả hai nếu có thể.
- **Windows**: xử lý focus, layout bàn phím, quyền.

### 3.2 Chọn mục từ history → đưa vào app đang focus
- [ ] **Mức 1 (nên có trước, portable hơn)**: hotkey / UI đưa text đã chọn lên **system clipboard**; user paste thủ công (Ctrl+V / Cmd+V).
- [ ] **Mức 2 (nâng cao theo OS)**: sau khi set clipboard, **mô phỏng** paste (Cmd+V / Ctrl+V) vào control đang focus.

**Lưu ý**
- Coi Mức 2 là **tăng dần theo từng OS**; ghi rõ trong tài liệu nền nào hỗ trợ đầy đủ.
- **Wayland** có thể không cho phép hoặc hạn chế inject; ưu tiên Mức 1 làm baseline.

### 3.3 Tích hợp với tray & lifecycle
- [ ] Hotkey chỉ hoạt động khi process còn chạy (tray); xử lý khi user Quit hoàn toàn.

**Lưu ý**
- Trùng với quyết định Phase 2: không thoát process khi chỉ đóng cửa sổ.

---

## Kiểm thử & tài liệu (xuyên suốt)

### Ma trận thủ công
- [ ] Linux: thử **Xorg** và **Wayland** (ví dụ Fedora) cho clipboard + (sau này) hotkey.
- [ ] macOS, Windows: smoke test build và quyền hệ thống.

**Lưu ý**
- Ghi known limitations vào README theo OS/session.

### Định dạng clipboard sau này
- Phase 1–3 ưu tiên **plain text**; rich text/HTML có thể là hướng mở rộng (watcher phức tạp hơn).

---

## Thứ tự gợi ý khi code

1. Phase 1.1 → 1.4 → 1.3 → 1.2 (có thể làm watcher theo một OS trước, rồi port).
2. Phase 2.1 → 2.2 → 2.5 → 2.3 → 2.4.
3. Phase 3.1 → 3.2 (Mức 1 trước) → 3.2 (Mức 2 từng OS) → 3.3.
