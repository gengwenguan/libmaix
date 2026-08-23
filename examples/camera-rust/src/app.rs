use crate::actions::ActionStore;
use crate::auth::WebAuth;
use crate::cleaner::RecordCleaner;
use crate::config::RuntimeConfig;
use crate::hub::{BroadcastHub, LiveHub};
use crate::light::LightController;
use crate::motion::{MotionInput, MotionWorker};
use crate::mqtt::MqttReporter;
use crate::native::{MediaCallbacks, NativeEngine};
use crate::person::{PersonInput, PersonWorker};
use crate::prompt::PromptService;
use crate::recorder::Recorder;
use crate::remote_hub::{HubMediaInput, RemoteHub};
use crate::snapshot::SnapshotService;
use crate::talk::TalkService;
use crate::webrtc::{WebRtcMedia, WebRtcService};
use anyhow::Result;
use std::path::PathBuf;
use std::sync::Arc;

pub struct CallbackState {
    pub live: Arc<LiveHub>,
    pub audio: Arc<BroadcastHub>,
    pub logs: Arc<BroadcastHub>,
    pub recorder: Arc<Recorder>,
    pub motion: Arc<MotionInput>,
    pub snapshot: Arc<SnapshotService>,
    pub person: Arc<PersonInput>,
    pub webrtc: Arc<WebRtcMedia>,
    pub remote_hub: Arc<HubMediaInput>,
}

impl MediaCallbacks for CallbackState {
    fn on_init_segment(&self, data: &[u8]) {
        self.live.set_init_segment(data);
        self.recorder.set_init_segment(data);
    }

    fn on_fragment(&self, data: &[u8]) {
        self.live.broadcast_fragment(data);
        if let Err(error) = self.recorder.write_fragment(data) {
            eprintln!("record fragment failed: {error:#}");
        }
    }

    fn on_audio_adts(&self, data: &[u8], pts_us: i64) {
        self.audio.broadcast(data);
        self.remote_hub.push_aac(data, pts_us);
    }

    fn on_log_line(&self, data: &[u8]) {
        self.logs.broadcast(data);
    }

    fn on_nv21_frame(&self, data: &[u8]) {
        self.snapshot.on_nv21_frame(data);
        self.motion.input_y_plane(data);
    }

    fn on_person_detected(&self, probability: f32) {
        self.person.detected(probability);
    }

    fn on_h264_access_unit(&self, data: &[u8], pts_us: i64, is_key: bool) {
        self.webrtc.push_video(data, pts_us);
        self.remote_hub.push_h264(data, pts_us, is_key);
    }

    fn on_opus_frame(&self, data: &[u8], _pts_us: i64) {
        self.webrtc.push_audio(data);
    }
}

pub struct AppState {
    pub runtime_dir: PathBuf,
    pub web_dir: PathBuf,
    pub snapshot_dir: PathBuf,
    pub engine: Arc<NativeEngine>,
    pub config: Arc<RuntimeConfig>,
    pub recorder: Arc<Recorder>,
    pub snapshot: Arc<SnapshotService>,
    pub prompt: PromptService,
    pub actions: Arc<ActionStore>,
    pub live: Arc<LiveHub>,
    pub audio: Arc<BroadcastHub>,
    pub logs: Arc<BroadcastHub>,
    pub webrtc: Arc<WebRtcService>,
    pub remote_hub: RemoteHub,
    pub web_auth: WebAuth,
    _mqtt: MqttReporter,
    _cleaner: RecordCleaner,
    _light: LightController,
    _motion: MotionWorker,
    _person: PersonWorker,
    talk: TalkService,
}

impl AppState {
    pub fn build(runtime_dir: PathBuf) -> Result<Arc<Self>> {
        let config = Arc::new(RuntimeConfig::load(runtime_dir.join("config.json"))?);
        let recorder = Arc::new(Recorder::new(runtime_dir.join("record"), config.clone())?);
        let actions = Arc::new(
            ActionStore::open(runtime_dir.join("actions.json"))
                .map_err(|error| anyhow::anyhow!("load actions: {error}"))?,
        );
        let live = Arc::new(LiveHub::default());
        let audio = Arc::new(BroadcastHub::default());
        let logs = Arc::new(BroadcastHub::default());
        let snapshot = SnapshotService::new(runtime_dir.join("snapshot"), config.clone())?;
        let (motion_input, motion_receiver) = MotionInput::channel(config.clone());
        let (person_input, person_receiver) = PersonInput::channel();
        let webrtc_media = Arc::new(WebRtcMedia::default());
        let remote_hub_media = HubMediaInput::new(config.clone());
        let callbacks = Arc::new(CallbackState {
            live: live.clone(),
            audio: audio.clone(),
            logs: logs.clone(),
            recorder: recorder.clone(),
            motion: motion_input,
            snapshot: snapshot.clone(),
            person: person_input,
            webrtc: webrtc_media.clone(),
            remote_hub: remote_hub_media.clone(),
        });
        let runtime_text = runtime_dir.to_string_lossy();
        let engine = Arc::new(NativeEngine::new(&runtime_text, callbacks)?);
        engine.set_config(&config.current())?;
        let prompt = PromptService::new(runtime_dir.join("prompt"), engine.clone());
        let talk = TalkService::new(engine.clone());
        let webrtc = Arc::new(WebRtcService::start(webrtc_media, engine.clone())?);
        let remote_hub = RemoteHub::start(
            config.clone(),
            remote_hub_media,
            engine.clone(),
            snapshot.clone(),
        )?;
        let mqtt = MqttReporter::start(config.clone())?;
        let web_auth = WebAuth::from_env();
        let cleaner =
            RecordCleaner::start(runtime_dir.join("record"), config.clone(), recorder.clone())?;
        let light = LightController::start(config.clone(), engine.clone())?;
        let motion = MotionWorker::start(
            motion_receiver,
            config.clone(),
            snapshot.clone(),
            engine.clone(),
        )?;
        let person = PersonWorker::start(
            person_receiver,
            config.clone(),
            snapshot.clone(),
            engine.clone(),
        )?;

        Ok(Arc::new(Self {
            web_dir: runtime_dir.join("web"),
            snapshot_dir: runtime_dir.join("snapshot"),
            runtime_dir,
            engine,
            config,
            recorder,
            snapshot,
            prompt,
            actions,
            live,
            audio,
            logs,
            webrtc,
            remote_hub,
            web_auth,
            _mqtt: mqtt,
            _cleaner: cleaner,
            _light: light,
            _motion: motion,
            _person: person,
            talk,
        }))
    }

    pub fn talk_connected(&self) -> Result<()> {
        self.talk.connect()
    }

    pub fn talk_disconnected(&self) {
        self.talk.disconnect();
    }

    pub fn talk_feed(&self, frame: &[u8]) {
        self.talk.feed(frame);
    }
}
