use std::ffi::CStr;
use std::net::{Ipv4Addr, Ipv6Addr};

pub fn addresses(ipv6_interface: &str) -> (String, String) {
    let mut ipv4 = String::new();
    let mut ipv6 = String::new();
    unsafe {
        let mut interfaces = std::ptr::null_mut();
        if libc::getifaddrs(&mut interfaces) != 0 {
            return (ipv4, ipv6);
        }
        let mut current = interfaces;
        while !current.is_null() {
            let interface = &*current;
            if interface.ifa_addr.is_null() {
                current = interface.ifa_next;
                continue;
            }
            let family = (*interface.ifa_addr).sa_family as i32;
            if ipv4.is_empty()
                && family == libc::AF_INET
                && interface.ifa_flags & libc::IFF_LOOPBACK as u32 == 0
            {
                let address = &*(interface.ifa_addr as *const libc::sockaddr_in);
                ipv4 = Ipv4Addr::from(u32::from_be(address.sin_addr.s_addr)).to_string();
            }
            if ipv6.is_empty() && family == libc::AF_INET6 && !interface.ifa_name.is_null() {
                let name = CStr::from_ptr(interface.ifa_name).to_string_lossy();
                if name == ipv6_interface {
                    let address = &*(interface.ifa_addr as *const libc::sockaddr_in6);
                    let candidate = Ipv6Addr::from(address.sin6_addr.s6_addr);
                    if !candidate.is_loopback()
                        && !candidate.is_unspecified()
                        && !candidate.is_unicast_link_local()
                    {
                        ipv6 = candidate.to_string();
                    }
                }
            }
            current = interface.ifa_next;
        }
        libc::freeifaddrs(interfaces);
    }
    (ipv4, ipv6)
}

pub fn global_ipv6(interface: &str) -> String {
    addresses(interface).1
}
