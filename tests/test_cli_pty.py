"""Real PTY + delayed local SSE model regression.

Run: python3 -m pip install pexpect pyte
     python3 tests/test_cli_pty.py build/bin/morph-cli-pty-driver
"""
import http.server
import json
import os
from pathlib import Path
import queue
import sys
import tempfile
import threading
import time

try:
    import pexpect
    import pyte
except ImportError:
    print('PTY tests require pexpect and pyte; see tests/test_cli_pty.py')
    sys.exit(77)


class Model(http.server.BaseHTTPRequestHandler):
    requests = queue.Queue()

    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        release = threading.Event()
        self.requests.put((body, release))
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        try:
            self.wfile.write(b': waiting\n\n')
            self.wfile.flush()
            deadline = time.monotonic() + 15
            chunks = 0
            while not release.wait(0.2) and time.monotonic() < deadline:
                if chunks < 3:
                    chunk = {'choices': [{'index': 0, 'delta': {
                        'content': f'Live output {chunks}.\n\n'}}]}
                    self.wfile.write(('data: ' + json.dumps(chunk) + '\n\n').encode())
                    self.wfile.flush()
                    chunks += 1
            payload = {'choices': [{'index': 0, 'delta': getattr(release, 'delta', {'content': 'UPDATED ANSWER'}),
                                    'finish_reason': None}]}
            self.wfile.write(('data: ' + json.dumps(payload) + '\n\n').encode())
            self.wfile.write(b'data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class Terminal:
    def __init__(self, driver, directory, port, extra_env=None, memory_enabled=False):
        config = directory / 'config.toml'
        config.write_text(f'''[general]
output_dir = "{directory}/output"
[memory]
enabled = {str(memory_enabled).lower()}
llm_extract_enabled = false
cold_path_enabled = false
[model.text]
provider = "openai"
model = "pty-test"
api_base = "http://127.0.0.1:{port}/v1"
api_key_env = "MORPH_PTY_API_KEY"
retry_count = 0
''')
        self.child = pexpect.spawn(str(driver), [str(config), str(directory),
                                               str(directory / 'data.db')],
                                   env={**os.environ, 'TERM': 'xterm-256color',
                                        'MORPH_PTY_API_KEY': 'local-test', **(extra_env or {})},
                                   encoding='utf-8', dimensions=(24, 80), timeout=8)
        self.screen = pyte.Screen(80, 24)
        self.stream = pyte.Stream(self.screen)
        self.duplicate_prompts = []
        self.raw = ''
        self.snapshots = []

    def pump(self, duration=0.3):
        until = time.monotonic() + duration
        while time.monotonic() < until:
            try:
                data = self.child.read_nonblocking(65536, timeout=0.03)
            except pexpect.TIMEOUT:
                continue
            except pexpect.EOF:
                return
            self.raw += data
            self.stream.feed(data)
            if any(row.count('›') > 1 for row in self.screen.display):
                self.duplicate_prompts.append('\n'.join(self.screen.display))

    def snapshot(self, label):
        self.pump()
        text = '\n'.join(self.screen.display)
        self.snapshots.append((label, text))
        print('checked:', label, flush=True)
        assert sum(row.strip() == '›' for row in text.splitlines()) <= 1, text
        return text

    def send(self, text):
        self.child.send(text)
        self.pump()

    def request(self):
        until = time.monotonic() + 8
        while time.monotonic() < until:
            self.pump(0.05)
            try:
                return Model.requests.get_nowait()
            except queue.Empty:
                pass
        raise AssertionError('model request did not arrive\n' + self.raw[-3000:])


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='morph-pty-') as temp:
        terminal = Terminal(driver, Path(temp), server.server_port)
        releases = []
        try:
            terminal.pump(1)
            assert '›' in terminal.snapshot('idle')
            terminal.send('\r\r\r')
            empty = terminal.snapshot('repeated empty Enter')
            assert sum(row.strip() == '›' for row in empty.splitlines()) == 1, empty
            terminal.send('  \r\r\x15')
            terminal.snapshot('whitespace Enter')
            terminal.send('initial task\r')
            first, release = terminal.request()
            releases.append(release)
            assert '›' in terminal.snapshot('model running'), 'composer missing'
            terminal.send('改成中文🙂')
            draft = terminal.snapshot('Chinese draft during spinner')
            assert '改成中文🙂' in draft, draft
            terminal.send('\x1b[D\x7f')
            terminal.send('新')
            terminal.send('\x05\r')
            assert 'Requirement queued' in terminal.raw
            started = time.monotonic()
            second, release2 = terminal.request()
            releases.append(release2)
            assert time.monotonic() - started < 2.5, 'steering waited for model completion'
            assert not release.is_set(), 'first request must still be gated'
            user_text = [m['content'] for m in second['messages'] if m['role'] == 'user']
            assert user_text[-1] == '改成中新🙂', user_text
            terminal.send('second adjustment\r')
            third, release_next = terminal.request()
            releases.append(release_next)
            user_text = [m['content'] for m in third['messages'] if m['role'] == 'user']
            assert user_text[-2:] == ['改成中新🙂', 'second adjustment'], user_text
            release.set()
            release2.set()
            release2 = release_next
            terminal.send('burst one\rburst two\rburst three\r')
            for _ in range(3):
                burst, burst_release = terminal.request()
                releases.append(burst_release)
                users = [m['content'] for m in burst['messages'] if m['role'] == 'user']
                if users[-3:] == ['burst one', 'burst two', 'burst three']:
                    break
            else:
                raise AssertionError(f'rapid submissions lost: {users}')
            release2.set()
            release2 = burst_release
            terminal.send('unsent draft 中文')
            draft = terminal.snapshot('draft across model transition')
            assert 'unsent draft 中文' in draft
            assert draft.count('Thinking') <= 1, draft
            terminal.child.setwinsize(24, 40)
            terminal.screen.resize(24, 40)
            terminal.pump(0.5)
            assert 'unsent draft 中文' in terminal.snapshot('narrow resize')
            terminal.send('\x15' + 'long 中文 ' * 10)
            terminal.snapshot('wrapped draft')
            release2.set()
            terminal.pump(1)
            completed = terminal.snapshot('wrapped draft after completion')
            assert completed.count('long') == 10, completed
            assert completed.count('UPDATED ANSWER') == 1, completed
            terminal.send('\x15\r')
            terminal.send('ask a question\r')
            ask_request, ask_release = terminal.request()
            releases.append(ask_release)
            terminal.send('draft before question')
            ask_release.delta = {'tool_calls': [{'index': 0, 'id': 'ask-1',
                'type': 'function', 'function': {'name': 'ask_user',
                'arguments': json.dumps({'question': 'Choose a color', 'choices': ['Blue', 'Red']})}}]}
            ask_release.set()
            terminal.pump(1)
            question = terminal.snapshot('ask_user owns input')
            assert 'Choose a color' in question, question
            terminal.send('1\r')
            answered, answered_release = terminal.request()
            releases.append(answered_release)
            assert any(m['role'] == 'tool' and 'Blue' in m['content'] for m in answered['messages'])
            assert 'draft before question' in terminal.snapshot('draft restored after ask_user')
            answered_release.set()
            terminal.pump(0.5)
            terminal.send('\x15\r')
            terminal.send('cancel this\r')
            third, release3 = terminal.request()
            releases.append(release3)
            terminal.send('\x1b')
            terminal.pump(1.5)
            release3.set()
            terminal.pump(0.5)
            assert '›' in terminal.snapshot('after escape cancellation')
            terminal.send('ctrl-c test\r')
            _, ctrl_release = terminal.request()
            releases.append(ctrl_release)
            terminal.send('draft to cancel')
            terminal.send('\x03')
            terminal.pump(1.2)
            cancelled = terminal.snapshot('after Ctrl-C')
            assert 'Thinking' not in cancelled, cancelled
            ctrl_release.set()
            terminal.send('\x1b[200~pasted 中文\nsecond line\x1b[201~')
            terminal.snapshot('bracketed multiline paste')
            terminal.send('\r')
            pasted, paste_release = terminal.request()
            releases.append(paste_release)
            users = [m['content'] for m in pasted['messages'] if m['role'] == 'user']
            assert users[-1] == 'pasted 中文\nsecond line', users
            terminal.send('line one\x0aline two\x1b\rline three')
            multiline = terminal.snapshot('Ctrl-J and Alt-Enter draft')
            assert all(line in multiline for line in ['line one', 'line two', 'line three']), multiline
            terminal.child.setwinsize(24, 80)
            terminal.screen.resize(24, 80)
            terminal.pump(0.5)
            terminal.snapshot('widen with multiline draft')
            paste_release.set()
            terminal.pump(0.5)
            terminal.send('\x01\x0b\x15')
            terminal.send('\x03')
            terminal.send('/quit\r')
            terminal.child.expect(pexpect.EOF)
            terminal.child.close()
            assert terminal.child.exitstatus == 0
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts[0]
            print('PASS: live input, prompt interruption, UTF-8 editing, FIFO steering, draft persistence, resize, wrapping, ask_user, Esc, Ctrl-C, multiline paste, exit')
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)
            artifacts = Path(tempfile.gettempdir()) / ('morph-pty-artifacts-' + driver.parents[1].name)
            artifacts.mkdir(exist_ok=True)
            (artifacts / 'transcript.ansi').write_text(terminal.raw)
            (artifacts / 'screens.txt').write_text('\n\n'.join(
                label + '\n' + screen for label, screen in terminal.snapshots))
            server.shutdown()


if __name__ == '__main__':
    main()
