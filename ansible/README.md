# Ansible cluster deployment

The playbook packages the source tree already present on the controller, sends
the same archive to every server, installs DPDK and build dependencies, updates
GRUB for huge pages and IOMMU, compiles the receiver, and installs its systemd
units. It does not require the target servers to access GitHub.

Hosts execute independently with Ansible's `free` strategy. The default five
Ansible forks are sufficient for most clusters in this project's expected
range of fewer than ten servers; use `--forks 10` if all hosts should run at
once.

## Requirements

Controller:

- Ansible Core 2.14 or newer.
- GNU tar.
- SSH access to every target through a user that can use `sudo`.

Targets:

- Debian or Ubuntu with access to its configured apt repositories.
- A compatible NVIDIA driver and CUDA toolkit under `/usr/local/cuda`.
- Enough RAM and reserved huge-page capacity for the selected receiver setup.

The playbook installs DPDK, Meson, Ninja, a C++ toolchain, and the required apt
development libraries. CUDA is intentionally not installed automatically
because the correct driver/toolkit version depends on each GPU and host OS.

## Configure the inventory

Copy the example and replace the documentation-only addresses and SSH user:

```sh
cp ansible/inventory.example.yml ansible/inventory.yml
ansible -i ansible/inventory.yml spectral_line_servers -m ping
```

Review these group variables before deployment:

- `spectral_line_hugepage_size`: `1G` or `2M`; default `1G`.
- `spectral_line_hugepages`: boot-time huge-page count; default `16`.
- `spectral_line_iommu_vendor`: `intel` or `amd`; default `intel`.
- `spectral_line_install_dir`: remote source directory; default
  `/opt/Spectral_line`.
- `spectral_line_build_dir`: Meson build directory; default `build`.

The supplied service units use `/opt/Spectral_line`. If the installation
directory is overridden, update the paths in `systemd/*.service` as well.

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
excludes `.git`, `build`, log files, Python caches, and the private inventory.
The archive is expanded into `/opt/Spectral_line` on each target, so local
controller changes are included even when they have not been pushed to GitHub.

The playbook writes `/etc/default/grub.d/90-spectral-line.cfg` and runs
`update-grub` when that file changes. It does not reboot automatically. Reboot
every changed server before running the DPDK receiver so the huge-page and
IOMMU parameters take effect:

```sh
ansible -i ansible/inventory.yml spectral_line_servers \
  --become -m reboot
```

After the build, the default executable path on every server is
`/opt/Spectral_line/build/7mm`. The playbook installs and reloads
`spectral-line.service` and `spectral-line-monitor.service`, but does not enable
or start either service. It also does not change NIC bindings, SR-IOV
configuration, or `config.yaml` values.

After rebooting and checking the NIC and configuration, enable and start the
receiver on the cluster:

```sh
ansible -i ansible/inventory.yml spectral_line_servers --become \
  -m ansible.builtin.systemd_service \
  -a "name=spectral-line.service enabled=true state=started"
```

Use `state=stopped` to stop it on every server.
