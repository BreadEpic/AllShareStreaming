//! Wake-on-LAN
//!
//! A single magic packet to `255.255.255.255:9` often never reaches a sleeping pc:
//! the limited broadcast only leaves through one interface, docker bridges swallow it
//! and routers drop it. This sends the packet to every address the host might be
//! reachable on (like the official Moonlight clients do) and repeats it a few times.

use std::{
    collections::HashSet,
    io,
    net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr},
    str::FromStr,
    time::Duration,
};

use moonlight_common::mac::MacAddress;
use tokio::{
    net::{UdpSocket, lookup_host},
    time::sleep,
};
use tracing::{debug, info, warn};

/// Standard WoL ports plus the port opened by the Moonlight Internet Hosting Tool.
const STATIC_WAKE_PORTS: [u16; 3] = [9, 7, 47009];
/// Ports opened by Sunshine / GFE relative to the http port.
/// Useful when the router forwards those ports to the host.
const DYNAMIC_WAKE_PORT_OFFSETS: [u16; 5] = [9, 10, 11, 13, 21];

const WAKE_ROUNDS: usize = 3;
const WAKE_ROUND_DELAY: Duration = Duration::from_millis(300);

pub fn magic_packet(mac: MacAddress) -> [u8; 102] {
    let mut packet = [0xFFu8; 102];

    let mac = mac.to_bytes();
    for chunk in packet[6..].chunks_exact_mut(6) {
        chunk.copy_from_slice(&mac);
    }

    packet
}

/// Parses mac addresses like `AA:BB:CC:DD:EE:FF`, `aa-bb-cc-dd-ee-ff`, `aabb.ccdd.eeff` or `aabbccddeeff`.
pub fn parse_mac(input: &str) -> Option<MacAddress> {
    let hex = input
        .trim()
        .chars()
        .filter(|c| !matches!(c, ':' | '-' | '.' | ' '))
        .collect::<String>();

    if hex.len() != 12 {
        return None;
    }

    let mut bytes = [0u8; 6];
    hex::decode_to_slice(hex, &mut bytes).ok()?;

    let mac = MacAddress::from_bytes(bytes);
    if mac == MacAddress::NULL || mac == MacAddress::from_bytes([0xFF; 6]) {
        return None;
    }

    Some(mac)
}

/// Sunshine reports `00:00:00:00:00:00` when it doesn't want to share the mac.
pub fn usable_mac(mac: Option<MacAddress>) -> Option<MacAddress> {
    mac.filter(|mac| *mac != MacAddress::NULL)
}

fn is_private_v4(ip: Ipv4Addr) -> bool {
    ip.is_private() || ip.is_link_local()
}

/// The broadcast of the /24 network the ip is in. Most home networks are a /24.
fn assumed_broadcast_v4(ip: Ipv4Addr) -> Ipv4Addr {
    let [a, b, c, _] = ip.octets();
    Ipv4Addr::new(a, b, c, 255)
}

fn ports_for(http_port: u16) -> Vec<u16> {
    let mut ports = STATIC_WAKE_PORTS.to_vec();

    for offset in DYNAMIC_WAKE_PORT_OFFSETS {
        if let Some(port) = http_port.checked_add(offset)
            && !ports.contains(&port)
        {
            ports.push(port);
        }
    }

    ports
}

async fn resolve(address: &str) -> Vec<IpAddr> {
    if let Ok(ip) = IpAddr::from_str(address) {
        return vec![ip];
    }
    // Allow ipv6 addresses in brackets
    if let Some(ip) = address
        .strip_prefix('[')
        .and_then(|address| address.strip_suffix(']'))
        .and_then(|address| Ipv6Addr::from_str(address).ok())
    {
        return vec![IpAddr::V6(ip)];
    }

    match lookup_host((address, 0)).await {
        Ok(addresses) => addresses.map(|address| address.ip()).collect(),
        Err(err) => {
            debug!("Wake-on-LAN: couldn't resolve {address}: {err}");
            Vec::new()
        }
    }
}

/// Collects all addresses a magic packet should be sent to.
pub async fn wake_targets(
    host_address: &str,
    http_port: u16,
    extra_addresses: &[String],
) -> Vec<SocketAddr> {
    let default_ports = ports_for(http_port);

    let mut ips = Vec::<IpAddr>::new();
    let mut explicit = Vec::<SocketAddr>::new();

    // Limited broadcast
    ips.push(IpAddr::V4(Ipv4Addr::BROADCAST));

    // Directed broadcasts of every network this server is in
    match if_addrs::get_if_addrs() {
        Ok(interfaces) => {
            for interface in interfaces {
                if interface.is_loopback() {
                    continue;
                }
                if let if_addrs::IfAddr::V4(addr) = interface.addr
                    && let Some(broadcast) = addr.broadcast
                {
                    ips.push(IpAddr::V4(broadcast));
                }
            }
        }
        Err(err) => warn!("Wake-on-LAN: failed to list network interfaces: {err}"),
    }

    // The host itself: works while its ARP entry is still cached or when a router forwards the ports
    for ip in resolve(host_address).await {
        ips.push(ip);

        if let IpAddr::V4(ip) = ip
            && is_private_v4(ip)
        {
            // Works even when we're behind a NAT like a docker bridge network
            ips.push(IpAddr::V4(assumed_broadcast_v4(ip)));
        }
    }

    // User configured addresses, either "ip" or "ip:port"
    for address in extra_addresses {
        if let Ok(address) = SocketAddr::from_str(address) {
            explicit.push(address);
        } else {
            ips.extend(resolve(address).await);
        }
    }

    let mut seen = HashSet::new();
    let mut targets = Vec::new();

    for ip in ips {
        for port in &default_ports {
            let target = SocketAddr::new(ip, *port);
            if seen.insert(target) {
                targets.push(target);
            }
        }
    }
    for target in explicit {
        if seen.insert(target) {
            targets.push(target);
        }
    }

    targets
}

/// Sends the magic packet to all targets a few times.
/// Only fails if not a single packet could be sent.
pub async fn send_magic_packets(mac: MacAddress, targets: &[SocketAddr]) -> io::Result<()> {
    let packet = magic_packet(mac);

    let socket_v4 = match UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).await {
        Ok(socket) => {
            socket.set_broadcast(true)?;
            Some(socket)
        }
        Err(err) => {
            warn!("Wake-on-LAN: failed to bind ipv4 socket: {err}");
            None
        }
    };
    let socket_v6 = if targets.iter().any(SocketAddr::is_ipv6) {
        UdpSocket::bind((Ipv6Addr::UNSPECIFIED, 0)).await.ok()
    } else {
        None
    };

    let mut sent = 0usize;
    let mut last_error = None;

    for round in 0..WAKE_ROUNDS {
        if round > 0 {
            sleep(WAKE_ROUND_DELAY).await;
        }

        for target in targets {
            let socket = match target {
                SocketAddr::V4(_) => socket_v4.as_ref(),
                SocketAddr::V6(_) => socket_v6.as_ref(),
            };
            let Some(socket) = socket else {
                continue;
            };

            match socket.send_to(&packet, target).await {
                Ok(_) => sent += 1,
                Err(err) => {
                    debug!("Wake-on-LAN: failed to send to {target}: {err}");
                    last_error = Some(err);
                }
            }
        }
    }

    if sent == 0 {
        return Err(last_error.unwrap_or_else(|| {
            io::Error::new(
                io::ErrorKind::AddrNotAvailable,
                "no address to send the wake on lan packet to",
            )
        }));
    }

    info!(
        "Wake-on-LAN: sent {sent} magic packets for {mac} to {} targets",
        targets.len()
    );

    Ok(())
}

#[cfg(test)]
mod test {
    use std::net::{IpAddr, Ipv4Addr, SocketAddr};

    use moonlight_common::mac::MacAddress;

    use crate::app::wake::{magic_packet, parse_mac, usable_mac, wake_targets};

    const MAC: MacAddress = MacAddress::NULL;

    #[test]
    fn packet_layout() {
        let mac = MacAddress::from_bytes([1, 2, 3, 4, 5, 6]);
        let packet = magic_packet(mac);

        assert_eq!(packet[0..6], [0xFF; 6]);
        for i in 1..17 {
            assert_eq!(packet[(i * 6)..((i + 1) * 6)], [1, 2, 3, 4, 5, 6]);
        }
    }

    #[test]
    fn parse_formats() {
        let expected = Some(MacAddress::from_bytes([0xAA, 0xBB, 0xCC, 0x0D, 0xEE, 0xFF]));

        assert_eq!(parse_mac("AA:BB:CC:0D:EE:FF"), expected);
        assert_eq!(parse_mac("aa-bb-cc-0d-ee-ff"), expected);
        assert_eq!(parse_mac("aabb.cc0d.eeff"), expected);
        assert_eq!(parse_mac(" aabbcc0deeff "), expected);

        assert_eq!(parse_mac(""), None);
        assert_eq!(parse_mac("aa:bb:cc"), None);
        assert_eq!(parse_mac("zz:bb:cc:dd:ee:ff"), None);
        assert_eq!(parse_mac("00:00:00:00:00:00"), None);
        assert_eq!(parse_mac("ff:ff:ff:ff:ff:ff"), None);
    }

    #[test]
    fn null_mac_is_unusable() {
        assert_eq!(usable_mac(Some(MAC)), None);
        assert_eq!(usable_mac(None), None);

        let mac = MacAddress::from_bytes([1, 2, 3, 4, 5, 6]);
        assert_eq!(usable_mac(Some(mac)), Some(mac));
    }

    #[actix_web::test]
    async fn targets_include_host_and_broadcasts() {
        let targets = wake_targets("192.168.1.20", 47989, &["10.0.0.255:4000".to_string()]).await;

        let has = |ip: [u8; 4], port: u16| {
            targets.contains(&SocketAddr::new(IpAddr::V4(Ipv4Addr::from(ip)), port))
        };

        assert!(has([255, 255, 255, 255], 9));
        assert!(has([192, 168, 1, 20], 9));
        assert!(has([192, 168, 1, 255], 9));
        assert!(has([192, 168, 1, 255], 47009));
        // Sunshine ports
        assert!(has([192, 168, 1, 20], 47998));
        assert!(has([192, 168, 1, 20], 48010));
        // Explicit address with port
        assert!(has([10, 0, 0, 255], 4000));

        // No duplicates
        let mut deduped = targets.clone();
        deduped.sort();
        deduped.dedup();
        assert_eq!(deduped.len(), targets.len());
    }
}
