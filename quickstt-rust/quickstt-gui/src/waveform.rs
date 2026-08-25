use egui::{Color32, Painter, Pos2, Rect, Rounding, Vec2};

const MIN_BAR_COUNT: usize = 24;
const MAX_BAR_COUNT: usize = 140;
const BASELINE_WIDTH: f32 = 220.0;
const BASELINE_HEIGHT: f32 = 28.0;

pub struct WaveformState {
    pub target_levels: Vec<f32>,
    pub display_levels: Vec<f32>,
    pub history_limit: usize,
    pub can_show: bool,
    pub start_time: Option<std::time::Instant>,
}

impl WaveformState {
    pub fn new() -> Self {
        Self {
            target_levels: Vec::new(),
            display_levels: Vec::new(),
            history_limit: 60,
            can_show: false,
            start_time: None,
        }
    }

    pub fn start(&mut self) {
        self.start_time = Some(std::time::Instant::now());
        self.can_show = false;
    }

    pub fn stop(&mut self) {
        self.start_time = None;
        self.can_show = false;
        self.target_levels.clear();
        self.display_levels.clear();
    }

    pub fn push_level(&mut self, level: f32) {
        if let Some(start) = self.start_time {
            if start.elapsed().as_millis() >= 45 {
                self.can_show = true;
            }
        }

        self.target_levels.push(level.clamp(0.0, 1.0));
        while self.target_levels.len() > self.history_limit {
            self.target_levels.remove(0);
        }

        while self.display_levels.len() < self.target_levels.len() {
            self.display_levels.push(0.0);
        }
        while self.display_levels.len() > self.history_limit {
            self.display_levels.remove(0);
        }
    }

    pub fn tick(&mut self) {
        for i in 0..self.display_levels.len().min(self.target_levels.len()) {
            let target = self.target_levels[i];
            let current = self.display_levels[i];
            if target > current {
                self.display_levels[i] = current + (target - current) * 0.74;
            } else {
                self.display_levels[i] = current + (target - current) * 0.46;
            }
        }
    }

    pub fn update_history_limit(&mut self, rect_width: f32) {
        self.history_limit = ((rect_width / 5.9) as usize).clamp(MIN_BAR_COUNT, MAX_BAR_COUNT);
    }
}

struct WaveLayout {
    slot_count: usize,
    start_x: f32,
    slot_step: f32,
    line_width: f32,
    dot_size: f32,
    min_line_height: f32,
    max_line_height: f32,
    dot_threshold: f32,
    low_line_threshold: f32,
}

fn build_layout(rect: Rect) -> WaveLayout {
    let w_scale = (rect.width() / BASELINE_WIDTH).clamp(0.85, 1.9);
    let h_scale = (rect.height() / BASELINE_HEIGHT).clamp(0.85, 2.6);

    let base_lw = if rect.width() >= 340.0 { 1.30 } else { 1.08 };
    let target_step = 6.15 * w_scale + base_lw * 0.40;
    let usable = rect.width() - 2.0;
    let slot_count = ((usable / target_step) as usize).clamp(MIN_BAR_COUNT, MAX_BAR_COUNT);
    let total_step_w = usable - base_lw;
    let slot_step = if slot_count > 1 {
        total_step_w / (slot_count - 1) as f32
    } else {
        0.0
    };
    let start_x = rect.left() + base_lw / 2.0;
    let line_width = base_lw * 1.40;
    let dot_size = (line_width * 0.95 * h_scale).clamp(1.33, rect.height() / 28.0);
    let dot_threshold = (0.025 * h_scale).clamp(0.020, 0.031);
    let low_line_threshold = dot_threshold + 0.065;
    let min_line_height = dot_size * 1.5;
    let max_line_height = rect.height() * 0.88;

    WaveLayout {
        slot_count,
        start_x,
        slot_step,
        line_width,
        dot_size,
        min_line_height,
        max_line_height,
        dot_threshold,
        low_line_threshold,
    }
}

fn sample_level(levels: &[f32], slot: usize, slot_count: usize) -> f32 {
    if levels.is_empty() || slot_count == 0 {
        return 0.0;
    }
    let t = slot as f32 / slot_count.max(1) as f32 * levels.len() as f32;
    let idx = (t as usize).min(levels.len() - 1);
    let next = (idx + 1).min(levels.len() - 1);
    let frac = t - idx as f32;
    let v = levels[idx] * (1.0 - frac) + levels[next] * frac;
    v.clamp(0.0, 1.0)
}

pub fn draw_waveform(painter: &Painter, rect: Rect, state: &WaveformState, color: Color32) {
    if !state.can_show || state.display_levels.is_empty() {
        return;
    }

    let sr = rect.shrink2(Vec2::new(0.0, 0.5));
    let center_y = sr.center().y;
    let h_scale = (sr.height() / BASELINE_HEIGHT).clamp(0.85, 2.6);
    let layout = build_layout(sr);
    let visual_curve = (0.72 - (h_scale - 1.0) * 0.045).clamp(0.60, 0.74);

    for i in 0..layout.slot_count {
        let x = if layout.slot_count > 1 {
            layout.start_x + i as f32 * layout.slot_step
        } else {
            sr.center().x
        };

        let level = sample_level(&state.display_levels, i, layout.slot_count);
        let visual = level.powf(visual_curve);

        if visual <= layout.dot_threshold {
            let dot_visual = (visual / layout.dot_threshold).clamp(0.0, 1.0);
            let s = layout.dot_size * (0.78 + dot_visual * 0.22);
            painter.circle_filled(Pos2::new(x, center_y), s / 2.0, color);
        } else {
            let norm =
                ((visual - layout.dot_threshold) / (1.0 - layout.dot_threshold)).clamp(0.0, 1.0);
            let mut line_h =
                layout.min_line_height + norm * (layout.max_line_height - layout.min_line_height);

            if visual < layout.low_line_threshold {
                let micro = ((visual - layout.dot_threshold)
                    / (layout.low_line_threshold - layout.dot_threshold))
                    .clamp(0.0, 1.0);
                line_h = layout.min_line_height + micro * (sr.height() * 0.12);
            }

            let bar = Rect::from_center_size(
                Pos2::new(x, center_y),
                Vec2::new(layout.line_width, line_h),
            );
            painter.rect_filled(bar, Rounding::same(layout.line_width / 2.0), color);
        }
    }
}
