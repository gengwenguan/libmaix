use crate::native::NativeEngine;
use anyhow::{anyhow, Context, Result};
use bytes::Bytes;
use serde::Serialize;
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{mpsc as std_mpsc, Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;
use tokio::sync::mpsc;
use webrtc::api::interceptor_registry::register_default_interceptors;
use webrtc::api::media_engine::{MediaEngine, MIME_TYPE_H264, MIME_TYPE_OPUS};
use webrtc::api::setting_engine::SettingEngine;
use webrtc::api::APIBuilder;
use webrtc::ice::mdns::MulticastDnsMode;
use webrtc::ice::network_type::NetworkType;
use webrtc::interceptor::registry::Registry;
use webrtc::media::Sample;
use webrtc::peer_connection::configuration::RTCConfiguration;
use webrtc::peer_connection::peer_connection_state::RTCPeerConnectionState;
use webrtc::peer_connection::sdp::session_description::RTCSessionDescription;
use webrtc::peer_connection::RTCPeerConnection;
use webrtc::rtcp::payload_feedbacks::full_intra_request::FullIntraRequest;
use webrtc::rtcp::payload_feedbacks::picture_loss_indication::PictureLossIndication;
use webrtc::rtp_transceiver::rtp_codec::{
    RTCRtpCodecCapability, RTCRtpCodecParameters, RTPCodecType,
};
use webrtc::rtp_transceiver::RTCPFeedback;
use webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample;
use webrtc::track::track_local::TrackLocal;

const VIDEO_QUEUE: usize = 4;
const AUDIO_QUEUE: usize = 16;
const COMMAND_TIMEOUT: Duration = Duration::from_secs(8);

#[derive(Clone)]
pub struct VideoFrame {
    data: Bytes,
    pts_us: i64,
}

struct MediaSenders {
    video: mpsc::Sender<VideoFrame>,
    audio: mpsc::Sender<Bytes>,
}

#[derive(Default)]
pub struct WebRtcMedia {
    senders: Mutex<Option<MediaSenders>>,
}

impl WebRtcMedia {
    pub fn push_video(&self, data: &[u8], pts_us: i64) {
        let senders = self.senders.lock().unwrap_or_else(|e| e.into_inner());
        let Some(senders) = senders.as_ref() else {
            return;
        };
        let _ = senders.video.try_send(VideoFrame {
            data: Bytes::copy_from_slice(data),
            pts_us,
        });
    }

    pub fn push_audio(&self, data: &[u8]) {
        let senders = self.senders.lock().unwrap_or_else(|e| e.into_inner());
        let Some(senders) = senders.as_ref() else {
            return;
        };
        let _ = senders.audio.try_send(Bytes::copy_from_slice(data));
    }

    fn attach(&self) -> (mpsc::Receiver<VideoFrame>, mpsc::Receiver<Bytes>) {
        let (video_tx, video_rx) = mpsc::channel(VIDEO_QUEUE);
        let (audio_tx, audio_rx) = mpsc::channel(AUDIO_QUEUE);
        *self.senders.lock().unwrap_or_else(|e| e.into_inner()) = Some(MediaSenders {
            video: video_tx,
            audio: audio_tx,
        });
        (video_rx, audio_rx)
    }

    fn detach(&self) {
        self.senders
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .take();
    }
}

enum Command {
    Offer {
        sdp: String,
        response: std_mpsc::SyncSender<Result<String>>,
    },
    Close {
        response: std_mpsc::SyncSender<()>,
    },
    Shutdown,
}

#[derive(Clone, Copy)]
#[repr(u8)]
enum State {
    Idle = 0,
    Negotiating = 1,
    Connecting = 2,
    Connected = 3,
    Disconnected = 4,
    Failed = 5,
}

#[derive(Serialize)]
pub struct WebRtcStatus {
    active: bool,
    state: &'static str,
}

pub struct WebRtcService {
    commands: mpsc::Sender<Command>,
    state: Arc<AtomicU8>,
    active: Arc<AtomicBool>,
    thread: Mutex<Option<JoinHandle<()>>>,
}

impl WebRtcService {
    pub fn start(media: Arc<WebRtcMedia>, engine: Arc<NativeEngine>) -> Result<Self> {
        let (commands, receiver) = mpsc::channel(4);
        let state = Arc::new(AtomicU8::new(State::Idle as u8));
        let active = Arc::new(AtomicBool::new(false));
        let state_for_thread = state.clone();
        let active_for_thread = active.clone();
        let handle = thread::Builder::new()
            .name("webrtc-runtime".to_owned())
            .stack_size(512 * 1024)
            .spawn(move || {
                let runtime = tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build();
                match runtime {
                    Ok(runtime) => runtime.block_on(command_loop(
                        receiver,
                        media,
                        engine,
                        state_for_thread,
                        active_for_thread,
                    )),
                    Err(error) => eprintln!("WebRTC runtime init failed: {error}"),
                }
            })?;
        Ok(Self {
            commands,
            state,
            active,
            thread: Mutex::new(Some(handle)),
        })
    }

    pub fn answer(&self, offer: String) -> Result<String> {
        if offer.len() > 256 * 1024 || !offer.contains("m=video") {
            return Err(anyhow!("invalid WebRTC offer"));
        }
        let (tx, rx) = std_mpsc::sync_channel(1);
        self.commands
            .blocking_send(Command::Offer {
                sdp: offer,
                response: tx,
            })
            .context("WebRTC runtime stopped")?;
        rx.recv_timeout(COMMAND_TIMEOUT)
            .context("WebRTC negotiation timeout")?
    }

    pub fn close(&self) {
        let (tx, rx) = std_mpsc::sync_channel(1);
        if self
            .commands
            .blocking_send(Command::Close { response: tx })
            .is_ok()
        {
            let _ = rx.recv_timeout(Duration::from_secs(3));
        }
    }

    pub fn status(&self) -> WebRtcStatus {
        let state = match self.state.load(Ordering::Acquire) {
            value if value == State::Negotiating as u8 => "negotiating",
            value if value == State::Connecting as u8 => "connecting",
            value if value == State::Connected as u8 => "connected",
            value if value == State::Disconnected as u8 => "disconnected",
            value if value == State::Failed as u8 => "failed",
            _ => "idle",
        };
        WebRtcStatus {
            active: self.active.load(Ordering::Acquire),
            state,
        }
    }
}

impl Drop for WebRtcService {
    fn drop(&mut self) {
        let _ = self.commands.blocking_send(Command::Shutdown);
        if let Some(handle) = self.thread.lock().unwrap_or_else(|e| e.into_inner()).take() {
            let _ = handle.join();
        }
    }
}

struct Session {
    peer: Arc<RTCPeerConnection>,
    tasks: Vec<tokio::task::JoinHandle<()>>,
}

impl Session {
    async fn close(self, media: &WebRtcMedia, engine: &NativeEngine) {
        media.detach();
        engine.set_webrtc_active(false);
        for task in self.tasks {
            task.abort();
        }
        let _ = self.peer.close().await;
    }
}

async fn command_loop(
    mut commands: mpsc::Receiver<Command>,
    media: Arc<WebRtcMedia>,
    engine: Arc<NativeEngine>,
    state: Arc<AtomicU8>,
    active: Arc<AtomicBool>,
) {
    let mut session: Option<Session> = None;
    while let Some(command) = commands.recv().await {
        match command {
            Command::Offer { sdp, response } => {
                if let Some(previous) = session.take() {
                    previous.close(&media, &engine).await;
                }
                active.store(false, Ordering::Release);
                state.store(State::Negotiating as u8, Ordering::Release);
                match create_session(
                    sdp,
                    media.clone(),
                    engine.clone(),
                    state.clone(),
                    active.clone(),
                )
                .await
                {
                    Ok((created, answer)) => {
                        session = Some(created);
                        let _ = response.send(Ok(answer));
                    }
                    Err(error) => {
                        media.detach();
                        engine.set_webrtc_active(false);
                        state.store(State::Failed as u8, Ordering::Release);
                        let _ = response.send(Err(error));
                    }
                }
            }
            Command::Close { response } => {
                if let Some(existing) = session.take() {
                    existing.close(&media, &engine).await;
                }
                active.store(false, Ordering::Release);
                state.store(State::Idle as u8, Ordering::Release);
                let _ = response.send(());
            }
            Command::Shutdown => break,
        }
    }
    if let Some(existing) = session.take() {
        existing.close(&media, &engine).await;
    }
}

async fn create_session(
    offer_sdp: String,
    media: Arc<WebRtcMedia>,
    engine: Arc<NativeEngine>,
    state: Arc<AtomicU8>,
    active: Arc<AtomicBool>,
) -> Result<(Session, String)> {
    let mut media_engine = MediaEngine::default();
    media_engine.register_codec(
        RTCRtpCodecParameters {
            capability: RTCRtpCodecCapability {
                mime_type: MIME_TYPE_OPUS.to_owned(),
                clock_rate: 48_000,
                channels: 2,
                sdp_fmtp_line: "minptime=10;useinbandfec=1".to_owned(),
                ..Default::default()
            },
            payload_type: 111,
            ..Default::default()
        },
        RTPCodecType::Audio,
    )?;
    media_engine.register_codec(
        RTCRtpCodecParameters {
            capability: RTCRtpCodecCapability {
                mime_type: MIME_TYPE_H264.to_owned(),
                clock_rate: 90_000,
                sdp_fmtp_line:
                    "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f"
                        .to_owned(),
                rtcp_feedback: vec![
                    RTCPFeedback {
                        typ: "nack".to_owned(),
                        parameter: String::new(),
                    },
                    RTCPFeedback {
                        typ: "nack".to_owned(),
                        parameter: "pli".to_owned(),
                    },
                    RTCPFeedback {
                        typ: "ccm".to_owned(),
                        parameter: "fir".to_owned(),
                    },
                ],
                ..Default::default()
            },
            payload_type: 117,
            ..Default::default()
        },
        RTPCodecType::Video,
    )?;
    let registry = register_default_interceptors(Registry::new(), &mut media_engine)?;
    let mut settings = SettingEngine::default();
    // The device has a globally routable IPv6 address. Advertising only UDP6
    // guarantees that low-latency mode does not silently fall back to LAN IPv4.
    settings.set_network_types(vec![NetworkType::Udp6]);
    settings.set_ice_multicast_dns_mode(MulticastDnsMode::Disabled);
    let api = APIBuilder::new()
        .with_media_engine(media_engine)
        .with_interceptor_registry(registry)
        .with_setting_engine(settings)
        .build();
    let peer = Arc::new(api.new_peer_connection(RTCConfiguration::default()).await?);

    let video_track = Arc::new(TrackLocalStaticSample::new(
        RTCRtpCodecCapability {
            mime_type: MIME_TYPE_H264.to_owned(),
            ..Default::default()
        },
        "camera-video".to_owned(),
        "camera".to_owned(),
    ));
    let video_sender = peer
        .add_track(video_track.clone() as Arc<dyn TrackLocal + Send + Sync>)
        .await?;
    let audio_track = Arc::new(TrackLocalStaticSample::new(
        RTCRtpCodecCapability {
            mime_type: MIME_TYPE_OPUS.to_owned(),
            ..Default::default()
        },
        "camera-audio".to_owned(),
        "camera".to_owned(),
    ));
    let audio_sender = peer
        .add_track(audio_track.clone() as Arc<dyn TrackLocal + Send + Sync>)
        .await?;

    let state_for_peer = state.clone();
    let active_for_peer = active.clone();
    let media_for_peer = media.clone();
    let engine_for_peer = engine.clone();
    peer.on_peer_connection_state_change(Box::new(move |current| {
        match current {
            RTCPeerConnectionState::Connecting => {
                state_for_peer.store(State::Connecting as u8, Ordering::Release)
            }
            RTCPeerConnectionState::Connected => {
                active_for_peer.store(true, Ordering::Release);
                state_for_peer.store(State::Connected as u8, Ordering::Release);
            }
            RTCPeerConnectionState::Disconnected => {
                state_for_peer.store(State::Disconnected as u8, Ordering::Release)
            }
            RTCPeerConnectionState::Failed | RTCPeerConnectionState::Closed => {
                active_for_peer.store(false, Ordering::Release);
                state_for_peer.store(State::Failed as u8, Ordering::Release);
                media_for_peer.detach();
                engine_for_peer.set_webrtc_active(false);
            }
            _ => {}
        }
        Box::pin(async {})
    }));

    peer.set_remote_description(RTCSessionDescription::offer(offer_sdp)?)
        .await?;
    let answer = peer.create_answer(None).await?;
    let mut gathered = peer.gathering_complete_promise().await;
    peer.set_local_description(answer).await?;
    let _ = tokio::time::timeout(Duration::from_secs(3), gathered.recv()).await;
    let answer_sdp = peer
        .local_description()
        .await
        .context("WebRTC local description missing")?
        .sdp;

    let (mut video_rx, mut audio_rx) = media.attach();
    engine.set_webrtc_active(true);
    state.store(State::Connecting as u8, Ordering::Release);

    let video_task = tokio::spawn(async move {
        let mut previous_pts = None;
        while let Some(frame) = video_rx.recv().await {
            let duration_us = previous_pts
                .map(|previous| frame.pts_us.saturating_sub(previous))
                .filter(|value| (10_000..=200_000).contains(value))
                .unwrap_or(33_333);
            previous_pts = Some(frame.pts_us);
            if video_track
                .write_sample(&Sample {
                    data: frame.data,
                    duration: Duration::from_micros(duration_us as u64),
                    ..Default::default()
                })
                .await
                .is_err()
            {
                break;
            }
        }
    });
    let audio_task = tokio::spawn(async move {
        while let Some(packet) = audio_rx.recv().await {
            if audio_track
                .write_sample(&Sample {
                    data: packet,
                    duration: Duration::from_millis(20),
                    ..Default::default()
                })
                .await
                .is_err()
            {
                break;
            }
        }
    });
    let engine_for_rtcp = engine.clone();
    let video_rtcp_task = tokio::spawn(async move {
        while let Ok((packets, _)) = video_sender.read_rtcp().await {
            if packets.iter().any(|packet| {
                packet.as_any().is::<PictureLossIndication>()
                    || packet.as_any().is::<FullIntraRequest>()
            }) {
                engine_for_rtcp.force_iframe();
            }
        }
    });
    let audio_rtcp_task =
        tokio::spawn(async move { while audio_sender.read_rtcp().await.is_ok() {} });

    Ok((
        Session {
            peer,
            tasks: vec![video_task, audio_task, video_rtcp_task, audio_rtcp_task],
        },
        answer_sdp,
    ))
}
