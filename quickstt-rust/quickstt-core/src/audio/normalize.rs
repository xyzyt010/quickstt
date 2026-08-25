pub const TARGET_SAMPLE_RATE: u32 = 16_000;
pub const TARGET_CHUNK_FRAMES: usize = 1_280;

pub struct InputNormalizer {
    channels: usize,
    source_rate: u32,
    phase: u32,
    out: Vec<i16>,
}

impl InputNormalizer {
    pub fn new(channels: u16, source_rate: u32) -> Self {
        Self {
            channels: channels.max(1) as usize,
            source_rate: source_rate.max(1),
            phase: 0,
            out: Vec::with_capacity(TARGET_CHUNK_FRAMES * 2),
        }
    }

    pub fn process_f32<F>(&mut self, input: &[f32], emit: F)
    where
        F: FnMut(Vec<i16>),
    {
        self.process_frames(input, emit, |sample| {
            let sample = sample.clamp(-1.0, 1.0);
            (sample * i16::MAX as f32) as i16
        });
    }

    pub fn process_i16<F>(&mut self, input: &[i16], emit: F)
    where
        F: FnMut(Vec<i16>),
    {
        self.process_frames(input, emit, |sample| sample);
    }

    pub fn process_u16<F>(&mut self, input: &[u16], emit: F)
    where
        F: FnMut(Vec<i16>),
    {
        self.process_frames(input, emit, |sample| {
            let centered = sample as i32 - 32_768;
            centered.clamp(i16::MIN as i32, i16::MAX as i32) as i16
        });
    }

    pub fn process_i32<F>(&mut self, input: &[i32], emit: F)
    where
        F: FnMut(Vec<i16>),
    {
        self.process_frames(input, emit, |sample| (sample >> 16) as i16);
    }

    fn process_frames<T, F, E>(&mut self, input: &[T], mut emit: E, convert: F)
    where
        T: Copy,
        F: Fn(T) -> i16,
        E: FnMut(Vec<i16>),
    {
        if input.is_empty() {
            return;
        }

        for frame in input.chunks(self.channels) {
            if frame.is_empty() {
                continue;
            }
            let mono = if frame.len() == 1 {
                convert(frame[0])
            } else {
                let sum = frame
                    .iter()
                    .map(|sample| convert(*sample) as i32)
                    .sum::<i32>();
                (sum / frame.len() as i32).clamp(i16::MIN as i32, i16::MAX as i32) as i16
            };

            self.phase = self.phase.saturating_add(TARGET_SAMPLE_RATE);
            while self.phase >= self.source_rate {
                self.phase -= self.source_rate;
                self.out.push(mono);
                if self.out.len() >= TARGET_CHUNK_FRAMES {
                    let rest = self.out.split_off(TARGET_CHUNK_FRAMES);
                    let chunk = std::mem::replace(&mut self.out, rest);
                    emit(chunk);
                }
            }
        }
    }
}
