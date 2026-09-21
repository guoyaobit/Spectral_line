# Ansible cluster deployment

The playbook packages the source tree already present on the controller, sends
the same archive to every server, installs the non-DPDK build dependencies,
compiles the receiver, creates the monitor Python environment, and deploys both
components as systemd services. Only the monitor is enabled at boot; the
receiver is installed disabled for coordinated manual startup. The deployment
does not require the target servers to access GitHub.

Hosts execute independently with Ansible's `free` strategy. The default five
Ansible forks are sufficient for most clusters in this project's expected
range of fewer than ten servers; use `--forks 10` if all hosts should run at
once.

## Requirements

Controller:

- Ansible Core 2.13 or newer.
- GNU tar.
- SSH access to every target through a user that can use `sudo`.

Targets:

- Debian or Ubuntu with access to its configured apt repositories.
- A compatible NVIDIA driver and CUDA toolkit under `/usr/local/cuda`.
- A preinstalled DPDK development environment discoverable as `libdpdk` through
  `pkg-config`.
- Enough RAM and reserved huge-page capacity for the selected receiver setup.

The playbook installs Meson, Ninja, a C++ toolchain, and the remaining required
apt development libraries. DPDK and CUDA are intentionally not installed or
upgraded automatically because their versions depend on each host's NIC, GPU,
driver, and operating system.

## Configure the inventory

Copy the example and replace the documentation-only addresses and SSH user:

```sh
cp ansible/inventory.example.yml ansible/inventory.yml
ansible -i ansible/inventory.yml spectral_line_servers -m ping
```

The deployment variables are:

- `spectral_line_install_dir`: remote source directory; default
  `/opt/Spectral_line`.
- `spectral_line_build_dir`: Meson build directory; default `build`.
- `spectral_line_server_id`: per-host `ServerID` written to the deployed
  `config.yaml`; it must be an integer from `0` through `7`. If omitted, the
  playbook uses the trailing digits of the inventory hostname, so hosts named
  `GPU0` through `GPU7` require no additional setting.
- `spectral_line_gigabit_ip`: optional management-network IP shown in the
  generated summary; it defaults to `ansible_host`.
- `spectral_line_bmc_channel`: optional IPMI LAN channel used to discover the
  local BMC IP; it defaults to channel `1`.
- `spectral_line_100g_interfaces`: optional fallback list of 100G receiver
  ports. Each entry can contain `name` (or `pci`), `ip`, and `mac`. Normally the
  summary maps `mlx5_0` and `mlx5_1` to Linux interfaces with `ibdev2netdev`
  and reads their addresses automatically. Inventory values are used only when
  that discovery produces no interfaces.

The systemd units are rendered from Ansible templates, so overriding
`spectral_line_install_dir`, `spectral_line_build_dir`, or
`spectral_line_service_user` also updates their runtime paths and account.

Use SSH keys or Ansible Vault for credentials. Do not store SSH passwords,
private keys, sudo passwords, or GitHub tokens in the inventory file. For SSH
password authentication, prompt at runtime:

```sh
ansible-playbook -i ansible/inventory.yml ansible/deploy.yml \
  --ask-pass --ask-become-pass
```

## Deploy and compile

From the repository root on the controller, run:

```sh
ansible-playbook -i ansible/inventory.yml ansible/deploy.yml --forks 10
```

The deployment switches every receiver to `multi-user.target` and stops its
display manager, so GPU and CPU resources are not consumed by a desktop
session. SSH, networking, and background services remain available. Apply only
this host setting without rebuilding the receiver by running:

```sh
ansible-playbook -i ansible/inventory.yml ansible/disable-desktop.yml
```

To restore graphical startup later, set `graphical.target` as the default and
start `display-manager.service` on the affected hosts.

The controller creates a temporary archive of its current working tree. It
excludes `.git`, `build`, `.venv`, log files, Python caches, and the private inventory.
The archive is expanded into `/opt/Spectral_line` on each target, so local
controller changes are included even when they have not been pushed to GitHub.

If the services are already installed, the playbook stops them before replacing
source files and rebuilding. It then creates `/opt/Spectral_line/.venv`, installs
`monitor/requirements.txt`, and renders the systemd units. The monitor is
enabled and started, while the receiver remains disabled and stopped.

After the build, the default executable path on every server is
`/opt/Spectral_line/build/7mm`. `spectral-line.service` is not enabled at boot;
start it only after the cluster's NIC, huge-page, input, and output paths have
been validated. The playbook does not manage GRUB, kernel command-line options,
huge pages, IOMMU, NIC bindings, SR-IOV configuration, reboots, or
`config.yaml` values other than `ServerID`.

## Generate the cluster summary

Every successful deployment writes `ansible/deployment-summary.md` on the
controller. Separate Markdown tables contain server identity, management and
BMC addresses, 100G receiver interfaces, SR-IOV sender interfaces, and subband
frequency ranges. The sender interface IPv4/MAC values are read live from the
local interface named by `Sender_Nic`; `Storage_node_ip` and
`Storage_node_mac` are shown separately as configured destination values.

Refresh the summary without rebuilding or restarting the receiver:

```sh
ansible-playbook -i ansible/inventory.yml ansible/summary.yml
```

The file is generated operational data and is excluded from Git and deployment
source archives. If an SR-IOV address is shown as `unavailable`, verify that the
interface named by `Sender_Nic` exists and has an IPv4 address. If the 100G
interfaces are not detected, verify `ibdev2netdev` is installed and reports
both `mlx5_0` and `mlx5_1`, or provide the inventory fallback values. A BMC
value of `unavailable` means `ipmitool lan print` failed; verify IPMI device
access and set `spectral_line_bmc_channel` if the LAN interface is not channel
1.

Both 100G DPDK ports receive UDP destination ports `60000–60007`. On each
interface, `60000/60001` are the X/Y streams of one subband,
`60002/60003` are the next, followed by `60004/60005` and `60006/60007`.
DPDK port 0 maps these pairs to subbands 0–3; DPDK port 1 maps them to subbands
4–7. The `subbands[].port` values `60000–60007` in `config.yaml` are outgoing
result destination ports on `Storage_node_ip` and are unrelated to this fixed
input mapping despite using the same numbers.

## Control all receivers

Use the cluster control script from the repository root. It targets every host
in the `spectral_line_servers` inventory group and keeps the service disabled
at boot:

```sh
bash ansible/service-control.sh start
bash ansible/service-control.sh stop
bash ansible/service-control.sh restart
bash ansible/service-control.sh status
```

Pass a different inventory as the second argument when required:

```sh
bash ansible/service-control.sh status /path/to/inventory.yml
```

These commands change the current runtime state only; the receiver remains
disabled for automatic startup after a reboot. Systemd does not automatically
restart `7mm`; the process is designed to remain active until an explicit
`systemctl stop` (or the cluster-wide stop command above) terminates it.
