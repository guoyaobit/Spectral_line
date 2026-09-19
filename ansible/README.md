# Ansible cluster deployment

The playbook packages the source tree already present on the controller, sends
the same archive to every server, installs the non-DPDK build dependencies,
compiles the receiver, creates the monitor Python environment, and deploys both
components as enabled systemd services. It does not require the target servers
to access GitHub.

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

The controller creates a temporary archive of its current working tree. It
excludes `.git`, `build`, `.venv`, log files, Python caches, and the private inventory.
The archive is expanded into `/opt/Spectral_line` on each target, so local
controller changes are included even when they have not been pushed to GitHub.

If the services are already installed, the playbook stops them before replacing
source files and rebuilding. It then creates `/opt/Spectral_line/.venv`, installs
`monitor/requirements.txt`, renders the systemd units, and enables both services.

After the build, the default executable path on every server is
`/opt/Spectral_line/build/7mm`, and the playbook starts both services. The
playbook does not manage GRUB, kernel command-line options, huge pages, IOMMU,
NIC bindings, SR-IOV configuration, reboots, or `config.yaml` values. Prepare
and validate those host settings before deployment.

If a receiver was intentionally stopped, start it on the cluster with:

```sh
ansible -i ansible/inventory.yml spectral_line_servers --become \
  -m systemd \
  -a "name=spectral-line.service state=started"
```

Use `state=stopped` to stop it on every server.
