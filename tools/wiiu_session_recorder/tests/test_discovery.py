import sys
from pathlib import Path
from unittest import TestCase, mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import discovery

class DiscoveryTests(TestCase):
    def test_probe_requires_aroma_layout(self):
        ftp = mock.MagicMock()
        ftp.size.return_value = 123
        with mock.patch.object(discovery, 'FTP', return_value=ftp):
            ftp.__enter__.return_value = ftp
            self.assertEqual(discovery.probe_ftp('192.168.1.2'), '192.168.1.2')
            ftp.size.return_value = None
            self.assertIsNone(discovery.probe_ftp('192.168.1.2'))
        ftp.storbinary.assert_not_called()
    def test_scan_is_local_bounded_and_excludes_self(self):
        with mock.patch.object(discovery.socket, 'getaddrinfo', return_value=[(0,0,0,0,('192.168.10.12',0))]), mock.patch.object(discovery.socket, 'socket', side_effect=OSError):
            hosts=discovery.local_candidates('192.168.10.49')
        self.assertEqual(hosts[0], '192.168.10.49')
        self.assertEqual(len(hosts),253)
        self.assertNotIn('192.168.10.12',hosts)
        self.assertNotIn('192.168.10.255',hosts)
    def test_listener_busy_falls_back_without_interrupting_it(self):
        with mock.patch.object(discovery.socket, 'socket', side_effect=OSError), mock.patch.object(discovery,'local_candidates',return_value=['192.168.10.49','192.168.10.50']), mock.patch.object(discovery,'probe_ftp',side_effect=lambda host: host if host.endswith('.49') else None):
            self.assertEqual(discovery.discover(timeout=1),['192.168.10.49'])
