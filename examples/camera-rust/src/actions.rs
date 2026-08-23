use serde::{Deserialize, Serialize};
use std::fmt;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const MAX_ACTIONS: usize = 32;
const MAX_NAME_LEN: usize = 32;
const MAX_URL_LEN: usize = 512;
const HTTP_TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Action {
    pub id: String,
    pub name: String,
    pub url: String,
}

struct StoreState {
    actions: Vec<Action>,
    sequence: u64,
}

pub struct ActionStore {
    path: PathBuf,
    state: Mutex<StoreState>,
}

#[derive(Debug)]
pub enum ActionError {
    BadName,
    BadUrl,
    TooMany,
    NotFound,
    Io(std::io::Error),
}

impl ActionError {
    pub fn http_status(&self) -> u16 {
        match self {
            Self::BadName | Self::BadUrl => 400,
            Self::TooMany => 409,
            Self::NotFound => 404,
            Self::Io(_) => 500,
        }
    }
}

impl fmt::Display for ActionError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::BadName => formatter.write_str("bad name"),
            Self::BadUrl => formatter.write_str("bad url"),
            Self::TooMany => formatter.write_str("too many actions"),
            Self::NotFound => formatter.write_str("not found"),
            Self::Io(error) => write!(formatter, "{error}"),
        }
    }
}

impl From<std::io::Error> for ActionError {
    fn from(error: std::io::Error) -> Self {
        Self::Io(error)
    }
}

impl ActionStore {
    pub fn open(path: PathBuf) -> Result<Self, ActionError> {
        let actions = load_actions(&path)?;
        Ok(Self {
            path,
            state: Mutex::new(StoreState {
                actions,
                sequence: 0,
            }),
        })
    }

    pub fn list(&self) -> Vec<Action> {
        self.state
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .actions
            .clone()
    }

    pub fn get(&self, id: &str) -> Result<Action, ActionError> {
        self.state
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .actions
            .iter()
            .find(|action| action.id == id)
            .cloned()
            .ok_or(ActionError::NotFound)
    }

    pub fn add(&self, name: &str, url: &str) -> Result<Action, ActionError> {
        let (name, url) = validate(name, url)?;
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        if state.actions.len() >= MAX_ACTIONS {
            return Err(ActionError::TooMany);
        }
        state.sequence = state.sequence.saturating_add(1);
        let epoch = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let action = Action {
            id: format!("a{epoch}_{}", state.sequence),
            name,
            url,
        };
        state.actions.push(action.clone());
        save_actions(&self.path, &state.actions)?;
        Ok(action)
    }

    pub fn update(&self, id: &str, name: &str, url: &str) -> Result<Action, ActionError> {
        let (name, url) = validate(name, url)?;
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        let action = state
            .actions
            .iter_mut()
            .find(|action| action.id == id)
            .ok_or(ActionError::NotFound)?;
        action.name = name;
        action.url = url;
        let updated = action.clone();
        save_actions(&self.path, &state.actions)?;
        Ok(updated)
    }

    pub fn remove(&self, id: &str) -> Result<(), ActionError> {
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        let index = state
            .actions
            .iter()
            .position(|action| action.id == id)
            .ok_or(ActionError::NotFound)?;
        state.actions.remove(index);
        save_actions(&self.path, &state.actions)?;
        Ok(())
    }

    pub fn invoke(&self, id: &str) -> Result<(Action, u16), InvokeError> {
        let action = self.get(id).map_err(InvokeError::Action)?;
        let status =
            post_empty(&action.url).map_err(|error| InvokeError::Network(action.clone(), error))?;
        Ok((action, status))
    }
}

#[derive(Debug)]
pub enum InvokeError {
    Action(ActionError),
    Network(Action, String),
}

fn validate(name: &str, url: &str) -> Result<(String, String), ActionError> {
    let name = name.trim();
    let url = url.trim();
    if name.is_empty() || name.len() > MAX_NAME_LEN || name.chars().any(char::is_control) {
        return Err(ActionError::BadName);
    }
    if !valid_http_url(url) {
        return Err(ActionError::BadUrl);
    }
    Ok((name.to_owned(), url.to_owned()))
}

fn valid_http_url(url: &str) -> bool {
    url.len() >= 8
        && url.len() <= MAX_URL_LEN
        && url
            .get(..7)
            .is_some_and(|scheme| scheme.eq_ignore_ascii_case("http://"))
        && !url[7..].is_empty()
        && !url.chars().any(char::is_control)
}

fn load_actions(path: &Path) -> Result<Vec<Action>, ActionError> {
    let text = match fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(error) => return Err(error.into()),
    };
    let actions = serde_json::from_str::<Vec<Action>>(&text).unwrap_or_default();
    Ok(actions
        .into_iter()
        .filter(|action| validate(&action.name, &action.url).is_ok())
        .take(MAX_ACTIONS)
        .collect())
}

fn save_actions(path: &Path, actions: &[Action]) -> Result<(), ActionError> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary = path.with_extension("json.tmp");
    let mut file = File::create(&temporary)?;
    serde_json::to_writer_pretty(&mut file, actions)
        .map_err(|error| ActionError::Io(std::io::Error::other(error)))?;
    file.write_all(b"\n")?;
    file.sync_all()?;
    fs::rename(temporary, path)?;
    Ok(())
}

struct ParsedUrl {
    host: String,
    port: u16,
    path: String,
}

fn parse_url(url: &str) -> Result<ParsedUrl, String> {
    if !valid_http_url(url) {
        return Err("bad url (only http:// supported)".to_owned());
    }
    let remainder = &url[7..];
    let (authority, path) = remainder
        .split_once('/')
        .map_or((remainder, "/".to_owned()), |(authority, path)| {
            (authority, format!("/{path}"))
        });
    if authority.is_empty() {
        return Err("bad url (missing host)".to_owned());
    }

    let (host, port) = if let Some(ipv6) = authority.strip_prefix('[') {
        let end = ipv6.find(']').ok_or_else(|| "bad IPv6 URL".to_owned())?;
        let host = &ipv6[..end];
        let suffix = &ipv6[end + 1..];
        let port = if suffix.is_empty() {
            80
        } else {
            suffix
                .strip_prefix(':')
                .ok_or_else(|| "bad IPv6 port".to_owned())?
                .parse::<u16>()
                .map_err(|_| "bad port".to_owned())?
        };
        (host.to_owned(), port)
    } else if let Some((host, port)) = authority.rsplit_once(':') {
        if host.contains(':') {
            return Err("IPv6 address must use brackets".to_owned());
        }
        (
            host.to_owned(),
            port.parse::<u16>().map_err(|_| "bad port".to_owned())?,
        )
    } else {
        (authority.to_owned(), 80)
    };
    if host.is_empty() {
        return Err("bad url (missing host)".to_owned());
    }
    Ok(ParsedUrl { host, port, path })
}

fn post_empty(url: &str) -> Result<u16, String> {
    let parsed = parse_url(url)?;
    let addresses = (parsed.host.as_str(), parsed.port)
        .to_socket_addrs()
        .map_err(|_| "dns failed".to_owned())?;
    let mut stream = addresses
        .filter_map(|address| TcpStream::connect_timeout(&address, HTTP_TIMEOUT).ok())
        .next()
        .ok_or_else(|| "connect failed".to_owned())?;
    stream
        .set_read_timeout(Some(HTTP_TIMEOUT))
        .map_err(|_| "set timeout failed".to_owned())?;
    stream
        .set_write_timeout(Some(HTTP_TIMEOUT))
        .map_err(|_| "set timeout failed".to_owned())?;

    let host = if parsed.host.contains(':') {
        format!("[{}]", parsed.host)
    } else {
        parsed.host
    };
    let host = if parsed.port == 80 {
        host
    } else {
        format!("{host}:{}", parsed.port)
    };
    let request = format!(
        "POST {} HTTP/1.0\r\n\
         Host: {host}\r\n\
         User-Agent: v831cam-rust-actionproxy\r\n\
         Connection: close\r\n\
         Content-Length: 0\r\n\r\n",
        parsed.path
    );
    stream
        .write_all(request.as_bytes())
        .map_err(|_| "send failed".to_owned())?;

    let mut response = [0u8; 512];
    let length = stream
        .read(&mut response)
        .map_err(|_| "no response".to_owned())?;
    let line = std::str::from_utf8(&response[..length])
        .ok()
        .and_then(|response| response.lines().next())
        .ok_or_else(|| "bad response".to_owned())?;
    let status = line
        .split_whitespace()
        .nth(1)
        .and_then(|status| status.parse::<u16>().ok())
        .filter(|status| (100..=599).contains(status))
        .ok_or_else(|| "bad response".to_owned())?;
    Ok(status)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_ipv4_domain_and_ipv6_urls() {
        let domain = parse_url("http://example.com/path").unwrap();
        assert_eq!(domain.host, "example.com");
        assert_eq!(domain.port, 80);
        assert_eq!(domain.path, "/path");

        let ipv4 = parse_url("http://192.168.1.2:8080/door").unwrap();
        assert_eq!(ipv4.host, "192.168.1.2");
        assert_eq!(ipv4.port, 8080);

        let ipv6 = parse_url("http://[2409::1]:8000/open").unwrap();
        assert_eq!(ipv6.host, "2409::1");
        assert_eq!(ipv6.port, 8000);
    }

    #[test]
    fn validates_action_fields() {
        assert!(validate("开门", "http://192.168.1.2/open").is_ok());
        assert!(validate("", "http://192.168.1.2/open").is_err());
        assert!(validate("bad\nname", "http://192.168.1.2/open").is_err());
        assert!(validate("开门", "https://192.168.1.2/open").is_err());
    }
}
