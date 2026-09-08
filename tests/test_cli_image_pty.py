"""Image composer PTY regression; clipboard subprocesses use local fixtures."""
import base64
import os
from pathlib import Path
import sys
import tempfile
import threading

from test_cli_pty import Model, Terminal, http, pexpect


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='morph-image-pty-') as temp:
        directory = Path(temp)
        source = directory / 'directory image.png'
        source.write_bytes(base64.b64decode(
            'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII='))
        binaries = directory / 'bin'
        binaries.mkdir()
        # Exercise the production clipboard pipeline without touching the user's clipboard.
        scripts = {'osascript': 'cp "$MORPH_PTY_IMAGE" "$3"',
                   'sips': 'cp "$MORPH_PTY_IMAGE" "$6"',
                   'wl-paste': 'cat "$MORPH_PTY_IMAGE"',
                   'xclip': 'cat "$MORPH_PTY_IMAGE"'}
        for name, body in scripts.items():
            script = binaries / name
            script.write_text('#!/bin/sh\n' + body + '\n')
            script.chmod(0o755)
        terminal = Terminal(driver, directory, server.server_port, {
            'PATH': str(binaries) + os.pathsep + os.environ['PATH'],
            'MORPH_PTY_IMAGE': str(source)})
        releases = []
        try:
            terminal.pump(1)
            terminal.send('Compare \x16\x16')
            terminal.pump(2)
            screen = terminal.snapshot('two clipboard image chips')
            assert '[IMAGE#1][IMAGE#2]' in screen, screen
            for y, row in enumerate(terminal.screen.display):
                for label in ('[IMAGE#1]', '[IMAGE#2]'):
                    if label in row:
                        x = row.index(label)
                        assert all(terminal.screen.buffer[y][col].bg != 'default'
                                   for col in range(x, x + len(label))), row
            terminal.child.setwinsize(24, 24)
            terminal.screen.resize(24, 24)
            narrow = terminal.snapshot('colored chips wrap in a narrow terminal')
            assert '[IMAGE#1][IMAGE#2]' in ''.join(row.rstrip() for row in narrow.splitlines()), narrow
            colored = ''.join(terminal.screen.buffer[y][x].data
                              for y in range(terminal.screen.lines)
                              for x in range(terminal.screen.columns)
                              if terminal.screen.buffer[y][x].bg != 'default')
            assert colored == '[IMAGE#1][IMAGE#2]', repr(colored)
            terminal.child.setwinsize(24, 80)
            terminal.screen.resize(24, 80)
            terminal.snapshot('chips after widening')
            terminal.send('\x7f')
            screen = terminal.snapshot('one Backspace removes the entire second image')
            assert '[IMAGE#1]' in screen and 'IMAGE#2' not in screen, screen
            terminal.send('\x1f')
            restored = terminal.snapshot('Undo restores the complete chip')
            assert '[IMAGE#1][IMAGE#2]' in restored, restored
            terminal.send('\x17')
            assert 'IMAGE#2' not in terminal.snapshot('Ctrl-W removes one chip')
            terminal.send('\x1b[200~ "' + str(source) + '"\x1b[201~')
            screen = terminal.snapshot('quoted directory path becomes a chip')
            assert '[IMAGE#3]' in screen and str(source) not in screen, screen
            terminal.send('\x1b[D\x1b[3~')
            screen = terminal.snapshot('arrow jumps over chip and Delete removes it')
            assert 'IMAGE#3' not in screen and '[IMAGE#1]' in screen, screen
            terminal.send('\x1b[200~"' + str(source) + '"\x1b[201~')
            screen = terminal.snapshot('clipboard and directory images together')
            assert '[IMAGE#4]' in screen, screen
            terminal.send('\r')
            request, release = terminal.request()
            releases.append(release)
            users = [m['content'] for m in request['messages'] if m['role'] == 'user']
            assert '[IMAGE#1] [Image:' in users[-1], users
            assert '[IMAGE#4] [Image:' in users[-1], users
            assert 'IMAGE#2' not in users[-1] and 'IMAGE#3' not in users[-1], users
            assert users[-1].count('[Image:') == 2, users
            terminal.send('\x16')
            assert '[IMAGE#5]' in terminal.snapshot('image paste while model runs')
            terminal.send('\x7fNo image now\r')
            request2, release2 = terminal.request()
            releases.append(release2)
            users = [m['content'] for m in request2['messages'] if m['role'] == 'user']
            assert users[-1] == 'No image now', users
            release.set()
            release2.set()
            terminal.pump(1)
            terminal.send('/quit\r')
            terminal.child.expect(pexpect.EOF)
            terminal.child.close()
            assert terminal.child.exitstatus == 0
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts[:1]
            assert '\x1b_G' not in terminal.raw and ']1337;File=' not in terminal.raw
            print('PASS: colored image chips, multi-image paste, file paths, atomic deletion, active-turn input, submitted image references')
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)
            artifacts = Path(tempfile.gettempdir()) / ('morph-image-pty-' + driver.parents[1].name)
            artifacts.mkdir(exist_ok=True)
            (artifacts / 'screens.txt').write_text('\n\n'.join(
                name + '\n' + screen for name, screen in terminal.snapshots))
            (artifacts / 'transcript.ansi').write_text(terminal.raw)
            server.shutdown()


if __name__ == '__main__':
    main()
