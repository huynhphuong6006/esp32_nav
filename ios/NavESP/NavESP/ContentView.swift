// Màn hình chính (nút bánh răng mở trang cài đặt): bản đồ chọn điểm đến (tìm kiếm hoặc giữ lâu trên bản đồ), xem trước tuyến,
// gửi xuống thiết bị và theo dõi trạng thái.
import MapKit
import SwiftUI

struct ContentView: View {
    @ObservedObject private var nav = NavController.shared
    @State private var camera: MapCameraPosition = .userLocation(fallback: .region(
        MKCoordinateRegion(center: CLLocationCoordinate2D(latitude: 12.6874, longitude: 108.0449),
                           latitudinalMeters: 4000, longitudinalMeters: 4000)))
    @State private var query = ""
    @State private var results: [MKMapItem] = []
    @FocusState private var searching: Bool
    @Namespace private var mapScope
    #if DEBUG
    @State private var showSettings = ProcessInfo.processInfo.environment["NAVESP_PREVIEW_SETTINGS"] != nil
    #else
    @State private var showSettings = false
    #endif

    var body: some View {
        ZStack(alignment: .top) {
            MapReader { proxy in
                Map(position: $camera, scope: mapScope) {
                    UserAnnotation()
                    if let d = nav.destination {
                        Marker(nav.destinationName, coordinate: d).tint(.red)
                    }
                    if let r = nav.route {
                        MapPolyline(coordinates: r.coordinates).stroke(.blue, lineWidth: 6)
                    }
                    if let st = nav.ble.status, st.lat != 0 {
                        Annotation("Thiết bị", coordinate: CLLocationCoordinate2D(latitude: st.lat, longitude: st.lon)) {
                            Image(systemName: "location.north.circle.fill")
                                .font(.title2).foregroundStyle(.green, .white)
                        }
                    }
                }
                .mapControls { }                     // nút bản đồ đặt riêng bên dưới ô tìm kiếm (tránh thanh trạng thái iPhone)
                .simultaneousGesture(
                    LongPressGesture(minimumDuration: 0.5)
                        .sequenced(before: DragGesture(minimumDistance: 0, coordinateSpace: .local))
                        .onEnded { value in
                            if case .second(true, let drag?) = value, let c = proxy.convert(drag.location, from: .local) {
                                searching = false
                                results = []
                                nav.setDestination(c)
                            }
                        })
            }
            .ignoresSafeArea()

            VStack(spacing: 8) {
                topBar
                if !results.isEmpty {
                    resultList
                } else {
                    HStack {
                        Spacer()
                        VStack(spacing: 8) {
                            MapUserLocationButton(scope: mapScope)
                            MapCompass(scope: mapScope)
                        }
                        .buttonBorderShape(.circle)
                    }
                }
                Spacer()
                bottomPanel
            }
            .padding(.horizontal, 12)
        }
        .mapScope(mapScope)
        .sheet(isPresented: $showSettings) { SettingsView() }
    }

    // MARK: Thanh trên: trạng thái kết nối + ô tìm kiếm

    private var topBar: some View {
        VStack(spacing: 6) {
            HStack {
                linkChip
                Spacer()
                if let st = nav.ble.status, st.sim { chip("SIM", color: .red) }
                Button { showSettings = true } label: {
                    Image(systemName: "gearshape.fill")
                        .font(.body)
                        .padding(7)
                        .background(.regularMaterial, in: Circle())
                }
                .accessibilityLabel("Cài đặt")
            }
            HStack {
                Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                TextField("Tìm điểm đến…", text: $query)
                    .focused($searching)
                    .submitLabel(.search)
                    .onSubmit { Task { await search() } }
                if !query.isEmpty {
                    Button { query = ""; results = [] } label: { Image(systemName: "xmark.circle.fill") }
                        .foregroundStyle(.secondary)
                }
            }
            .padding(10)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
        }
    }

    private var linkChip: some View {
        switch nav.ble.link {
        case .off: return chip("Bluetooth tắt", color: .gray)
        case .idle: return chip("Chưa kết nối", color: .gray)
        case .scanning: return chip("Đang tìm NavESP…", color: .orange)
        case .connecting: return chip("Đang kết nối…", color: .orange)
        case .connected: return chip("Đã kết nối NavESP", color: .green)
        }
    }

    private func chip(_ text: String, color: Color) -> some View {
        Text(text)
            .font(.caption.bold())
            .padding(.horizontal, 10).padding(.vertical, 5)
            .background(color.opacity(0.85), in: Capsule())
            .foregroundStyle(.white)
    }

    private var resultList: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                ForEach(results, id: \.self) { item in
                    Button {
                        searching = false
                        results = []
                        query = item.name ?? ""
                        let c = item.placemark.coordinate
                        nav.setDestination(c, name: item.name ?? "")
                        camera = .region(MKCoordinateRegion(center: c, latitudinalMeters: 3000, longitudinalMeters: 3000))
                    } label: {
                        VStack(alignment: .leading) {
                            Text(item.name ?? "").font(.body)
                            Text(item.placemark.title ?? "").font(.caption).foregroundStyle(.secondary).lineLimit(1)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.vertical, 8).padding(.horizontal, 10)
                    }
                    .foregroundStyle(.primary)
                    Divider()
                }
            }
        }
        .frame(maxHeight: 260)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    }

    private func search() async {
        let req = MKLocalSearch.Request()
        req.naturalLanguageQuery = query
        if let loc = nav.userLocation {
            req.region = MKCoordinateRegion(center: loc.coordinate, latitudinalMeters: 30000, longitudinalMeters: 30000)
        }
        results = (try? await MKLocalSearch(request: req).start().mapItems) ?? []
        if results.isEmpty { nav.message = "Không tìm thấy \"\(query)\"" }
    }

    // MARK: Bảng dưới: tuyến + nút + trạng thái thiết bị

    private var bottomPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let r = nav.route {
                HStack {
                    VStack(alignment: .leading) {
                        if nav.navigating {
                            Text(nav.rerouting ? "Đang tính lại đường…" : "Đang dẫn đường")
                                .font(.caption.bold()).foregroundStyle(nav.rerouting ? .orange : .blue)
                        }
                        Text(nav.destinationName).font(.headline).lineLimit(1)
                        Text("\(formatDist(r.distanceM)) · \(Int((r.durationS / 60).rounded(.up))) phút · \(r.steps.count) bước")
                            .font(.subheadline).foregroundStyle(.secondary)
                    }
                    Spacer()
                    if nav.busy { ProgressView() }
                }
            } else if nav.busy {
                HStack { ProgressView(); Text("Đang tính đường…") }
            } else {
                Text("Giữ lâu trên bản đồ hoặc tìm kiếm để chọn điểm đến").font(.subheadline).foregroundStyle(.secondary)
            }

            // Hàng nút chính: luôn 1 dòng, chiều cao cố định (không giãn khi đổi trạng thái)
            HStack(spacing: 8) {
                Button { nav.start() } label: {
                    Label(startTitle, systemImage: nav.navigating && !nav.routeChanged ? "location.north.line.fill" : "location.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .disabled(nav.route == nil || nav.ble.link != .connected || nav.busy || (nav.navigating && !nav.routeChanged))
                Button { nav.stop() } label: { Text("Dừng").frame(maxWidth: .infinity) }
                    .buttonStyle(.bordered)
                    .disabled(nav.ble.link != .connected || !nav.navigating)
                Button(role: .destructive) { nav.clear() } label: { Text("Xoá tuyến").frame(maxWidth: .infinity) }
                    .buttonStyle(.bordered)
            }
            .lineLimit(1)
            .minimumScaleFactor(0.8)
            .controlSize(.regular)
            .fixedSize(horizontal: false, vertical: true)

            // Hàng nút thử nghiệm (SIM) nhỏ, tách riêng
            HStack(spacing: 8) {
                Button { nav.toggleSim() } label: { Text(nav.ble.status?.sim == true ? "Tắt SIM" : "SIM") }
                    .buttonStyle(.bordered)
                    .tint(nav.ble.status?.sim == true ? .red : .accentColor)
                    .disabled(nav.ble.link != .connected)
                if nav.ble.status?.sim == true {
                    Button { nav.simOffRoute() } label: { Text("Lệch tuyến") }   // thử tự tính lại đường
                        .buttonStyle(.bordered)
                }
                Spacer()
            }
            .lineLimit(1)
            .controlSize(.small)

            if let st = nav.ble.status { deviceStatus(st) }
            if let m = nav.message { Text(m).font(.caption).foregroundStyle(.red) }
            if nav.route != nil {
                Text("Tuyến đường © OpenStreetMap contributors (OSRM)").font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(12)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
        .padding(.bottom, 8)
    }

    private func deviceStatus(_ st: DeviceStatus) -> some View {
        let state: (String, Color) = {
            switch st.state {
            case .idle: return ("Chưa dẫn đường", .gray)
            case .navigating: return ("Đang dẫn đường", .blue)
            case .offRoute: return ("Lệch tuyến", .red)
            case .arrived: return ("Đã đến nơi", .green)
            }
        }()
        return HStack(spacing: 10) {
            Text(state.0).font(.caption.bold()).foregroundStyle(state.1)
            if st.state == .navigating {
                Text("Rẽ sau \(formatDist(Double(st.distNextM)))").font(.caption)
                Text("Còn \(formatDist(Double(st.remainM)))").font(.caption)
            }
            Spacer()
            Text(st.gpsFix ? "GPS" : "Chưa có GPS").font(.caption2).foregroundStyle(st.gpsFix ? .green : .orange)
            if nav.rerouteCount > 0 { Text("Tính lại: \(nav.rerouteCount)").font(.caption2).foregroundStyle(.secondary) }
        }
    }

    /// Nút chính: "Bắt đầu" khi chưa dẫn, "Đang dẫn đường" (mờ) khi đang dẫn, "Đổi tuyến" khi vừa chọn điểm đến mới
    private var startTitle: String {
        guard nav.navigating else { return "Bắt đầu" }
        return nav.routeChanged ? "Đổi tuyến" : "Đang dẫn"
    }

    private func formatDist(_ m: Double) -> String {
        m < 1000 ? "\(Int((m / 10).rounded()) * 10) m" : String(format: "%.1f km", m / 1000)
    }
}
