//! What the address bar's text means: an address, or something to search for.

use url::Url;

/// The search engine for text that is not an address. Its HTML page works without scripts.
const SEARCH: &str = "https://html.duckduckgo.com/html/";

/// True for hosts that are on the machine or the local network: they are reached over http, as
/// they rarely have a certificate.
fn is_local(host: &str) -> bool {
    let host = host.trim_start_matches('[').trim_end_matches(']');
    if host == "localhost" || host.ends_with(".local") || host.ends_with(".localhost") {
        return true;
    }
    if host.parse::<std::net::Ipv6Addr>().is_ok() {
        return true;
    }
    if let Ok(address) = host.parse::<std::net::Ipv4Addr>() {
        return address.is_loopback() || address.is_private() || address.is_link_local();
    }
    false
}

/// Whether `text` looks like a host name, with an optional port and path: one word, with a dot,
/// or `localhost`, or an address.
fn looks_like_host(text: &str) -> bool {
    if text.is_empty() || text.contains(char::is_whitespace) {
        return false;
    }
    let host = text.split(['/', '?', '#']).next().unwrap_or("");
    let host = host.rsplit('@').next().unwrap_or(host);
    let name = if host.starts_with('[') {
        host.split(']').next().unwrap_or(host).trim_start_matches('[')
    } else {
        host.rsplit_once(':')
            .filter(|(_, port)| !port.is_empty() && port.bytes().all(|b| b.is_ascii_digit()))
            .map_or(host, |(name, _)| name)
    };
    !name.is_empty() && (name == "localhost" || name.contains('.') || name.contains(':'))
}

/// The URL for what was typed: an address as it is (without a scheme, https, or http on the
/// local network), or a search.
pub fn resolve(text: &str) -> Option<Url> {
    let text = text.trim();
    if text.is_empty() {
        return None;
    }
    if text.contains("://") || text.starts_with("about:") || text.starts_with("data:") || text.starts_with("file:") {
        return Url::parse(text).ok();
    }
    if looks_like_host(text) {
        let host = text.split(['/', '?', '#']).next().unwrap_or("");
        let host = host.rsplit('@').next().unwrap_or(host);
        let name = host.rsplit_once(':').filter(|(_, p)| p.bytes().all(|b| b.is_ascii_digit())).map_or(host, |(n, _)| n);
        let scheme = if is_local(name) { "http" } else { "https" };
        if let Ok(url) = Url::parse(&format!("{scheme}://{text}")) {
            return Some(url);
        }
    }
    let mut url = Url::parse(SEARCH).ok()?;
    url.query_pairs_mut().append_pair("q", text);
    Some(url)
}

/// The address as shown when it is not being edited: the part before the host, the host, and the
/// rest, so the host can stand out.
pub fn split_for_display(address: &str) -> (&str, &str, &str) {
    let Some((scheme, rest)) = address.split_once("://") else {
        return ("", address, "");
    };
    let prefix_len = scheme.len() + 3;
    let host_end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
    (&address[..prefix_len], &rest[..host_end], &rest[host_end..])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn addresses() {
        assert_eq!(resolve("example.com").unwrap().as_str(), "https://example.com/");
        assert_eq!(resolve("10.0.2.2:8765/a.html").unwrap().as_str(), "http://10.0.2.2:8765/a.html");
        assert_eq!(resolve("localhost:3000").unwrap().as_str(), "http://localhost:3000/");
        assert_eq!(resolve("http://a.b/c").unwrap().as_str(), "http://a.b/c");
        assert!(resolve("rust lang").unwrap().as_str().starts_with(SEARCH));
        assert!(resolve("weather").unwrap().as_str().contains("q=weather"));
        assert!(resolve("   ").is_none());
    }
}
