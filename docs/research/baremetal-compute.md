# Research: Bare Metal Compute Guide

> **Provenance.** Captured 2026-06-26 from a user-supplied hands-on guide
> ("Bare Metal Compute Guide: Launching and Running Workloads on macOS, Linux, and
> Windows"), whose command syntax is grounded in official OS / tool docs
> (Apple, systemd/freedesktop, Microsoft Learn, Docker, Podman, NVIDIA; primary-
> source links are inline below).
>
> **Status:** reference / background. It is the cited basis for the command shapes
> the **bare-metal cloud-burst provisioner** uses — `BareMetalCliControlPlane`
> (`src/core/cloud/bare_metal_cli_control_plane.cpp`); see
> [bare-metal-provisioner.md](../cloud/bare-metal-provisioner.md). zetta's burst backend
> uses only the **local container** slice (§7 Containers on bare metal — `docker` /
> `podman run -d` / `ps` / `rm -f`, the cross-OS synchronous-launch mechanism — and
> §8 `--gpus all` for local GPUs); the rest (processes, systemd/launchd/Windows
> services, cron/timers/Task Scheduler, single-machine parallelism, monitoring) is
> background for choosing primitives and for a possible future raw-process variant.
> Bare metal is the local host, not a remote cloud/cluster. Examples are
> illustrative — replace names, images, and paths with values that match your host.

---

# Bare Metal Compute Guide: Launching and Running Workloads on macOS, Linux, and Windows

This guide is a practical, command-first handbook for running compute workloads directly on local or physical machines across macOS, Linux, and Windows. Unlike cloud CLI guides that provision remote infrastructure, the “compute resource” here is the machine you are sitting at or administering: its CPU cores, memory, disks, GPUs, OS scheduler, process table, service manager, and local container runtime.

## 1. Introduction & mental model

“Bare metal” in this guide means running workloads directly on an operating system rather than asking a cloud control plane, hypervisor, or Kubernetes cluster to place work somewhere else. In practice, the unit of work is usually a process, a long-lived service, a scheduled job, a local container, or a GPU/HPC program.

Choose bare metal when you need low latency to local hardware, direct GPU access, offline operation, simple single-machine automation, or maximum control over the host OS. Choose cloud or Kubernetes when you need elastic horizontal scale, managed high availability, infrastructure APIs, managed identity, multi-host scheduling, or operational isolation beyond one machine.

The cross-OS concepts are the same even though the tools differ:

| Concept | macOS | Linux | Windows |
|---|---|---|---|
| Interactive shell | zsh/bash | bash/zsh/fish | PowerShell/cmd |
| Package manager | Homebrew | apt/dnf/pacman | winget/Chocolatey |
| Short workload | foreground or background process | foreground or background process | process or PowerShell job |
| Always-on workload | launchd agent/daemon | systemd service | Windows Service or scheduled task |
| Scheduled workload | cron or launchd calendar trigger | cron, at, systemd timer | Task Scheduler |
| Privilege escalation | `sudo` | `sudo` | Run as Administrator |
| Local container runtime | Docker Desktop, Podman | Docker Engine/Desktop, Podman | Docker Desktop, WSL2-backed containers |

A shell starts commands and passes environment variables, arguments, standard input, standard output, and standard error to processes. A process has a process ID, exit code, ownership, priority, resource usage, and signals or control messages. A service or daemon is a process supervised by the OS so it can start at boot/login, restart after failure, and write logs to a central logging facility. A scheduler starts commands later or repeatedly. A package manager installs and updates tools so scripts can be reproducible.

## 2. Getting started per OS: shell and package manager

### macOS

Modern macOS uses zsh as the default interactive shell, while bash remains available for many POSIX-style scripts. Install Apple’s Command Line Tools before compiling software or using developer utilities; Apple documents `xcode-select --install` as a way to install the Command Line Tools package, and `xcode-select --print-path` shows the active developer directory ([Apple Developer](https://developer.apple.com/library/archive/technotes/tn2339/_index.html), [Apple Xcode documentation](https://developer.apple.com/documentation/xcode/configuring-command-line-tools-settings?changes=_8)).

```bash
# Show your shell.
echo "$SHELL"

# Install Apple Command Line Tools.
xcode-select --install

# Confirm the active developer tools path.
xcode-select --print-path
```

Homebrew is the de facto package manager for macOS developer tooling, and its manual documents `brew install <formula>` for installing command-line packages ([Homebrew Documentation](https://docs.brew.sh/Manpage)).

```bash
# Install common local-compute tools.
brew install htop jq python node go git

# Install a GUI app with a cask.
brew install --cask docker

# Update package metadata and upgrade installed packages.
brew update
brew upgrade
```

Use `sudo` only when the operation genuinely needs elevated privileges, such as writing to `/Library`, managing a LaunchDaemon, or binding privileged ports. Prefer user-level tools and user-level LaunchAgents when possible.

### Linux

Linux distributions typically ship bash and a distribution-specific package manager. Ubuntu documents APT as the recommended command-line way to install, upgrade, and remove Debian packages, Fedora documents DNF for search/install/remove workflows, and Arch documents pacman for synchronizing repositories and installing packages ([Ubuntu Server documentation](https://ubuntu.com/server/docs/how-to/software/package-management/), [Fedora Docs](https://docs.fedoraproject.org/en-US/quick-docs/dnf/), [ArchWiki](https://wiki.archlinux.org/title/Pacman)).

```bash
# Debian/Ubuntu.
sudo apt update
sudo apt install -y htop jq python3 python3-venv build-essential git

# Fedora/RHEL-family.
sudo dnf install -y htop jq python3 gcc make git

# Arch Linux.
sudo pacman -Syu --needed htop jq python base-devel git
```

Use `sudo` for package installation, system service changes, privileged ports, and host-wide configuration. Run workloads as an unprivileged user by default, then grant only the specific permissions needed.

### Windows

PowerShell is the preferred automation shell for modern Windows, while `cmd.exe` remains useful for legacy command-line tools. Microsoft documents WinGet as the Windows Package Manager client for discovering, installing, upgrading, removing, and configuring applications, and its `install` command accepts a query or exact package identifier ([Microsoft Learn: WinGet](https://learn.microsoft.com/en-us/windows/package-manager/winget/), [Microsoft Learn: winget install](https://learn.microsoft.com/en-us/windows/package-manager/winget/install)).

```powershell
# Show PowerShell version.
$PSVersionTable

# Search and install packages with winget.
winget search Python.Python.3
winget install --id Python.Python.3.12 --source winget
winget install --id Git.Git --source winget
winget install --id Docker.DockerDesktop --source winget
```

Chocolatey is another Windows package manager, and its install command is `choco install <pkg>` ([Chocolatey Docs](https://docs.chocolatey.org/en-us/choco/commands/install/)).

```powershell
# Run in an elevated PowerShell if Chocolatey is installed.
choco install -y git python jq
```

Open PowerShell as Administrator for machine-wide installs, service creation, firewall changes, and stopping processes owned by other users. Microsoft documents WSL as a way to run a GNU/Linux environment directly on Windows without a traditional VM or dual boot, and `wsl --install` enables WSL2 and installs a default Linux distribution on supported Windows versions ([Microsoft Learn: WSL](https://learn.microsoft.com/en-us/windows/wsl/), [Microsoft Learn: WSL setup](https://learn.microsoft.com/en-us/windows/wsl/setup/environment)).

```powershell
# Install WSL2 with the default Ubuntu distribution.
wsl --install

# List installed distros and status.
wsl --list --verbose

# Start a Linux shell from Windows.
wsl
```

## 3. Launching and managing processes: the core workload

### macOS/Linux: POSIX process basics

Run a program in the foreground when you want the terminal attached to its input and output. The shell returns an exit code for the last foreground command, with `0` conventionally meaning success and non-zero meaning failure.

```bash
python3 - <<'PY'
print("hello from a local workload")
PY

echo "exit code was $?"
```

Use environment variables to configure a process without hard-coding settings in the command. Redirection captures stdout and stderr to files.

```bash
mkdir -p logs
PORT=8080 ENV=dev python3 -m http.server "$PORT" >logs/server.out 2>logs/server.err
```

Add `&` to start a job in the background, then use shell job-control commands to inspect, foreground, background, or wait for it. Bash documents `jobs`, `fg`, and `bg` as job-control builtins, with `bg` resuming a suspended job in the background and `fg` resuming a job in the foreground ([GNU Bash Manual](https://www.gnu.org/software/bash/manual/html_node/Job-Control-Builtins.html)).

```bash
# Start in background.
python3 -m http.server 8080 >logs/server.log 2>&1 &
SERVER_PID=$!
echo "server pid=$SERVER_PID"

# List shell-managed jobs.
jobs -l

# Bring the most recent job to the foreground.
fg %+

# Suspend a foreground job with Ctrl-Z, then resume it in the background.
bg %+
```

Use `nohup` when you want a process to ignore hangup signals and continue after the terminal exits; GNU Coreutils documents that `nohup` does not automatically put a command in the background, so you still add `&` when needed ([GNU Coreutils: nohup](http://www.gnu.org/s/coreutils/manual/html_node/nohup-invocation.html)).

```bash
mkdir -p logs
nohup python3 long_job.py >logs/long_job.log 2>&1 &
echo $! > long_job.pid
```

Use `ps`, `pgrep`, and `pkill` to find processes; the Linux `ps` manual describes `ps` as reporting a snapshot of active processes, and the `pgrep` manual describes `pgrep`/`pkill` as selecting or signaling processes by name and attributes ([man7 ps](https://man7.org/linux/man-pages/man1/ps.1.html), [man7 pgrep](https://man7.org/linux/man-pages/man1/pgrep.1.html)).

```bash
# Show your processes with CPU/memory columns.
ps -u "$USER" -o pid,ppid,stat,%cpu,%mem,etime,command

# Find processes by exact command name.
pgrep -a -x python3

# Stop a process gracefully with SIGTERM.
kill "$SERVER_PID"

# Force-stop only if graceful termination fails.
kill -KILL "$SERVER_PID"

# Stop matching processes by pattern; review with pgrep first.
pgrep -a -f 'python3 -m http.server'
pkill -TERM -f 'python3 -m http.server'
```

Use `nice` to start a lower-priority CPU-bound job; GNU Coreutils documents that `nice` runs a command with modified niceness and that niceness affects how favorably the process is scheduled ([GNU Coreutils: nice](https://www.gnu.org/s/coreutils/manual/html_node/nice-invocation.html)).

```bash
# Start lower priority: positive niceness means less CPU preference.
nice -n 10 python3 cpu_heavy_job.py >logs/cpu_heavy.log 2>&1 &

# Change priority of an existing process.
renice 10 -p "$SERVER_PID"
```

### Windows: processes and PowerShell jobs

Use `Start-Process` when you want to launch an executable with arguments, a working directory, output redirection, or a separate window; Microsoft documents `Start-Process` as starting one or more local processes and supports parameters such as `-FilePath`, `-ArgumentList`, `-WorkingDirectory`, `-RedirectStandardOutput`, `-RedirectStandardError`, `-PassThru`, and `-Wait` ([Microsoft Learn: Start-Process](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.management/start-process?view=powershell-7.6)).

```powershell
New-Item -ItemType Directory -Force logs | Out-Null

$p = Start-Process -FilePath "python" `
  -ArgumentList "-m", "http.server", "8080" `
  -WorkingDirectory $PWD `
  -RedirectStandardOutput "logs\server.out" `
  -RedirectStandardError "logs\server.err" `
  -PassThru

"server pid=$($p.Id)"
```

Use PowerShell background jobs for asynchronous shell work; Microsoft documents that `Start-Job` starts a background job, `Get-Job` lists job objects, and `Receive-Job` retrieves results after or while the job runs ([Microsoft Learn: Start-Job](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/start-job?view=powershell-7.6), [Microsoft Learn: Get-Job](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/get-job?view=powershell-7.6)).

```powershell
$job = Start-Job -Name LocalBatch -ScriptBlock {
  1..5 | ForEach-Object {
    Start-Sleep -Seconds 1
    "item $_ done"
  }
}

Get-Job
Receive-Job -Name LocalBatch -Wait -AutoRemoveJob
```

Use `Get-Process` and `Stop-Process` for process inspection and termination; Microsoft documents that `Get-Process` returns local process objects and that `Stop-Process` stops processes by name, PID, or process object ([Microsoft Learn: Get-Process](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.management/get-process?view=powershell-7.6), [Microsoft Learn: Stop-Process](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.management/stop-process?view=powershell-7.6)).

```powershell
# List heavy local processes.
Get-Process | Sort-Object CPU -Descending | Select-Object -First 10 Id,ProcessName,CPU,WS

# Stop by PID.
Stop-Process -Id $p.Id

# Stop by name with confirmation.
Stop-Process -Name notepad -Confirm
```

Use legacy `tasklist` and `taskkill` when working in cmd scripts or when you need classic Windows command syntax; Microsoft documents `taskkill /pid <processID>` and `/im <imagename>`, with `/f` for force and `/t` for child processes ([Microsoft Learn: taskkill](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/taskkill)).

```cmd
REM List processes.
tasklist

REM Gracefully end by image name where possible.
taskkill /IM python.exe

REM Force-kill a process tree by PID.
taskkill /PID 12345 /F /T
```

PowerShell can adjust priority through the process object’s `PriorityClass` property after launch.

```powershell
$p = Start-Process python -ArgumentList "cpu_heavy_job.py" -PassThru
$p.PriorityClass = 'BelowNormal'
Get-Process -Id $p.Id | Select-Object Id,ProcessName,PriorityClass
```

## 4. Running workloads as long-lived services/daemons

### Linux: systemd service

Systemd is the standard service manager on many Linux distributions, and `systemctl` controls the systemd system and service manager ([freedesktop.org systemctl](https://www.freedesktop.org/software/systemd/man/systemctl.html)). A service unit describes how to start, stop, restart, and supervise a long-lived workload; systemd service units use sections such as `[Unit]`, `[Service]`, and `[Install]` ([freedesktop.org systemd.service](https://www.freedesktop.org/software/systemd/man/systemd.service.html)).

Create a simple Python HTTP service:

```bash
sudo tee /etc/systemd/system/local-web.service >/dev/null <<'EOF'
[Unit]
Description=Local Python HTTP workload
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/srv/local-web
ExecStart=/usr/bin/python3 -m http.server 8080
Restart=on-failure
RestartSec=5
User=www-data
Group=www-data
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
EOF
```

Prepare the working directory and start the service:

```bash
sudo mkdir -p /srv/local-web
printf 'hello from systemd\n' | sudo tee /srv/local-web/index.html >/dev/null
sudo chown -R www-data:www-data /srv/local-web

sudo systemctl daemon-reload
sudo systemctl enable --now local-web.service
sudo systemctl status local-web.service
```

Use `journalctl` for logs because it prints entries from the systemd journal ([freedesktop.org journalctl](https://www.freedesktop.org/software/systemd/man/journalctl.html)).

```bash
# Follow logs for this unit.
sudo journalctl -u local-web.service -f

# Stop, restart, disable, and remove.
sudo systemctl stop local-web.service
sudo systemctl restart local-web.service
sudo systemctl disable local-web.service
```

### macOS: launchd agents and daemons

Apple recommends launchd-compliant daemons and agents for macOS background processes, and distinguishes per-user agents from system-level daemons ([Apple Developer: Creating Launch Daemons and Agents](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html)). `launchctl` interfaces with launchd to manage daemons, agents, and services; modern examples often use `bootstrap`, `bootout`, `kickstart`, and `print` with a domain such as `gui/<uid>` for the current user ([launchctl man page mirror](https://keith.github.io/xcode-man-pages/launchctl.1.html)).

Create a per-user LaunchAgent:

```bash
mkdir -p "$HOME/Library/LaunchAgents" "$HOME/local-web" "$HOME/Library/Logs"
printf 'hello from launchd\n' > "$HOME/local-web/index.html"

cat > "$HOME/Library/LaunchAgents/com.example.local-web.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.example.local-web</string>

  <key>ProgramArguments</key>
  <array>
    <string>/usr/bin/python3</string>
    <string>-m</string>
    <string>http.server</string>
    <string>8080</string>
  </array>

  <key>WorkingDirectory</key>
  <string>__HOME__/local-web</string>

  <key>RunAtLoad</key>
  <true/>

  <key>KeepAlive</key>
  <dict>
    <key>SuccessfulExit</key>
    <false/>
  </dict>

  <key>StandardOutPath</key>
  <string>__HOME__/Library/Logs/local-web.out.log</string>
  <key>StandardErrorPath</key>
  <string>__HOME__/Library/Logs/local-web.err.log</string>
</dict>
</plist>
EOF

# Replace placeholder with your actual home directory.
perl -pi -e "s#__HOME__#$ENV{HOME}#g" "$HOME/Library/LaunchAgents/com.example.local-web.plist"
```

Load and manage it:

```bash
UID_NOW=$(id -u)
PLIST="$HOME/Library/LaunchAgents/com.example.local-web.plist"

launchctl bootstrap "gui/$UID_NOW" "$PLIST"
launchctl kickstart -k "gui/$UID_NOW/com.example.local-web"
launchctl print "gui/$UID_NOW/com.example.local-web"

tail -f "$HOME/Library/Logs/local-web.out.log" "$HOME/Library/Logs/local-web.err.log"

launchctl bootout "gui/$UID_NOW/com.example.local-web"
```

Use `/Library/LaunchDaemons` only for system-wide daemons that need to run before login or under a dedicated service account. LaunchDaemons normally require root ownership and stricter file permissions, so prefer LaunchAgents for user-level compute.

### Windows: Windows Services and service-like scheduled tasks

Windows Services are controlled through the Service Control Manager, and Microsoft documents `sc.exe create` as creating service entries in the registry and Service Control Manager database ([Microsoft Learn: sc.exe create](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/sc-create)). PowerShell also provides `New-Service`, which creates a Windows service and requires an executable binary path ([Microsoft Learn: New-Service](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.management/new-service?view=powershell-7.6)).

Native Windows services should be built to implement the Windows service protocol. For arbitrary console executables, use a wrapper such as NSSM or use Task Scheduler “at startup” instead of assuming any process can be a well-behaved service.

```powershell
# Example for a real service-capable executable; run as Administrator.
New-Service -Name "LocalWorker" `
  -BinaryPathName "C:\Tools\LocalWorker\LocalWorker.exe --port 8080" `
  -DisplayName "Local Worker" `
  -Description "Example local compute service" `
  -StartupType Automatic

Start-Service LocalWorker
Get-Service LocalWorker
Stop-Service LocalWorker
```

The equivalent `sc.exe` form is useful in cmd scripts, and note the required space after `binPath=` and `start=` in common `sc.exe` syntax:

```cmd
REM Run as Administrator.
sc.exe create LocalWorker binPath= "C:\Tools\LocalWorker\LocalWorker.exe --port 8080" start= auto DisplayName= "Local Worker"
sc.exe start LocalWorker
sc.exe query LocalWorker
sc.exe stop LocalWorker
sc.exe delete LocalWorker
```

PowerShell service cmdlets list and change service state; Microsoft documents `Get-Service`, `Start-Service`, and `Stop-Service` for listing and controlling services ([Microsoft Learn: Managing services](https://learn.microsoft.com/en-us/powershell/scripting/samples/managing-services?view=powershell-7.6), [Microsoft Learn: Stop-Service](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.management/stop-service?view=powershell-7.6)).

```powershell
Get-Service -Name LocalWorker
Start-Service -Name LocalWorker
Stop-Service -Name LocalWorker
```

Task Scheduler is often the most practical “service-like” option for arbitrary scripts because it can run at startup, at logon, on a schedule, or whether a user is logged in.

```powershell
# Run as Administrator for machine-wide startup behavior.
$Action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -File C:\Tools\worker.ps1"
$Trigger = New-ScheduledTaskTrigger -AtStartup
Register-ScheduledTask -TaskName "LocalWorkerAtStartup" -Action $Action -Trigger $Trigger -RunLevel Highest -Description "Run local worker at startup"
Start-ScheduledTask -TaskName "LocalWorkerAtStartup"
```

## 5. Scheduled & batch jobs

### Linux/macOS: cron

Cron starts commands at specified times, and the Linux crontab manual describes a crontab file as instructions to “run this command at this time on this date” ([man7 crontab](https://man7.org/linux/man-pages/man5/crontab.5.html)).

```bash
# Edit your user crontab.
crontab -e
```

Add a job like this:

```cron
# minute hour day-of-month month day-of-week command
# Run every day at 02:30 and append logs.
30 2 * * * /usr/bin/python3 /home/you/jobs/nightly.py >>/home/you/logs/nightly.log 2>&1

# Run every 5 minutes.
*/5 * * * * /usr/local/bin/healthcheck.sh >>/tmp/healthcheck.log 2>&1
```

List and remove user crontabs carefully:

```bash
crontab -l
crontab -r   # removes the current user's crontab; use with care
```

Use `at` for one-off deferred jobs; POSIX documents `at` as reading commands and executing them at a later time ([man7 at](https://man7.org/linux/man-pages/man1/at.1p.html)).

```bash
echo 'python3 /home/you/jobs/one_off.py >>/home/you/logs/one_off.log 2>&1' | at 23:30
atq
```

### Linux: systemd timers

Systemd timers are a native Linux alternative to cron, and `systemd-run` can create transient timer-triggered services with options such as `--on-calendar=` ([freedesktop.org systemd-run](https://www.freedesktop.org/software/systemd/man/systemd-run.html)).

```bash
# One-off transient job 10 minutes from now.
systemd-run --user --on-active=10m --unit=local-oneoff /usr/bin/python3 "$HOME/jobs/one_off.py"

# Calendar-style transient job.
systemd-run --user --on-calendar='*-*-* 02:30:00' --unit=nightly-job /usr/bin/python3 "$HOME/jobs/nightly.py"

systemctl --user list-timers
journalctl --user -u nightly-job.service
```

For production recurring jobs, create `.service` and `.timer` files in `/etc/systemd/system` or `~/.config/systemd/user` and manage them with `systemctl enable --now <timer>`.

### macOS: launchd calendar triggers

Launchd can start jobs based on time as well as at load. Apple documents launchd as supporting timed intervals and background job management for daemons and agents ([Apple Developer: Creating Launch Daemons and Agents](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html)).

```xml
<key>StartCalendarInterval</key>
<dict>
  <key>Hour</key>
  <integer>2</integer>
  <key>Minute</key>
  <integer>30</integer>
</dict>
```

Place that key in a LaunchAgent plist alongside `ProgramArguments`, then load it with `launchctl bootstrap gui/$(id -u) path/to/job.plist`.

### Windows: Task Scheduler

The `schtasks /create` command creates scheduled tasks and accepts `/sc` for schedule type, `/tn` for task name, and `/tr` for the command to run ([Microsoft Learn: schtasks create](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/schtasks-create)).

```cmd
REM Daily at 02:30.
schtasks /create /tn "NightlyLocalJob" /sc DAILY /st 02:30 /tr "powershell.exe -NoProfile -File C:\Jobs\nightly.ps1" /f

REM Run once at 23:30 today.
schtasks /create /tn "OneOffLocalJob" /sc ONCE /st 23:30 /tr "powershell.exe -NoProfile -File C:\Jobs\oneoff.ps1" /f

schtasks /query /tn "NightlyLocalJob" /v /fo LIST
schtasks /run /tn "NightlyLocalJob"
schtasks /delete /tn "NightlyLocalJob" /f
```

PowerShell’s ScheduledTasks module registers local scheduled task definitions, and `New-ScheduledTaskTrigger` can create time-based triggers such as once, daily, weekly, at startup, or at logon ([Microsoft Learn: Register-ScheduledTask](https://learn.microsoft.com/en-us/powershell/module/scheduledtasks/register-scheduledtask?view=windowsserver2025-ps), [Microsoft Learn: New-ScheduledTaskTrigger](https://learn.microsoft.com/en-us/powershell/module/scheduledtasks/new-scheduledtasktrigger?view=windowsserver2025-ps)).

```powershell
$Action  = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -File C:\Jobs\nightly.ps1"
$Trigger = New-ScheduledTaskTrigger -Daily -At 2:30am
Register-ScheduledTask -TaskName "NightlyLocalJob" -Action $Action -Trigger $Trigger -Description "Nightly local batch workload" -Force
```

## 6. Parallelism & scaling on one machine

Scaling on one machine means using more CPU cores, more GPU devices, more disk throughput, or more independent processes. It does not create the fault tolerance or horizontal capacity of cloud fleets or Kubernetes clusters.

### macOS/Linux

Use shell background jobs for simple fan-out:

```bash
mkdir -p logs
for i in 1 2 3 4; do
  python3 worker.py --shard "$i" >"logs/worker-$i.log" 2>&1 &
done
wait
```

Use `xargs -P` for bounded parallelism; GNU Findutils documents `-P`/`--max-procs` as running up to a specified number of processes at a time ([GNU Findutils](https://www.gnu.org/software/findutils/manual/html_node/find_html/Controlling-Parallelism.html)).

```bash
# Process files four at a time; -n 1 passes one item per command.
find data -name '*.json' -print0 |
  xargs -0 -n 1 -P 4 python3 process_file.py
```

GNU Parallel is a richer tool for command fan-out and supports the `parallel [options] [command] ::: arguments` style ([GNU Parallel Tutorial](https://www.gnu.org/software/parallel/parallel_tutorial.html)).

```bash
# Install first: brew install parallel, sudo apt install parallel, or distro equivalent.
parallel -j 4 'python3 process_file.py {} > logs/{/.}.log 2>&1' ::: data/*.json
```

On Linux, CPU affinity tools such as `taskset` can pin a process to specific CPUs when cache locality or noisy neighbors matter. Use affinity only after measuring because it can reduce scheduler flexibility.

```bash
# Linux only: run on CPUs 0 and 1.
taskset -c 0,1 python3 cpu_heavy_job.py
```

### Windows

PowerShell 7 supports parallel pipeline execution through `ForEach-Object -Parallel`, with `-ThrottleLimit` controlling concurrency.

```powershell
# Requires PowerShell 7+.
1..8 | ForEach-Object -Parallel {
  python process_shard.py --shard $_
} -ThrottleLimit 4
```

PowerShell jobs are useful when each unit of work is a script block and you want job objects, status, and result retrieval ([Microsoft Learn: Start-Job](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/start-job?view=powershell-7.6)).

```powershell
$jobs = 1..4 | ForEach-Object {
  Start-Job -Name "worker-$_" -ScriptBlock { param($Shard) python worker.py --shard $Shard } -ArgumentList $_
}
$jobs | Wait-Job | Receive-Job
$jobs | Remove-Job
```

If one machine is not enough, move up the stack: local containers make packaging portable, local Kubernetes helps practice orchestration, and cloud or cluster schedulers provide real horizontal scale.

## 7. Containers on bare metal

Containers on bare metal still run on the local machine; they package process filesystems and runtime settings while sharing the host kernel or a lightweight VM layer depending on OS and runtime. Docker documents `docker run` as creating and running a new container from an image, and Docker’s container guide notes that a container runs as an isolated process on a host ([Docker CLI reference](https://docs.docker.com/reference/cli/docker/container/run/), [Docker Docs: running containers](https://docs.docker.com/engine/containers/run/)).

```bash
# macOS/Linux shell; similar from PowerShell with adjusted paths.
docker run --rm hello-world

docker run --rm -it \
  -p 8080:80 \
  --name local-nginx \
  nginx:alpine
```

Key Docker flags: `--rm` deletes the container after exit, `-it` attaches an interactive terminal, `-p host:container` publishes a port, `-v host:container` mounts a volume, and `-d` runs detached in the background.

```bash
mkdir -p "$PWD/site"
echo 'hello from container' > "$PWD/site/index.html"

docker run -d --name web \
  -p 8080:80 \
  -v "$PWD/site:/usr/share/nginx/html:ro" \
  nginx:alpine

docker logs web
docker stop web
docker rm web
```

Docker Desktop provides Docker on macOS and Windows, while Docker Engine is the common Linux server install path; Docker documents Docker Desktop installation for Mac and Windows and Docker Engine installation for Linux ([Docker Desktop for Mac](https://docs.docker.com/desktop/setup/install/mac-install/), [Docker Desktop for Windows](https://docs.docker.com/desktop/setup/install/windows-install/), [Docker Engine install](https://docs.docker.com/engine/install/)).

Podman is a daemonless OCI container tool, and its documentation describes `podman run` as starting a process with its own filesystem, networking, and isolated process tree ([Podman docs](https://docs.podman.io), [podman-run](https://docs.podman.io/en/latest/markdown/podman-run.1.html)).

```bash
podman run --rm hello-world
podman run -d --name web -p 8080:80 nginx:alpine
podman logs web
podman stop web
podman rm web
```

Local single-node Kubernetes tools are the on-ramp to the kubectl guide. Kind runs local Kubernetes clusters using container “nodes,” and minikube starts a local Kubernetes cluster with `minikube start` ([kind quick start](https://kind.sigs.k8s.io/docs/user/quick-start/), [minikube start](https://minikube.sigs.k8s.io/docs/start/)).

```bash
# kind.
kind create cluster
kubectl get nodes
kind delete cluster

# minikube.
minikube start --driver=docker
kubectl get nodes
minikube delete
```

## 8. GPU / ML & HPC on bare metal

### Linux: NVIDIA, CUDA, and containers

For NVIDIA GPU workloads on Linux, install the correct NVIDIA driver and CUDA Toolkit for your distribution and GPU. NVIDIA’s Linux CUDA installation guide lists prerequisites such as a CUDA-capable GPU, supported Linux version, GCC/toolchain, and the CUDA Toolkit ([NVIDIA CUDA Installation Guide for Linux](https://docs.nvidia.com/cuda/cuda-installation-guide-linux/index.html)).

```bash
# Check GPU and driver visibility.
nvidia-smi

# Watch utilization every second.
watch -n 1 nvidia-smi
```

CUDA programs commonly compile with `nvcc`, while Python ML stacks often consume CUDA through framework wheels or Conda packages. Keep driver/toolkit compatibility in mind because NVIDIA documents minimum driver requirements and compatibility behavior for CUDA applications ([NVIDIA CUDA Compatibility](https://docs.nvidia.com/deploy/pdf/CUDA_Compatibility.pdf)).

```bash
# If CUDA toolkit is installed.
nvcc --version

# Python smoke test for a CUDA-enabled PyTorch install.
python3 - <<'PY'
import torch
print(torch.cuda.is_available())
print(torch.cuda.get_device_name(0) if torch.cuda.is_available() else "no cuda")
PY
```

NVIDIA Container Toolkit enables GPU-accelerated Docker containers, and NVIDIA documents Docker runtime integration and GPU enumeration for CUDA containers ([NVIDIA Container Toolkit install guide](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/1.10.0/install-guide.html), [NVIDIA Container Toolkit user guide](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/1.10.0/user-guide.html)).

```bash
# After installing NVIDIA Container Toolkit and configuring Docker.
docker run --rm --gpus all nvidia/cuda:12.5.0-base-ubuntu22.04 nvidia-smi
```

### Windows: NVIDIA, CUDA, and WSL2 GPU

On Windows, install the vendor GPU driver first, then CUDA if compiling or running native CUDA workloads. Use `nvidia-smi` from PowerShell or cmd to confirm the driver sees the GPU.

```powershell
nvidia-smi

# Repeat every second in PowerShell.
while ($true) { cls; nvidia-smi; Start-Sleep -Seconds 1 }
```

WSL2 is often the best path for Linux ML tooling on Windows because WSL runs a GNU/Linux environment directly on Windows and supports Linux command-line tools ([Microsoft Learn: WSL](https://learn.microsoft.com/en-us/windows/wsl/)).

```powershell
wsl --install
wsl
```

Inside WSL2, use Linux package and Python workflows, then check GPU visibility:

```bash
nvidia-smi
python3 - <<'PY'
import torch
print(torch.cuda.is_available())
PY
```

### macOS: Apple Silicon GPU, Metal, and MPS

Modern Macs do not support NVIDIA CUDA; Apple Silicon ML acceleration generally uses Apple’s Metal stack through framework integrations. PyTorch documents the `mps` device as mapping ML computation to Metal Performance Shaders on supported macOS devices ([PyTorch MPS backend](https://docs.pytorch.org/docs/stable/notes/mps.html)).

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip torch

python - <<'PY'
import torch
print("mps available:", torch.backends.mps.is_available())
if torch.backends.mps.is_available():
    x = torch.ones(5, device="mps")
    print((x * 2).cpu())
PY
```

For utilization, use Activity Monitor’s GPU History window on macOS, or command-line tools such as `powermetrics` where available and permitted.

```bash
# Requires sudo and may vary by macOS version.
sudo powermetrics --samplers gpu_power -i 1000
```

## 9. Monitoring, resource management & cleanup

### CPU and memory

On macOS and Linux, use `top` everywhere and `htop` where installed. On Linux, `ps` gives snapshots and `top` gives repeated updates; the `ps` manual explicitly points users to `top` for repetitive updates ([man7 ps](https://man7.org/linux/man-pages/man1/ps.1.html)).

```bash
# macOS/Linux.
top
htop
ps -eo pid,ppid,user,stat,%cpu,%mem,etime,command --sort=-%cpu | head
```

On macOS, Activity Monitor is the GUI view and `vm_stat` reports virtual memory counters.

```bash
vm_stat
```

On Linux, `free`, `df`, and `du` are standard first checks for memory and disk pressure.

```bash
free -h
df -h
du -sh ./* 2>/dev/null | sort -h
```

On Windows, use Task Manager for the GUI and `Get-Process` or `Get-Counter` for shell monitoring. Microsoft documents `Get-Counter` as retrieving Windows performance counter data with parameters such as `-Counter`, `-SampleInterval`, `-MaxSamples`, and `-Continuous` ([Microsoft Learn: Get-Counter](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.diagnostics/get-counter?view=powershell-7.6)).

```powershell
Get-Process | Sort-Object CPU -Descending | Select-Object -First 10 Id,ProcessName,CPU,WS

Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 5
Get-Counter '\Memory\Available MBytes' -SampleInterval 1 -MaxSamples 5
```

### Finding and stopping runaway workloads

First identify the process, then request a clean stop, then force only if needed. On POSIX systems, try `TERM` before `KILL`; on Windows, try `Stop-Process` without `-Force` before using forceful termination.

```bash
# POSIX.
pgrep -a -f 'worker.py'
pkill -TERM -f 'worker.py'
sleep 5
pkill -KILL -f 'worker.py'
```

```powershell
# Windows.
Get-Process python | Select-Object Id,ProcessName,CPU,WS
Stop-Process -Name python
Stop-Process -Name python -Force
```

### Resource limits and cleanup

Use shell limits on Linux/macOS to prevent a job from exhausting the host; Bash documents `ulimit` as controlling resources available to processes started by the shell where the OS supports it ([GNU Bash Manual: ulimit](https://ftp.gnu.org/old-gnu/Manuals/bash/html_chapter/bashref_4.html)).

```bash
# Show limits.
ulimit -a

# Limit CPU seconds and max open files for child processes from this shell.
ulimit -t 3600
ulimit -n 1024
python3 bounded_job.py
```

Clean disk usage from logs, caches, build artifacts, and stopped containers:

```bash
# macOS/Linux logs and build artifacts; review before deleting.
find logs -type f -mtime +14 -print
find logs -type f -mtime +14 -delete

# Docker cleanup; review with docker system df first.
docker system df
docker container prune
docker image prune
```

```powershell
# Windows: review large files and old logs.
Get-ChildItem C:\Logs -Recurse -File |
  Sort-Object Length -Descending |
  Select-Object -First 20 FullName,Length

Get-ChildItem C:\Logs -Recurse -File |
  Where-Object LastWriteTime -lt (Get-Date).AddDays(-14) |
  Remove-Item -WhatIf
```

### Best practices

Use least privilege: run compute as a normal user, reserve `sudo` or Administrator for installation and system configuration, and prefer dedicated service accounts for long-lived services. Capture logs to files or the OS journal from day one so failures have evidence. Keep workloads reproducible by checking scripts, service files, environment files, and package manifests into version control. Pin or record package versions for production jobs. Keep OS packages, GPU drivers, container runtimes, and language dependencies updated on a maintenance schedule. Write clean shutdown handlers so services can respond to `SIGTERM`, service stop, or Task Scheduler stop events without corrupting data.

## Cross-OS quick-reference command table

| Task | macOS | Linux | Windows |
|---|---|---|---|
| Install package | `brew install htop` | `sudo apt install htop` / `sudo dnf install htop` / `sudo pacman -S htop` | `winget install --id Git.Git` / `choco install git` |
| Install developer tools | `xcode-select --install` | `sudo apt install build-essential` or distro equivalent | `winget install --id Microsoft.VisualStudio.2022.BuildTools` |
| Run foreground workload | `python3 app.py` | `python3 app.py` | `python app.py` |
| Run in background | `python3 app.py >app.log 2>&1 &` | `python3 app.py >app.log 2>&1 &` | `Start-Process python -ArgumentList 'app.py' -RedirectStandardOutput app.log -PassThru` |
| Keep running after logout | `nohup python3 app.py >app.log 2>&1 &` | `nohup python3 app.py >app.log 2>&1 &` | Use Task Scheduler or a service wrapper |
| List shell jobs | `jobs -l` | `jobs -l` | `Get-Job` |
| List processes | `ps aux` / `top` / `htop` | `ps -ef` / `top` / `htop` | `Get-Process` / `tasklist` |
| Find process by name | `pgrep -a -f app.py` | `pgrep -a -f app.py` | `Get-Process python` |
| Stop process | `kill <pid>` / `pkill -TERM -f app.py` | `kill <pid>` / `pkill -TERM -f app.py` | `Stop-Process -Id <pid>` / `taskkill /PID <pid>` |
| Force stop | `kill -KILL <pid>` | `kill -KILL <pid>` | `Stop-Process -Id <pid> -Force` / `taskkill /PID <pid> /F /T` |
| Lower priority | `nice -n 10 command` | `nice -n 10 command` | `$p.PriorityClass = 'BelowNormal'` |
| Create service | LaunchAgent plist + `launchctl bootstrap gui/$(id -u) file.plist` | systemd unit + `sudo systemctl enable --now name.service` | `New-Service ...` or `sc.exe create ...` |
| View service logs | `tail -f ~/Library/Logs/*.log` or Console.app | `journalctl -u name.service -f` | Event Viewer, app logs, or redirected task output |
| Schedule recurring job | launchd `StartCalendarInterval` or `crontab -e` | `crontab -e` or systemd timer | `schtasks /create ...` or `Register-ScheduledTask` |
| One-off delayed job | `echo 'cmd' \\| at 23:30` if `at` is available | `echo 'cmd' \\| at 23:30` or `systemd-run --on-active=10m cmd` | `schtasks /create /sc ONCE ...` |
| Parallel local jobs | `xargs -P 4`, `parallel -j 4`, `&` + `wait` | `xargs -P 4`, `parallel -j 4`, `&` + `wait` | `ForEach-Object -Parallel -ThrottleLimit 4` / `Start-Job` |
| Run container | `docker run --rm hello-world` / `podman run --rm hello-world` | `docker run --rm hello-world` / `podman run --rm hello-world` | `docker run --rm hello-world` |
| Local Kubernetes | `kind create cluster` / `minikube start` | `kind create cluster` / `minikube start` | Docker Desktop Kubernetes, `kind create cluster`, or `minikube start` |
| Check NVIDIA GPU | Not applicable on modern Macs | `nvidia-smi` | `nvidia-smi` |
| Check Apple Silicon GPU ML | PyTorch `torch.backends.mps.is_available()` | Not applicable | Not applicable |
| Disk usage | `df -h`; `du -sh *` | `df -h`; `du -sh *` | `Get-PSDrive`; `Get-ChildItem ... | Sort Length` |
| Resource limits | `ulimit -a` | `ulimit -a`; systemd unit resource controls | Job objects, service config, container limits, or app-level limits |

## Final workflow pattern

For a short experiment, run a foreground process and capture its output. For a long experiment, redirect logs, run it in the background, save its PID, and monitor CPU/memory. For an always-on workload, promote it to launchd, systemd, or a Windows Service/Task Scheduler entry. For a batch workload, use cron, systemd timers, launchd calendar triggers, or Task Scheduler. For packaging and portability, put the workload in a local container. For real horizontal scale, move from this guide to the earlier Docker/Kubernetes/cloud guides.
