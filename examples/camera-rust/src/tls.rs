use crate::native::TlsContext;
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::json;
use sha2::{Digest, Sha256};
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};

#[derive(Deserialize, Serialize)]
pub struct CertificateUpdate {
    pub bundle_sha256: String,
    pub chain: String,
    pub key: String,
}

struct Loaded {
    context: Arc<TlsContext>,
    hash: String,
}

pub struct TlsService {
    directory: PathBuf,
    loaded: RwLock<Option<Loaded>>,
}

pub fn bundle_hash(chain: &str, key: &str) -> String {
    let mut digest = Sha256::new();
    digest.update(chain.as_bytes());
    digest.update([0]);
    digest.update(key.as_bytes());
    format!("{:x}", digest.finalize())
}

fn write_private(path: &Path, data: &[u8]) -> Result<()> {
    let temporary = path.with_extension("new");
    let mut file = OpenOptions::new().write(true).create(true).truncate(true)
        .mode(0o600).open(&temporary)?;
    file.write_all(data)?;
    file.sync_all()?;
    fs::rename(&temporary, path)?;
    fs::File::open(path.parent().context("TLS file parent")?)?.sync_all()?;
    Ok(())
}

impl TlsService {
    pub fn load(runtime: &Path) -> Arc<Self> {
        let service = Arc::new(Self {
            directory: runtime.join("state/tls"),
            loaded: RwLock::new(None),
        });
        let result = (|| -> Result<()> {
            fs::create_dir_all(&service.directory)?;
            let cache = service.directory.join("bundle.json");
            if cache.is_file() {
                let bundle: CertificateUpdate = serde_json::from_slice(&fs::read(cache)?)?;
                service.install(&bundle)?;
                return Ok(());
            }
            let managed = service.directory.join("fullchain.pem");
            let (cert, key) = if managed.is_file() {
                (managed, service.directory.join("private.key"))
            } else {
                (runtime.join("cert/server.crt"), runtime.join("cert/server.key"))
            };
            if !cert.is_file() || !key.is_file() {
                return Ok(());
            }
            let chain = fs::read_to_string(cert)?;
            let key = fs::read_to_string(key)?;
            service.install(&CertificateUpdate {
                bundle_sha256: bundle_hash(&chain, &key), chain, key,
            })?;
            Ok(())
        })();
        if let Err(error) = result {
            eprintln!("TLS startup load failed: {error:#}");
        }
        service
    }

    pub fn current(&self) -> Option<Arc<TlsContext>> {
        self.loaded.read().unwrap().as_ref().map(|loaded| loaded.context.clone())
    }

    pub fn hash(&self) -> String {
        self.loaded.read().unwrap().as_ref().map(|loaded| loaded.hash.clone()).unwrap_or_default()
    }

    pub fn acknowledge(&self, update: &CertificateUpdate) -> String {
        let result = self.install(update);
        json!({
            "type": "certificate_ack",
            "bundle_sha256": update.bundle_sha256,
            "ok": result.is_ok(),
            "reloaded": result.as_ref().copied().unwrap_or(false),
            "error": result.err().map(|error| format!("{error:#}")),
        }).to_string()
    }

    /// Called only at startup and from the single certificate worker.
    fn install(&self, update: &CertificateUpdate) -> Result<bool> {
        if update.chain.len() + update.key.len() > 64 * 1024
            || bundle_hash(&update.chain, &update.key) != update.bundle_sha256
        {
            bail!("certificate bundle size/hash mismatch");
        }
        if self.hash() == update.bundle_sha256 {
            return Ok(false);
        }
        // Validate a fresh context before modifying the authoritative cache or live context.
        let stage_cert = self.directory.join("candidate-chain.pem");
        let stage_key = self.directory.join("candidate-key.pem");
        let candidate = (|| -> Result<TlsContext> {
            write_private(&stage_cert, update.chain.as_bytes())?;
            write_private(&stage_key, update.key.as_bytes())?;
            TlsContext::new(&stage_cert.to_string_lossy(), &stage_key.to_string_lossy())
                .context("validate certificate and private key")
        })();
        let _ = fs::remove_file(stage_cert);
        let _ = fs::remove_file(stage_key);
        let candidate = Arc::new(candidate?);
        // One authoritative bundle recovers a crash between the two PEM replacements.
        write_private(&self.directory.join("bundle.json"), &serde_json::to_vec(update)?)?;
        write_private(&self.directory.join("private.key"), update.key.as_bytes())?;
        write_private(&self.directory.join("fullchain.pem"), update.chain.as_bytes())?;
        *self.loaded.write().unwrap() = Some(Loaded {
            context: candidate, hash: update.bundle_sha256.clone(),
        });
        eprintln!("TLS certificate hot loaded: {}", update.bundle_sha256);
        Ok(true)
    }
}
