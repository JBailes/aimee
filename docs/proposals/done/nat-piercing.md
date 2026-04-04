# Proposal: NAT Piercing via Reverse SSH Tunnels

## Problem

Aimee runs behind NAT/firewall on internal networks. External delegate agents (Codex, Claude, etc.) cannot initiate connections to aimee's internal networks (WOL at 10.0.0.0/24, ACK at 10.1.0.0/24, LAN at 192.168.1.0/24).

The current architecture requires a pre-configured externally-accessible SSH server (`ssh_entry` in agents.json, e.g., `ssh -p 2222 deploy@deploy.ackmud.com`). Aimee generates ephemeral Ed25519 keys per session (`agent_ssh_setup()` in `agent_context.c:790-892`), authorizes them on the jump host, and delegates SSH through it. This works but has a hard dependency on manually maintaining that external server with inbound port forwarding.

## Goals

- Aimee establishes and manages its own reverse SSH tunnels to reach internal networks from external delegates.
- No inbound port forwarding required on aimee's network — aimee connects outward.
- Per-host tunnel resolution — each internal host can specify which tunnel to use.
- Backward compatible — existing configs with just `ssh_entry` continue to work unchanged.
- Tunnel lifecycle matches delegate execution (start before, stop after, auto-reconnect on failure).

## Approach

### Architecture

```
Aimee (behind NAT) --[ssh -R 0:target:22 -N]--> Relay (public IP)
                                                    |
Delegate (Codex)   --[SSH to relay:DYNAMIC_PORT]----+--[through tunnel]--> Internal host
```

Aimee initiates an outbound SSH connection to a relay server and establishes a reverse port forward (`ssh -R 0:target_host:target_port`). The relay allocates a dynamic port. Delegates connect to that port on the relay, and traffic flows back through the tunnel.

### Config format

```json
{
  "network": {
    "ssh_entry": "ssh -p 2222 deploy@deploy.ackmud.com",
    "tunnels": [
      {
        "name": "lan",
        "relay_ssh": "ssh relay@relay.example.com",
        "relay_key": "~/.ssh/relay_ed25519",
        "target_host": "192.168.1.101",
        "target_port": 22
      }
    ],
    "hosts": [
      {"name": "proxmox", "ip": "192.168.1.253", "user": "root", "tunnel": "lan", "desc": "Proxmox VE"}
    ],
    "networks": [
      {"name": "LAN", "cidr": "192.168.1.0/24", "desc": "Home LAN"}
    ]
  }
}
```

### New types (`agent_types.h`)

- `agent_tunnel_state_t` — enum: IDLE, CONNECTING, ACTIVE, RECONNECTING, FAILED, STOPPED
- `agent_tunnel_t` — config (name, relay_ssh, relay_key, target_host, target_port) + runtime (state, allocated_port, ssh_pid, monitor_thread, effective_ssh_entry)
- `agent_tunnel_mgr_t` — tunnel array, count, mutex, shutdown flag
- `tunnel[64]` field on `agent_net_host_t` — associates a host with a named tunnel
- `tunnel_mgr` pointer on `agent_network_t` — links network config to tunnel runtime

### Tunnel lifecycle (`agent_tunnel.c`)

1. **Start:** fork/exec `ssh -R 0:target:port -N -o ServerAliveInterval=15 -o ServerAliveCountMax=3 -o ExitOnForwardFailure=yes relay`. Parse stderr for `"Allocated port N"`. Store dynamic port.
2. **Monitor:** per-tunnel pthread monitors ssh child via `waitpid(WNOHANG)`. Reconnects on failure with configurable delay (default 5s) and max retries.
3. **Stop:** `kill(ssh_pid, SIGTERM)`, join monitor thread, cleanup.
4. **Signal safety:** SIGTERM handler kills all ssh_pids directly (async-signal-safe). SSH processes with ServerAliveInterval self-terminate within 45s if parent dies without signaling.

### Integration points

- `agent.c:398` — start tunnels before delegate execution, stop at cleanup
- `agent_context.c:626-662` — resolve per-host SSH entry via `agent_tunnel_resolve_entry()`. Active tunnel: emit `ssh -p DYNAMIC_PORT -i KEY user@relay`. No tunnel: fall back to ssh_entry.
- `agent_config.c:209-277` — parse/serialize tunnel config from agents.json
- `cmd_agent.c` — `aimee agent tunnel` status subcommand

### Changes

| File | Change |
|------|--------|
| `src/headers/agent_types.h` | New tunnel types, tunnel field on host/network |
| `src/headers/agent_tunnel.h` | New header — public API |
| `src/agent_tunnel.c` | New — tunnel lifecycle (~400 lines) |
| `src/agent_config.c` | Parse/serialize tunnel config |
| `src/agent.c` | Start/stop tunnels in `agent_execute_with_tools()` |
| `src/agent_context.c` | Per-host tunnel entry point resolution |
| `src/cmd_agent.c` | `aimee agent tunnel` subcommand |
| `src/Makefile` | Add `agent_tunnel.c` to `AGENT_SRCS` |

## Acceptance Criteria

- [ ] Config with no tunnels: existing behavior unchanged, all tests pass
- [ ] Config with tunnels: `aimee agent tunnel` shows tunnel name, state, relay, target, allocated port
- [ ] Tunnel starts on delegate execution, dynamic port allocated, delegate context shows per-host SSH commands
- [ ] Tunnel auto-reconnects on SSH process death (up to max_reconnects)
- [ ] Tunnel stops on delegate execution cleanup (normal exit, error, timeout)
- [ ] SIGTERM to aimee kills all tunnel SSH processes
- [ ] Ephemeral keys authorized on relay (not just ssh_entry) when tunnels are active

## Owner and Effort

- **Owner:** JBailes
- **Effort:** M (~400 lines new code + ~100 lines modifications)
- **Dependencies:** None — fully backward compatible

## Rollout and Rollback

- **Rollout:** Direct code changes. Tunnels are opt-in via config — no behavioral change for existing users.
- **Rollback:** `git revert`. No migrations or state changes.
- **Blast radius:** Only affects delegate execution when tunnels are configured. Misconfigured tunnels fall back to ssh_entry.

## Test Plan

- [ ] Unit tests: config parsing with and without tunnels, tunnel state machine transitions
- [ ] Unit tests: `agent_tunnel_resolve_entry()` returns tunnel entry when active, falls back to ssh_entry when not
- [ ] Integration test: start tunnel to localhost sshd, verify dynamic port allocation via `ssh -R 0:localhost:22`
- [ ] Integration test: kill ssh child, verify monitor thread reconnects within delay
- [ ] Integration test: `aimee agent tunnel` output matches configured tunnels
- [ ] Backward compat: run existing test suite with no tunnel config — zero behavioral change

## Operational Impact

- **Metrics:** None new.
- **Logging:** Tunnel state changes logged to stderr (CONNECTING, ACTIVE, RECONNECTING, FAILED).
- **Alerts:** None.
- **Disk/CPU/Memory:** One ssh process per tunnel (~2MB RSS each). One monitor pthread per tunnel. Negligible CPU.

## Priority

P2 — enhances delegate infrastructure but existing ssh_entry model works for current deployment.

## Trade-offs

**Why reverse SSH tunnels, not WireGuard/Tailscale?** SSH is already in the codebase, requires no additional software on the relay, and the ephemeral key model is proven. WireGuard would require kernel module or userspace implementation on both sides. Tailscale adds an external dependency and account.

**Why dynamic ports (`ssh -R 0:...`)?** Avoids port conflicts. The relay OS assigns an unused port. The only risk is running out of ephemeral ports, which is not practically a concern.

**Why per-host tunnels, not per-network?** SSH reverse tunnels forward to a specific host:port. A per-network tunnel would require a SOCKS proxy or jump host at the tunnel endpoint. Per-host is simpler and matches the existing model where delegates SSH to specific hosts.

**Thread per tunnel:** Simple, debuggable, matches the existing fork/waitpid patterns. An event-loop alternative (epoll on pipe fds) would avoid threads but adds complexity for minimal benefit with <=8 tunnels.
