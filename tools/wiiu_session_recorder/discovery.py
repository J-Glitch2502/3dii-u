"""Bounded local Wii U discovery through Aroma syslog or read-only FTP checks."""
import ipaddress
import socket
import time
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
from ftplib import FTP, all_errors

BEACON = b"WIIU_TCP_SYSLOG_BEACON_V1"


def local_candidates(preferred=None):
    addresses = set()
    try:
        addresses.update(item[4][0] for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET))
    except OSError:
        pass
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
            route.connect(('192.0.2.1', 9))  # select a route; no datagram is sent
            addresses.add(route.getsockname()[0])
    except OSError:
        pass
    networks = set()
    for value in addresses:
        address = ipaddress.IPv4Address(value)
        if address.is_private and not address.is_loopback and not address.is_link_local:
            networks.add(ipaddress.ip_network(value + '/24', strict=False))
    result = []
    for network in sorted(networks, key=lambda n: int(n.network_address))[:4]:
        result.extend(str(ip) for ip in network.hosts() if str(ip) not in addresses)
    if preferred in result:
        result.remove(preferred)
        result.insert(0, preferred)
    return result


def probe_ftp(host):
    try:
        with FTP() as ftp:
            ftp.connect(host, 21, timeout=0.35)
            ftp.login()
            ftp.voidcmd('TYPE I')
            # A generic open FTP port is insufficient: require Aroma's SD layout.
            for environment in ('aroma', 'aroma-debug'):
                try:
                    if ftp.size('/fs/vol/external01/wiiu/environments/' + environment + '/root.rpx') > 0:
                        return host
                except all_errors + (TypeError,):
                    pass
    except all_errors:
        pass
    return None


def discover(preferred=None, timeout=4.0):
    found = set()
    started = time.monotonic()
    # Aroma's TCP logger connects back after this standard UDP beacon. This
    # also discovers consoles with FTP disabled. An existing recorder may own
    # the port; then use the read-only FTP fallback without disrupting it.
    try:
        with socket.socket() as listener, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as beacon:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(('', 7965))
            listener.listen(4)
            listener.settimeout(min(0.8, timeout))
            beacon.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            beacon.sendto(BEACON, ('255.255.255.255', 4445))
            deadline = time.monotonic() + min(0.8, timeout)
            while time.monotonic() < deadline:
                try:
                    client, peer = listener.accept()
                    with client:
                        client.settimeout(0.2)
                        if ipaddress.IPv4Address(peer[0]).is_private and client.recv(256):
                            found.add(peer[0])
                except (OSError, ValueError):
                    break
    except OSError:
        pass
    if found:
        return sorted(found, key=ipaddress.IPv4Address)
    candidates = local_candidates(preferred)
    pool = ThreadPoolExecutor(max_workers=24)
    pending = {pool.submit(probe_ftp, host) for host in candidates}
    deadline = started + timeout
    try:
        while pending and time.monotonic() < deadline:
            completed, pending = wait(pending, timeout=max(0, deadline-time.monotonic()), return_when=FIRST_COMPLETED)
            for future in completed:
                result = future.result()
                if result:
                    found.add(result)
    finally:
        pool.shutdown(wait=False, cancel_futures=True)
    return sorted(found, key=ipaddress.IPv4Address)
