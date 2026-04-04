# Proposal: Fedora / RHEL-Family Support (dnf/yum Compatibility)

## Problem

Aimee's build scripts, installer, and runtime guardrails assume a Debian-family system. Users on Fedora, RHEL, AlmaLinux, or Rocky Linux hit failures at multiple points:

1. **`setup.sh` is Debian-only** (`setup.sh:5-6`): Hardcodes `apt-get update && apt-get install` for build dependencies. On Fedora this fails immediately — there is no alternative path.

2. **`install.sh` uses apt for runtime deps** (`install.sh:51,57`): Auto-installs `universal-ctags` and `sqlite3` via `apt-get`. On Fedora these would be `ctags` (from `ctags` or `universal-ctags`) and `sqlite` (already present), but the script doesn't try `dnf`.

3. **`install.sh` error messages are Debian-specific** (`install.sh:43`): The libcurl error says `apt-get install libcurl4-openssl-dev` — the Fedora equivalent is `dnf install libcurl-devel`.

4. **Guardrails miss dnf/yum/rpm write commands** (`guardrails.c:321`): The `pkg_cmds[]` list covers `apt-get install` and `apt install` but not `dnf install`, `yum install`, `dnf remove`, `yum remove`, `rpm -i`, or `rpm -e`. An agent on Fedora can run `dnf install malicious-package` without triggering the write-command guardrail.

5. **Makefile tool hints are apt-only** (`src/Makefile:294,296,307,309`): `cppcheck` and `clang-tidy` install suggestions say `apt install`. Fedora users see misleading advice.

6. **No RPM spec file or Copr package**: Users must build from source with no packaging guidance.

7. **No CI coverage for RPM-based distros**: A glibc, OpenSSL, or SQLite change in Fedora could break the build silently.

8. **Package name differences are undocumented**: Fedora uses `-devel` suffixes (not `-dev`), different package names for several libraries, and `dnf` group installs instead of metapackages.

## Goals

- Aimee builds cleanly and passes all tests on current Fedora (41+) and RHEL 9 / Alma 9 / Rocky 9.
- `setup.sh` and `install.sh` detect the package manager and use the correct commands.
- Guardrails correctly classify `dnf`, `yum`, and `rpm` write operations.
- `aimee env` reports `package_manager=dnf` on Fedora.
- An RPM spec file exists for `rpmbuild` and potential Copr/EPEL submission.
- Documentation covers Fedora/RHEL build instructions.

## Approach

### 1. Guardrails: recognize dnf/yum/rpm commands

Add to the `pkg_cmds[]` array in `guardrails.c`:

```c
"dnf install", "dnf remove", "dnf erase", "dnf upgrade", "dnf update",
"yum install", "yum remove", "yum erase", "yum upgrade", "yum update",
"rpm -i", "rpm -e", "rpm -U", "rpm --install", "rpm --erase", "rpm --upgrade",
```

This mirrors the existing `apt-get install`, `apt install` entries.

### 2. Package manager detection: distro-aware install helpers

Create a shared shell function library (`distro-detect.sh`) sourced by `setup.sh`, `install.sh`, and `update.sh`:

```bash
detect_pkg_manager() {
    if command -v dnf &>/dev/null; then
        PKG_MGR="dnf"
        PKG_INSTALL="sudo dnf install -y"
        PKG_UPDATE="sudo dnf makecache"
    elif command -v yum &>/dev/null; then
        PKG_MGR="yum"
        PKG_INSTALL="sudo yum install -y"
        PKG_UPDATE="sudo yum makecache"
    elif command -v apt-get &>/dev/null; then
        PKG_MGR="apt"
        PKG_INSTALL="sudo apt-get install -y"
        PKG_UPDATE="sudo apt-get update -qq"
    elif command -v pacman &>/dev/null; then
        PKG_MGR="pacman"
        PKG_INSTALL="sudo pacman -S --noconfirm"
        PKG_UPDATE="sudo pacman -Sy"
    elif command -v brew &>/dev/null; then
        PKG_MGR="brew"
        PKG_INSTALL="brew install"
        PKG_UPDATE="brew update"
    else
        PKG_MGR="unknown"
    fi
}

# Map abstract dependency names to distro-specific package names
pkg_name() {
    local dep="$1"
    case "$PKG_MGR" in
        dnf|yum)
            case "$dep" in
                libsqlite3-dev) echo "sqlite-devel" ;;
                libcurl4-openssl-dev) echo "libcurl-devel" ;;
                libpam0g-dev) echo "pam-devel" ;;
                libsecret-1-dev) echo "libsecret-devel" ;;
                universal-ctags) echo "ctags" ;;
                build-essential) echo "gcc make" ;;
                *) echo "$dep" ;;
            esac ;;
        pacman)
            case "$dep" in
                libsqlite3-dev) echo "sqlite" ;;
                libcurl4-openssl-dev) echo "curl" ;;
                libpam0g-dev) echo "pam" ;;
                libsecret-1-dev) echo "libsecret" ;;
                build-essential) echo "base-devel" ;;
                *) echo "$dep" ;;
            esac ;;
        *) echo "$dep" ;;
    esac
}
```

### 3. Refactor setup.sh

Replace the hardcoded apt-get calls with:

```bash
source "$(dirname "$0")/distro-detect.sh"
detect_pkg_manager

$PKG_UPDATE
$PKG_INSTALL $(pkg_name gcc) $(pkg_name make) $(pkg_name libsqlite3-dev) \
    $(pkg_name libcurl4-openssl-dev) $(pkg_name git) $(pkg_name universal-ctags)
```

### 4. Refactor install.sh

Replace the three `apt-get install` calls (lines 51, 57) and the error message (line 43) with distro-aware equivalents using the same `distro-detect.sh`.

### 5. Makefile: distro-aware tool hints

Apply the same `PKG_HINT` pattern described in the Arch proposal to the `cppcheck` and `clang-tidy` targets. Fedora mapping: `dnf install cppcheck`, `dnf install clang-tools-extra`.

### 6. Agent policy: confirm dnf is already in the list

`agent_policy.c:538` already includes `dnf` in `pkg_mgrs[]`. Add `yum` for older RHEL systems that may not have `dnf`.

### 7. RPM spec file

Create `dist/aimee.spec`:

```spec
Name:           aimee
Version:        %{version}
Release:        1%{?dist}
Summary:        AI-powered development memory and agent framework

License:        Proprietary
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc make sqlite-devel openssl-devel libcurl-devel
Requires:       sqlite libcurl openssl git
Recommends:     universal-ctags pam libsecret

%description
Aimee provides tiered memory, code indexing, agent delegation, and
workflow automation for AI coding tools.

%build
cd src && make all server

%install
cd src && make install PREFIX=%{buildroot}/usr

%files
/usr/bin/aimee
/usr/bin/aimee-server
```

### 8. Documentation

Add a "Fedora / RHEL / AlmaLinux / Rocky Linux" section to `README.md`:

| Debian/Ubuntu package | Fedora/RHEL package |
|---|---|
| `build-essential` | `gcc make` (or `@development-tools` group) |
| `libsqlite3-dev` | `sqlite-devel` |
| `libcurl4-openssl-dev` | `libcurl-devel` |
| `libpam0g-dev` | `pam-devel` |
| `libsecret-1-dev` | `libsecret-devel` |
| `universal-ctags` | `ctags` |
| `cppcheck` | `cppcheck` |
| `clang-tidy` | `clang-tools-extra` |

### 9. CI: Fedora container test job

Add a job that builds and runs tests inside `fedora:latest`:

```yaml
fedora-test:
  runs-on: ubuntu-latest
  container: fedora:latest
  steps:
    - uses: actions/checkout@v4
    - run: dnf install -y gcc make sqlite-devel openssl-devel libcurl-devel git
    - run: cd src && make all server
    - run: cd src && make unit-tests
```

Optionally add an `almalinux:9` job for RHEL compatibility.

### Changes

| File | Change |
|------|--------|
| `src/guardrails.c` | Add `dnf install/remove/upgrade/update`, `yum install/remove/upgrade/update`, `rpm -i/-e/-U` to `pkg_cmds[]` |
| `src/agent_policy.c` | Add `yum` to `pkg_mgrs[]` array |
| `distro-detect.sh` (new) | Shared shell library for package manager detection and package name mapping |
| `setup.sh` | Source `distro-detect.sh`, replace hardcoded `apt-get` calls |
| `install.sh` | Source `distro-detect.sh`, replace hardcoded `apt-get` calls and error messages |
| `update.sh` | Source `distro-detect.sh`, replace hardcoded `apt-get` call for ctags |
| `src/Makefile` | Replace `apt install` hints with distro-aware `PKG_HINT` |
| `dist/aimee.spec` (new) | RPM spec file for rpmbuild/Copr |
| `README.md` | Add Fedora/RHEL build instructions and package-name table |
| `.github/workflows/ci.yml` | Add `fedora:latest` container test job |

## Acceptance Criteria

- [ ] `make && make unit-tests` passes in a clean `fedora:latest` container
- [ ] `make && make unit-tests` passes in a clean `almalinux:9` container
- [ ] `setup.sh` completes successfully on Fedora (installs correct packages via dnf)
- [ ] `install.sh` completes successfully on Fedora (builds, installs, auto-installs ctags via dnf)
- [ ] `aimee env` reports `package_manager=dnf` on Fedora
- [ ] `is_write_command("dnf install foo")` returns 1
- [ ] `is_write_command("yum remove bar")` returns 1
- [ ] `is_write_command("rpm -i package.rpm")` returns 1
- [ ] `rpmbuild -ba dist/aimee.spec` produces a working RPM on Fedora
- [ ] CI includes a Fedora build-and-test job

## Owner and Effort

- **Owner:** TBD
- **Effort:** M (guardrails and agent_policy are S; `distro-detect.sh` and refactoring three shell scripts is M; RPM spec and CI are S each)
- **Dependencies:** None (can land independently of the Arch proposal, though the `distro-detect.sh` library and Makefile `PKG_HINT` changes overlap — whichever lands first sets the pattern)

## Rollout and Rollback

- **Rollout:** Merge to main. RPM spec is opt-in (users run `rpmbuild` locally). CI job is additive. Shell script changes use detection, so Debian behavior is unchanged.
- **Rollback:** Revert the commit. No schema changes, no config migrations.
- **Blast radius:** Low. Guardrail changes add new patterns without altering existing ones. Shell script refactor could break existing Debian installs if `distro-detect.sh` has a bug — mitigated by keeping `apt` as a well-tested code path and testing both in CI.

## Test Plan

- [ ] Unit tests: add guardrails test cases for `dnf install foo`, `dnf remove bar`, `yum install baz`, `yum remove qux`, `rpm -i pkg.rpm`, `rpm -e pkg`, `rpm -Uvh pkg.rpm`
- [ ] Unit tests: add agent_policy test for `yum` detection in `pkg_mgrs[]`
- [ ] Integration tests: run full suite in Fedora container
- [ ] Integration tests: run full suite in AlmaLinux 9 container
- [ ] Manual verification: `rpmbuild -ba dist/aimee.spec`, install the RPM, run `aimee status`
- [ ] Manual verification: `setup.sh` on a fresh Fedora VM

## Operational Impact

- **Metrics:** None new (distro detection already in agent_policy; `distro_id` from `/etc/os-release` is covered by the Arch proposal).
- **Logging:** No new log lines.
- **Alerts:** CI failure on Fedora container is a new alert surface.
- **Disk/CPU/Memory:** Negligible. `distro-detect.sh` runs a few `command -v` checks at install time only.

## Priority

| Item | Priority | Effort | Impact |
|------|----------|--------|--------|
| Guardrails for dnf/yum/rpm | P1 | S | Safety gap — agent can run unguarded package installs on Fedora |
| `distro-detect.sh` + refactor setup.sh/install.sh | P1 | M | Aimee cannot be installed on Fedora without this |
| RPM spec file | P2 | S | Enables native packaging for Fedora/RHEL users |
| CI Fedora container | P2 | S | Prevents regressions from Fedora-specific lib versions |
| Documentation | P3 | S | Quality-of-life for Fedora users building from source |

## Trade-offs

**Why a shared `distro-detect.sh` instead of separate install scripts per distro?** The install logic is 90% identical across distros — only package names and the install command differ. Separate scripts would duplicate the build-check, binary-install, database-init, provider-selection, and hook-configuration logic. A mapping function keeps one code path with distro-specific package names.

**Why not also handle SUSE/zypper?** Scope. The `distro-detect.sh` pattern makes it trivial to add later (one more case branch), but testing and documenting SUSE is separate effort. Fedora/RHEL is the priority because it's the second-largest server distro family after Debian.

**Why keep `yum` support when Fedora 41+ only has `dnf`?** RHEL 8 and CentOS Stream 8 still use `yum` as the default command (though it's a `dnf` wrapper). RHEL 7 is EOL but still deployed in enterprise. Detection order is `dnf` first, `yum` as fallback, so modern systems always hit `dnf`.

**Overlap with the Arch proposal:** Both proposals introduce distro-aware tooling hints in the Makefile and `/etc/os-release` parsing. Whichever lands first sets the pattern; the second adapts. The `distro-detect.sh` library proposed here could also serve the Arch proposal's needs, so coordinating the two is recommended.
