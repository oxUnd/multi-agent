"""Real composer, commands, streaming steering and persisted preference QA."""
import http.server
from pathlib import Path
import sys
import tempfile
import threading

from test_cli_pty import Model, Terminal


def system_prompt(body):
    return '\n'.join(message.get('content', '') for message in body['messages']
                     if message['role'] in ('system', 'developer'))


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    releases = []
    terminal = None
    with tempfile.TemporaryDirectory(prefix='morph-memory-pty-') as temp:
        directory = Path(temp)
        try:
            terminal = Terminal(driver, directory, server.server_port, memory_enabled=True)
            terminal.pump(1)
            terminal.send('/memory set language Chinese\r')
            assert 'Preference saved' in terminal.raw, terminal.raw[-4000:]
            assert 'response.language: Chinese [personal' in terminal.raw
            terminal.snapshot('preference command committed')
            terminal.send('检查项目\r')
            first, release = terminal.request()
            releases.append(release)
            assert 'response.language: Chinese [personal' in system_prompt(first)
            terminal.send('以后都用英文回答，默认简洁\r')
            second, release = terminal.request()
            releases.append(release)
            assert 'response.language: English [personal' in system_prompt(second), second
            assert 'response.language: Chinese' not in system_prompt(second)
            assert 'response.detail: concise' in system_prompt(second)
            terminal.snapshot('steering updates next model request')
            for event in releases:
                event.set()
            terminal.pump(1)
            terminal.send('/memory history\r')
            assert 'Preference history and sources' in terminal.raw
            terminal.snapshot('history and effective scope')
            terminal.child.setwinsize(24, 48)
            terminal.pump()
            terminal.snapshot('narrow terminal memory view')
            terminal.child.setwinsize(24, 80)
            terminal.send('/quit\r')
            terminal.child.expect(__import__('pexpect').EOF, timeout=10)
            assert not terminal.duplicate_prompts

            terminal = Terminal(driver, directory, server.server_port, memory_enabled=True)
            terminal.pump(1)
            terminal.send('这次用中文回答\r')
            temporary, release = terminal.request()
            releases.append(release)
            assert 'response.language: English [personal' in system_prompt(temporary)
            release.set()
            terminal.pump(1)
            terminal.send('继续工作\r')
            following, release = terminal.request()
            releases.append(release)
            assert 'response.language: English [personal' in system_prompt(following)
            release.set()
            terminal.pump(1)
            terminal.snapshot('restart inheritance and temporary request isolation')
            terminal.send('/memory set language Chinese session\r')
            assert 'response.language: Chinese [session' in terminal.raw
            terminal.send('/memory unset language session\r')
            terminal.send('/memory explain\r')
            terminal.snapshot('unset restores personal preference')
            terminal.send('/memory unset language\r')
            terminal.send('检查删除结果\r')
            cleared, release = terminal.request()
            releases.append(release)
            assert 'response.language:' not in system_prompt(cleared)
            release.set()
            terminal.pump(1)
            assert not terminal.duplicate_prompts
            terminal.send('/quit\r')
            terminal.child.expect(__import__('pexpect').EOF, timeout=10)
            print('PASS: memory commands, streaming steering, restart, temporary scope, unset')
        finally:
            for event in releases:
                event.set()
            if terminal and terminal.child.isalive():
                terminal.child.close(force=True)
            server.shutdown()


if __name__ == '__main__':
    main()
