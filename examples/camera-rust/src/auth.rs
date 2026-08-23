use sha1::{Digest, Sha1};
use std::fs::File;
use std::io::Read;
use std::time::{SystemTime, UNIX_EPOCH};

pub const COOKIE_NAME: &str = "camera_session";

pub struct WebAuth {
    username: String,
    password: String,
    token: String,
}

impl WebAuth {
    pub fn from_env() -> Self {
        let username = std::env::var("CAMERA_WEB_USERNAME").unwrap_or_else(|_| "admin".to_owned());
        let password = std::env::var("CAMERA_WEB_PASSWORD").unwrap_or_else(|_| "12345".to_owned());
        Self {
            username,
            password,
            token: session_token(),
        }
    }

    pub fn verify(&self, username: &str, password: &str) -> bool {
        constant_time_eq(username.as_bytes(), self.username.as_bytes())
            & constant_time_eq(password.as_bytes(), self.password.as_bytes())
    }

    pub fn authorized_cookie(&self, cookie: Option<&str>) -> bool {
        cookie
            .and_then(|header| cookie_value(header, COOKIE_NAME))
            .is_some_and(|value| constant_time_eq(value.as_bytes(), self.token.as_bytes()))
    }

    pub fn set_cookie(&self, secure: bool) -> String {
        format!(
            "{COOKIE_NAME}={}; Path=/; Max-Age=604800; HttpOnly; SameSite=Strict{}",
            self.token,
            if secure { "; Secure" } else { "" }
        )
    }

    pub fn clear_cookie(secure: bool) -> String {
        format!(
            "{COOKIE_NAME}=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict{}",
            if secure { "; Secure" } else { "" }
        )
    }
}

fn cookie_value<'a>(header: &'a str, name: &str) -> Option<&'a str> {
    header.split(';').find_map(|part| {
        let (key, value) = part.trim().split_once('=')?;
        (key == name).then_some(value)
    })
}

fn session_token() -> String {
    let mut random = [0u8; 32];
    if File::open("/dev/urandom")
        .and_then(|mut file| file.read_exact(&mut random))
        .is_err()
    {
        let fallback = format!(
            "{}:{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|value| value.as_nanos())
                .unwrap_or_default()
        );
        random[..20].copy_from_slice(&Sha1::digest(fallback.as_bytes()));
    }
    random.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn constant_time_eq(left: &[u8], right: &[u8]) -> bool {
    let mut difference = left.len() ^ right.len();
    let length = left.len().max(right.len());
    for index in 0..length {
        difference |= usize::from(
            left.get(index).copied().unwrap_or_default()
                ^ right.get(index).copied().unwrap_or_default(),
        );
    }
    difference == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_exact_cookie_name() {
        assert_eq!(
            cookie_value("theme=dark; camera_session=secret; x=1", COOKIE_NAME),
            Some("secret")
        );
        assert_eq!(
            cookie_value("other_camera_session=secret", COOKIE_NAME),
            None
        );
    }

    #[test]
    fn verifies_credentials_and_session_cookie() {
        let auth = WebAuth {
            username: "admin".to_owned(),
            password: "12345".to_owned(),
            token: "token".to_owned(),
        };
        assert!(auth.verify("admin", "12345"));
        assert!(!auth.verify("admin", "wrong"));
        assert!(auth.authorized_cookie(Some("camera_session=token")));
        assert!(!auth.authorized_cookie(Some("camera_session=wrong")));
    }
}
