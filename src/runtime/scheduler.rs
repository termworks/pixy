use std::sync::{Condvar, Mutex};
use std::time::{Duration, Instant};

pub struct Latest<T> {
    state: Mutex<LatestState<T>>,
    changed: Condvar,
}

struct LatestState<T> {
    value: Option<T>,
    closed: bool,
}

impl<T> Default for Latest<T> {
    fn default() -> Self {
        Self {
            state: Mutex::new(LatestState {
                value: None,
                closed: false,
            }),
            changed: Condvar::new(),
        }
    }
}

impl<T> Latest<T> {
    pub fn submit(&self, value: T) {
        self.state.lock().expect("latest value lock").value = Some(value);
        self.changed.notify_one();
    }

    pub fn take(&self) -> Option<T> {
        self.state.lock().expect("latest value lock").value.take()
    }

    pub fn take_wait(&self) -> Option<T> {
        let mut state = self.state.lock().expect("latest value lock");
        while state.value.is_none() && !state.closed {
            state = self.changed.wait(state).expect("latest value wait");
        }
        state.value.take()
    }

    pub fn close(&self) {
        self.state.lock().expect("latest value lock").closed = true;
        self.changed.notify_all();
    }
}

pub struct Scheduler {
    interval: Duration,
    next_allowed: Instant,
}

impl Scheduler {
    pub fn new(fps: u32) -> Self {
        let interval = Duration::from_nanos(1_000_000_000 / u64::from(fps.max(1)));
        Self {
            interval,
            next_allowed: Instant::now() + interval,
        }
    }

    pub fn wait(&mut self) {
        self.wait_until(None, 0);
    }

    pub fn wait_until(&mut self, next_frame_ms: Option<u64>, rendered_at_ms: u64) {
        self.wait_until_capped(next_frame_ms, rendered_at_ms, None);
    }

    pub fn wait_until_capped(
        &mut self,
        next_frame_ms: Option<u64>,
        rendered_at_ms: u64,
        maximum: Option<Duration>,
    ) {
        let now = Instant::now();
        let frame_delay = next_frame_ms
            .and_then(|next| next.checked_sub(rendered_at_ms))
            .map(Duration::from_millis)
            .unwrap_or_default();
        let allowed_delay = self.next_allowed.saturating_duration_since(now);
        let mut delay = allowed_delay.max(frame_delay);
        if let Some(maximum) = maximum {
            delay = delay.min(maximum);
        }
        let target = now.checked_add(delay).unwrap_or(now);
        if !delay.is_zero() {
            std::thread::sleep(delay);
        }
        self.next_allowed = target
            .max(Instant::now())
            .checked_add(self.interval)
            .unwrap_or_else(Instant::now);
    }
}

#[cfg(test)]
mod tests {
    use super::{Latest, Scheduler};
    use std::sync::Arc;
    use std::time::{Duration, Instant};

    #[test]
    fn accepts_zero_fps_safely() {
        let _ = Scheduler::new(0);
    }

    #[test]
    fn latest_queue_coalesces_values() {
        let queue = Latest::default();
        queue.submit(1);
        queue.submit(2);
        assert_eq!(queue.take(), Some(2));
        assert_eq!(queue.take(), None);
        queue.close();
        assert_eq!(queue.take_wait(), None);
    }

    #[test]
    fn slow_consumer_retains_only_the_latest_value() {
        let queue = Arc::new(Latest::default());
        let producer_queue = Arc::clone(&queue);
        let producer = std::thread::spawn(move || {
            for value in 0..10_000 {
                producer_queue.submit(value);
            }
            producer_queue.close();
        });
        std::thread::sleep(Duration::from_millis(10));
        assert_eq!(queue.take_wait(), Some(9_999));
        assert_eq!(queue.take_wait(), None);
        producer.join().expect("producer");
    }

    #[test]
    fn next_frame_deadline_uses_monotonic_waiting() {
        let mut scheduler = Scheduler::new(1_000);
        let started = Instant::now();
        scheduler.wait_until(Some(120), 100);
        assert!(started.elapsed() >= Duration::from_millis(15));
        assert!(started.elapsed() < Duration::from_millis(200));
    }

    #[test]
    fn maximum_wait_caps_a_distant_frame_deadline() {
        let mut scheduler = Scheduler::new(1);
        let started = Instant::now();
        scheduler.wait_until_capped(Some(60_100), 100, Some(Duration::from_millis(10)));
        assert!(started.elapsed() < Duration::from_millis(100));
    }
}
