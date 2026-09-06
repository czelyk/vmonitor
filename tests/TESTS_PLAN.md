# vmonitor Test Plan

## 1. Purpose

This document defines the integration and negative test plan for the vmonitor project.

The system under test consists of:

- `vmonitor.ko`
- `/dev/vmonitor`
- `monitor_server`
- `monitor_cli`

The goal is to verify that the kernel driver, TCP/UDP server, protocol handling, CLI, and error handling work together correctly.

---

## 2. Test Environment

### Components

- Linux kernel module: `vmonitor.ko`
- Character device: `/dev/vmonitor`
- TCP server: `monitor_server`
- TCP client: `monitor_cli`
- UDP telemetry listener: `monitor_cli --udp`

### Expected Ports

- TCP server: `5000`
- UDP telemetry: `5001`

---

## 3. Basic Build Tests

### TC-BUILD-01 — Build kernel module

**Steps**

1. Enter the kernel directory.
2. Run:

```bash
make clean
make