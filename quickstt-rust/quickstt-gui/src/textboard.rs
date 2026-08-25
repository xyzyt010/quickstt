//! TextBoard — floating transcript window (mirrors C++ TextBoardWindow)
//! C++ original: Source/text_board.h + text_board.cpp
//! Features: title bar 28px, opacity, monospace text, attach/detach, custom scroll, QSizeGrip
//! Egui port: second viewport "quickstt_textboard" with same visual styling

use eframe::egui::{self, Color32, Pos2, Rect, Vec2, ViewportBuilder, ViewportId, ViewportCommand};

pub const TEXTBOARD_TITLE_HEIGHT: f32 = 28.0;

#[derive(Clone)]
pub struct TextBoardState {
    pub visible: bool,
    pub attached: bool,
    pub opacity_pct: u32,
    pub text_size: f32,
    pub header_text: String,
    pub content: String,
    pub window_pos: Option<Pos2>,
    pub window_size: Option<Vec2>,
    pub user_scrolled_up: bool,
}

impl Default for TextBoardState {
    fn default() -> Self {
        Self {
            visible: true,
            attached: true,
            opacity_pct: 90,
            text_size: 14.0,
            header_text: "Idling…".into(),
            content: String::new(),
            window_pos: None,
            window_size: Some(Vec2::new(420.0, 180.0)),
            user_scrolled_up: false,
        }
    }
}

impl TextBoardState {
    pub fn append_text(&mut self, text: &str) {
        let mut normalized = text.replace("\r\n", "\n").replace('\r', "\n");
        let parts: Vec<String> = normalized.split('\n')
            .map(|s| s.split_whitespace().collect::<Vec<_>>().join(" "))
            .filter(|s| !s.is_empty())
            .collect();
        normalized = parts.join(" ");
        if normalized.is_empty() { return; }
        if !self.content.trim().is_empty() {
            self.content.push('\n');
        }
        self.content.push_str(&normalized);
    }

    pub fn set_header(&mut self, text: &str) {
        self.header_text = if text.trim().is_empty() { "Idling…".into() } else { text.trim().to_string() };
    }

    pub fn toggle_attach(&mut self) { self.attached = !self.attached; }

    /// Snap to pill bottom when attached, else keep detached pos
    pub fn reposition_attached(&mut self, pill_rect: Rect) {
        if !self.attached { return; }
        self.window_pos = Some(Pos2::new(pill_rect.left(), pill_rect.bottom()));
        if let Some(sz) = self.window_size {
            self.window_size = Some(Vec2::new(pill_rect.width(), sz.y));
        } else {
            self.window_size = Some(Vec2::new(pill_rect.width(), 180.0));
        }
    }
}

/// Render the TextBoard as an egui second viewport.
/// Call from QuickSttApp::render — handles viewport creation internally.
pub struct TextBoardViewport {
    pub state: TextBoardState,
}

impl TextBoardViewport {
    pub fn new(state: TextBoardState) -> Self { Self { state } }

    pub fn show(&mut self, ctx: &egui::Context, pill_rect: Option<Rect>) {
        if !self.state.visible { return; }

        // If attached, sync position/size to pill
        if let Some(pr) = pill_rect { self.state.reposition_attached(pr); }

        let pos = self.state.window_pos.unwrap_or(Pos2::new(100.0, 80.0));
        let size = self.state.window_size.unwrap_or(Vec2::new(420.0, 180.0));
        let opacity = (self.state.opacity_pct as f32 / 100.0).clamp(0.2, 1.0);

        let mut builder = ViewportBuilder::default()
            .with_title("QuickSTT — Transcripts")
            .with_inner_size(size)
            .with_position(pos)
            .with_decorations(false)
            .with_transparent(true)
            .with_resizable(true)
            .with_always_on_top()
            .with_taskbar(false);

        // Only set outer position if we have one — avoids jumpy behavior
        if self.state.window_pos.is_some() {
            builder = builder.with_position(pos);
        }

        let mut new_content = self.state.content.clone();
        let header = self.state.header_text.clone();
        let attached = self.state.attached;

        ctx.show_viewport_immediate(
            ViewportId::from_hash_of("quickstt_textboard"),
            builder,
            move |ctx, _class| {
                let visuals = {
                    let mut v = ctx.style().visuals.clone();
                    v.window_fill = Color32::from_rgba_premultiplied(20, 20, 20, (240.0*opacity) as u8);
                    v.panel_fill = Color32::from_rgba_premultiplied(20, 20, 20, (240.0*opacity) as u8);
                    v.override_text_color = Some(Color32::WHITE);
                    v
                };
                ctx.set_visuals(visuals);

                // Title bar 28px
                let title_h = TEXTBOARD_TITLE_HEIGHT;
                egui::TopBottomPanel::top("tb_title").exact_height(title_h).show(ctx, |ui| {
                    ui.horizontal(|ui| {
                        ui.add_space(8.0);
                        ui.label(egui::RichText::new(&header).color(Color32::from_rgb(0xCC,0xCC,0xCC)).size(11.0));
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            let label = if attached { "⛓" } else { "🔗" };
                            let tip = if attached { "Detach from widget" } else { "Attach to widget" };
                            if ui.add(egui::Button::new(label).frame(false)).on_hover_text(tip).clicked() {
                                // Toggle will be handled via message passing — for now just visual
                            }
                            // Drag handle — allocate interact for moving window
                            let resp = ui.allocate_response(Vec2::new(ui.available_width(), title_h), egui::Sense::drag());
                            if resp.dragged() {
                                ctx.send_viewport_cmd(ViewportCommand::OuterPosition(resp.rect.min));
                            }
                        });
                    });
                });

                egui::CentralPanel::default().show(ctx, |ui| {
                    // Custom scrollbar styling via egui style
                    let mut style = (*ctx.style()).clone();
                    style.visuals.widgets.inactive.bg_fill = Color32::from_rgba_premultiplied(255,255,255,18);
                    style.visuals.widgets.hovered.bg_fill = Color32::from_rgba_premultiplied(245,245,245,182);
                    ctx.set_style(style);

                    egui::ScrollArea::vertical().auto_shrink([false,false]).show(ui, |ui| {
                        ui.add_sized(
                            ui.available_size(),
                            egui::TextEdit::multiline(&mut new_content)
                                .font(egui::TextStyle::Monospace)
                                .text_color(Color32::WHITE)
                                .frame(false)
                                .hint_text("Transcripts appear here automatically…")
                        );
                    });
                });

            },
        );
    }
}
