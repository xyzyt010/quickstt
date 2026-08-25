use eframe::NativeOptions;
fn main() {
    let mut opt = NativeOptions::default();
    opt.wgpu_options.supported_backends = eframe::wgpu::Backends::GL;
}
