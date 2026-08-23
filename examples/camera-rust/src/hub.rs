use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex, RwLock, Weak};
use std::time::Duration;

const MAX_QUEUED_ITEMS: usize = 8;
const MAX_QUEUED_BYTES: usize = 1024 * 1024;

#[derive(Default)]
struct QueueState {
    closed: bool,
    bytes: usize,
    items: VecDeque<Arc<[u8]>>,
}

struct ClientQueue {
    state: Mutex<QueueState>,
    ready: Condvar,
}

impl ClientQueue {
    fn new() -> Self {
        Self {
            state: Mutex::new(QueueState::default()),
            ready: Condvar::new(),
        }
    }

    fn push(&self, item: Arc<[u8]>) {
        if item.is_empty() || item.len() > MAX_QUEUED_BYTES {
            return;
        }
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        if state.closed {
            return;
        }
        while !state.items.is_empty()
            && (state.items.len() >= MAX_QUEUED_ITEMS
                || state.bytes.saturating_add(item.len()) > MAX_QUEUED_BYTES)
        {
            if let Some(old) = state.items.pop_front() {
                state.bytes = state.bytes.saturating_sub(old.len());
            }
        }
        state.bytes += item.len();
        state.items.push_back(item);
        self.ready.notify_one();
    }

    fn pop_timeout(&self, timeout: Duration) -> Option<Arc<[u8]>> {
        let state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        let (mut state, _) = self
            .ready
            .wait_timeout_while(state, timeout, |state| {
                !state.closed && state.items.is_empty()
            })
            .unwrap_or_else(|error| error.into_inner());
        let item = state.items.pop_front();
        if let Some(item) = &item {
            state.bytes = state.bytes.saturating_sub(item.len());
        }
        item
    }

    fn close(&self) {
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        state.closed = true;
        state.items.clear();
        state.bytes = 0;
        self.ready.notify_all();
    }
}

#[derive(Default)]
pub struct BroadcastHub {
    clients: Mutex<Vec<Weak<ClientQueue>>>,
}

impl BroadcastHub {
    pub fn subscribe(&self) -> Subscription {
        self.subscribe_with_initial(None)
    }

    fn subscribe_with_initial(&self, initial: Option<Arc<[u8]>>) -> Subscription {
        let queue = Arc::new(ClientQueue::new());
        if let Some(initial) = initial {
            queue.push(initial);
        }
        let mut clients = self
            .clients
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        clients.retain(|client| client.strong_count() > 0);
        clients.push(Arc::downgrade(&queue));
        Subscription { queue }
    }

    pub fn broadcast(&self, data: &[u8]) {
        if data.is_empty() {
            return;
        }
        let item = Arc::<[u8]>::from(data);
        let mut clients = self
            .clients
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        clients.retain(|client| {
            if let Some(client) = client.upgrade() {
                client.push(item.clone());
                true
            } else {
                false
            }
        });
    }

    pub fn subscriber_count(&self) -> usize {
        let mut clients = self
            .clients
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        clients.retain(|client| client.strong_count() > 0);
        clients.len()
    }
}

#[derive(Default)]
pub struct LiveHub {
    init: RwLock<Option<Arc<[u8]>>>,
    broadcast: BroadcastHub,
}

impl LiveHub {
    pub fn set_init_segment(&self, data: &[u8]) {
        let item = Arc::<[u8]>::from(data);
        *self.init.write().unwrap_or_else(|error| error.into_inner()) = Some(item.clone());
        self.broadcast.broadcast(&item);
    }

    pub fn broadcast_fragment(&self, data: &[u8]) {
        self.broadcast.broadcast(data);
    }

    pub fn subscribe(&self) -> Subscription {
        let initial = self
            .init
            .read()
            .unwrap_or_else(|error| error.into_inner())
            .clone();
        self.broadcast.subscribe_with_initial(initial)
    }

    pub fn subscriber_count(&self) -> usize {
        self.broadcast.subscriber_count()
    }
}

pub struct Subscription {
    queue: Arc<ClientQueue>,
}

impl Subscription {
    pub fn recv_timeout(&self, timeout: Duration) -> Option<Arc<[u8]>> {
        self.queue.pop_timeout(timeout)
    }
}

impl Drop for Subscription {
    fn drop(&mut self) {
        self.queue.close();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn live_subscriber_receives_init_before_fragment() {
        let hub = LiveHub::default();
        hub.set_init_segment(b"init");
        let subscription = hub.subscribe();
        hub.broadcast_fragment(b"fragment");

        assert_eq!(
            subscription.recv_timeout(Duration::ZERO).unwrap().as_ref(),
            b"init"
        );
        assert_eq!(
            subscription.recv_timeout(Duration::ZERO).unwrap().as_ref(),
            b"fragment"
        );
    }

    #[test]
    fn slow_subscriber_is_bounded() {
        let hub = BroadcastHub::default();
        let subscription = hub.subscribe();
        for value in 0..32u8 {
            hub.broadcast(&[value]);
        }
        let mut values = Vec::new();
        while let Some(value) = subscription.recv_timeout(Duration::ZERO) {
            values.push(value[0]);
        }
        assert_eq!(values, (24..32).collect::<Vec<_>>());
    }
}
