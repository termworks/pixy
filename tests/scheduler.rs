use pixy::scheduler::{Latest, Scheduler};
use std::time::{Duration, Instant};

#[test]
fn public_scheduler_coalesces_and_caps_waits() {
    let queue = Latest::default();
    queue.submit("old");
    queue.submit("latest");
    assert_eq!(queue.take(), Some("latest"));

    let mut scheduler = Scheduler::new(1);
    let started = Instant::now();
    scheduler.wait_until_capped(Some(60_000), 0, Some(Duration::from_millis(10)));
    assert!(started.elapsed() < Duration::from_millis(100));
}
