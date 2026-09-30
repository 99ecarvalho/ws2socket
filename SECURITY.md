# Security Policy

## Supported versions

ws2socket is in early development. Security fixes are made on the `main`
branch and included in the next release.

| Version | Supported |
|---|---|
| `main` | Yes |
| 0.1.x | Yes |
| Older | No |

## Reporting a vulnerability

**Please do not report security vulnerabilities in public issues, pull
requests or discussions.**

Report them privately instead, in either of these ways:

1. **GitHub private vulnerability reporting** (preferred): open the
   [Security tab](https://github.com/99ecarvalho/ws2socket/security) of the
   repository and click **Report a vulnerability**.
2. **Email**: write to Eduardo Correia at <ecorreia@apliant.com.br> with
   `[ws2socket security]` in the subject line.

Please include:

- the affected version or commit;
- a description of the issue and its impact;
- steps to reproduce, or a proof of concept;
- any known workaround.

### What to expect

- You should receive an acknowledgement within **7 days**.
- We will investigate, keep you informed of progress, and agree on a
  disclosure date with you. The target is a fix within **90 days**, and sooner
  for severe issues.
- Once a fix is released, the issue is disclosed in a GitHub security
  advisory. You are credited unless you prefer to stay anonymous.

This is a volunteer-maintained project. If you have had no response after 14
days, please send a reminder.

## Scope

Examples of what we consider vulnerabilities:

- memory-safety bugs (buffer overflows, use-after-free, out-of-bounds reads)
  in the HTTP, WebSocket frame, compression or configuration parsers;
- serving files outside `--web-root` (path traversal);
- crashes or resource exhaustion that one unauthenticated client can trigger
  and that affect other sessions or the listener;
- ways to make ws2socket connect somewhere other than the configured target,
  or other than the target of the token presented;
- ways to bypass TLS, `max_connections` or token checks.

### Known limitations

The following are documented design limitations of the current release, not
vulnerabilities. See
[docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md#known-limitations) for
details.

- ws2socket does **not authenticate users**. Without a token file, anyone who
  can reach the listening port can reach the target. With one, anyone who
  knows a valid token can.
- Tokens are bearer secrets carried in the URL. Browsers may keep them in
  history, and proxies may log them.

Reports that show how these limitations can be exploited *beyond* the
documented behavior are still welcome.

## Deploying ws2socket safely

- Enable TLS (`--cert`/`--key`) on any network you do not fully trust.
  Otherwise VNC traffic and tokens travel in the clear.
- Restrict who can reach the port with a firewall, or bind ws2socket to
  `127.0.0.1` behind a reverse proxy (nginx, Caddy, HAProxy) that adds
  authentication. An example is in
  [docs/NOVNC_GUIDE.md](docs/NOVNC_GUIDE.md#running-behind-nginx).
- When using a token file, generate long random tokens
  (`openssl rand -hex 16`), and keep the file readable only by the ws2socket
  user.
- Keep `max_connections` at a level your system can handle.
- Run it as an unprivileged user. The systemd unit in the noVNC guide and the
  Docker image both do this.
- Protect the target service with its own authentication, such as a VNC
  password.
