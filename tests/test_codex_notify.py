import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('setup', ROOT / 'packaging/configure-codex.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class CodexNotifyTests(unittest.TestCase):
    def test_install_preserves_config_and_conflicting_hook(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'CODEX_HOME': tmp}):
            config = Path(tmp) / 'config.toml'
            original = '# café\nmodel = "example"\n[features]\nexample = true\n'
            config.write_text(original)
            setup.install('/tmp/Orbit with spaces')
            installed = config.read_text()
            self.assertTrue(installed.endswith(original))
            self.assertEqual((Path(tmp) / 'config.toml.orbit-backup').read_text(), original)
            setup.install('/tmp/Orbit with spaces')
            self.assertEqual(config.read_text(), installed)
            existing = 'notify = ["existing-hook"]\n' + original
            config.write_text(existing)
            with self.assertRaises(ValueError):
                setup.install('/tmp/orbit')
            self.assertEqual(config.read_text(), existing)

    def test_invalid_toml_is_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'CODEX_HOME': tmp}):
            config = Path(tmp) / 'config.toml'
            config.write_text('invalid = [')
            with self.assertRaises(ValueError):
                setup.install('/tmp/orbit')
            self.assertEqual(config.read_text(), 'invalid = [')

    def test_headless_delivery_excludes_prompt_and_bounds_reply(self):
        binary = ROOT / 'build/Desktop_Release/nameless-orbit'
        if not binary.exists():
            self.skipTest('Build the desktop binary first')
        with tempfile.TemporaryDirectory() as tmp:
            server = socket.socket(socket.AF_UNIX)
            try:
                server.bind(str(Path(tmp) / 'nameless-orbit-orbit-test'))
            except PermissionError:
                server.close()
                self.skipTest('Unix sockets are blocked by the execution sandbox')
            server.listen(1)
            server.settimeout(5)
            received = []
            def receive():
                connection, _ = server.accept()
                with connection:
                    data = b''
                    while b'\n' not in data:
                        data += connection.recv(16384)
                    received.append(json.loads(data))
                    connection.sendall(b'{"ok":true}\n')
            thread = threading.Thread(target=receive)
            thread.start()
            event = {'type': 'agent-turn-complete', 'input-messages': ['SECRET'], 'cwd': '/SECRET', 'last-assistant-message': 'Ответ! ' * 1000}
            env = dict(os.environ, USER='orbit-test', TMPDIR=tmp, QT_QPA_PLATFORM='invalid')
            result = subprocess.run([str(binary), '--codex-notify', json.dumps(event)], env=env, capture_output=True, timeout=6)
            thread.join(6)
            server.close()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(received[0]['command'], 'codexReply')
            self.assertEqual(len(received[0]['body']), 600)
            self.assertNotIn('SECRET', json.dumps(received))

    def test_headless_validation_needs_no_gui(self):
        binary = ROOT / 'build/Desktop_Release/nameless-orbit'
        for payload, status in [('{', 2), ('{"type":"other"}', 0)]:
            result = subprocess.run([str(binary), '--codex-notify', payload], env=dict(os.environ, QT_QPA_PLATFORM='invalid'), capture_output=True, timeout=3)
            self.assertEqual(result.returncode, status)



if __name__ == '__main__':
    unittest.main()
