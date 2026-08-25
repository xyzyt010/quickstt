use egui::{Color32, ColorImage, TextureHandle, TextureOptions};

const MIC_ACTIVE_SVG: &[u8] = include_bytes!("../../../QuickSTT_App/mic_active.svg");
const MIC_INACTIVE_SVG: &[u8] = include_bytes!("../../../QuickSTT_App/mic_inactive.svg");
const APP_ICON_SVG: &[u8] = include_bytes!("../../assets/app_icon.svg");

pub struct IconSet {
    pub mic_active: TextureHandle,
    pub mic_inactive: TextureHandle,
    pub app_icon: TextureHandle,
}

impl IconSet {
    pub fn load(ctx: &egui::Context, icon_size: u32) -> Self {
        let mic_active =
            render_svg_to_texture(ctx, "mic_active", MIC_ACTIVE_SVG, icon_size, icon_size);
        let mic_inactive =
            render_svg_to_texture(ctx, "mic_inactive", MIC_INACTIVE_SVG, icon_size, icon_size);
        let app_icon = render_svg_to_texture(ctx, "app_icon", APP_ICON_SVG, 32, 32);

        Self {
            mic_active,
            mic_inactive,
            app_icon,
        }
    }

    pub fn reload(&mut self, ctx: &egui::Context, icon_size: u32) {
        self.mic_active =
            render_svg_to_texture(ctx, "mic_active", MIC_ACTIVE_SVG, icon_size, icon_size);
        self.mic_inactive =
            render_svg_to_texture(ctx, "mic_inactive", MIC_INACTIVE_SVG, icon_size, icon_size);
    }
}

fn render_svg_to_texture(
    ctx: &egui::Context,
    name: &str,
    svg_data: &[u8],
    width: u32,
    height: u32,
) -> TextureHandle {
    let image = render_svg(svg_data, width, height);
    ctx.load_texture(name, image, TextureOptions::LINEAR)
}

fn render_svg(svg_data: &[u8], target_w: u32, target_h: u32) -> ColorImage {
    let opt = usvg::Options::default();
    match usvg::Tree::from_data(svg_data, &opt) {
        Ok(tree) => {
            let size = tree.size();
            let sx = target_w as f32 / size.width();
            let sy = target_h as f32 / size.height();
            let scale = sx.min(sy);
            let pw = (size.width() * scale).ceil() as u32;
            let ph = (size.height() * scale).ceil() as u32;

            if let Some(mut pixmap) = resvg::tiny_skia::Pixmap::new(pw.max(1), ph.max(1)) {
                resvg::render(
                    &tree,
                    resvg::tiny_skia::Transform::from_scale(scale, scale),
                    &mut pixmap.as_mut(),
                );
                let data = pixmap.data();
                let pixels: Vec<Color32> = data
                    .chunks_exact(4)
                    .map(|rgba| {
                        Color32::from_rgba_premultiplied(rgba[0], rgba[1], rgba[2], rgba[3])
                    })
                    .collect();

                let mut image = ColorImage::new([pw as usize, ph as usize], Color32::TRANSPARENT);
                image.pixels = pixels;
                pad_to_size(image, target_w as usize, target_h as usize)
            } else {
                fallback_image(target_w as usize, target_h as usize)
            }
        }
        Err(_) => fallback_image(target_w as usize, target_h as usize),
    }
}

fn pad_to_size(src: ColorImage, tw: usize, th: usize) -> ColorImage {
    if src.width() == tw && src.height() == th {
        return src;
    }
    let mut out = ColorImage::new([tw, th], Color32::TRANSPARENT);
    let ox = (tw.saturating_sub(src.width())) / 2;
    let oy = (th.saturating_sub(src.height())) / 2;
    for y in 0..src.height().min(th) {
        for x in 0..src.width().min(tw) {
            let dx = ox + x;
            let dy = oy + y;
            if dx < tw && dy < th {
                out[(dx, dy)] = src[(x, y)];
            }
        }
    }
    out
}

fn fallback_image(w: usize, h: usize) -> ColorImage {
    let mut img = ColorImage::new([w, h], Color32::TRANSPARENT);
    let cx = w / 2;
    let cy = h / 2;
    let r = w.min(h) / 2;
    for y in 0..h {
        for x in 0..w {
            let dx = x as i32 - cx as i32;
            let dy = y as i32 - cy as i32;
            if (dx * dx + dy * dy) <= (r * r) as i32 {
                img[(x, y)] = Color32::from_rgb(0x00, 0xAA, 0xFF);
            }
        }
    }
    img
}
