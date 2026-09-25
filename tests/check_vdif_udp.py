#!/usr/bin/env python3
"""Receive FPGA VDIF-over-UDP packets and validate their 32-byte headers."""

import argparse
import datetime as dt
import socket
import struct
import sys
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from typing import Optional, Sequence, Tuple


VDIF_HEADER_BYTES = 32
VDIF_PAYLOAD_BYTES = 8192
DEFAULT_PACKET_BYTES = VDIF_HEADER_BYTES + VDIF_PAYLOAD_BYTES
DEFAULT_FRAMES_PER_SECOND = 62500
UTC = dt.timezone.utc

# UTC midnights immediately after positive leap seconds since VDIF epoch 0.
# This matches include/vdif.hpp. Update both lists after a new IERS Bulletin C.
LEAP_EFFECTIVE_UTC = (
    dt.datetime(2006, 1, 1, tzinfo=UTC),
    dt.datetime(2009, 1, 1, tzinfo=UTC),
    dt.datetime(2012, 7, 1, tzinfo=UTC),
    dt.datetime(2015, 7, 1, tzinfo=UTC),
    dt.datetime(2017, 1, 1, tzinfo=UTC),
)


@dataclass(frozen=True)
class VdifHeader:
    invalid: bool
    legacy: bool
    seconds_from_epoch: int
    reference_epoch: int
    frame_number: int
    version: int
    log2_channels: int
    frame_bytes: int
    station_id: int
    thread_id: int
    bits_per_sample: int
    complex_data: bool
    edv: int
    noise_source_on: bool

    def packet_id(self, frames_per_second: int) -> int:
        return self.seconds_from_epoch * frames_per_second + self.frame_number


def parse_vdif_header(packet: bytes) -> VdifHeader:
    if len(packet) < VDIF_HEADER_BYTES:
        raise ValueError(
            f"packet has {len(packet)} bytes; VDIF header needs "
            f"{VDIF_HEADER_BYTES}"
        )
    words = struct.unpack_from("<8I", packet)
    return VdifHeader(
        invalid=bool((words[0] >> 31) & 1),
        legacy=bool((words[0] >> 30) & 1),
        seconds_from_epoch=words[0] & 0x3FFFFFFF,
        reference_epoch=(words[1] >> 24) & 0x3F,
        frame_number=words[1] & 0x00FFFFFF,
        version=(words[2] >> 29) & 0x7,
        log2_channels=(words[2] >> 24) & 0x1F,
        frame_bytes=(words[2] & 0x00FFFFFF) * 8,
        station_id=words[3] & 0xFFFF,
        thread_id=(words[3] >> 16) & 0x03FF,
        bits_per_sample=((words[3] >> 26) & 0x1F) + 1,
        complex_data=bool((words[3] >> 31) & 1),
        edv=(words[4] >> 24) & 0xFF,
        noise_source_on=bool(words[7] & 1),
    )


def epoch_start(reference_epoch: int) -> dt.datetime:
    if not 0 <= reference_epoch <= 63:
        raise ValueError(f"invalid VDIF reference epoch {reference_epoch}")
    return dt.datetime(
        2000 + reference_epoch // 2,
        1 if reference_epoch % 2 == 0 else 7,
        1,
        tzinfo=UTC,
    )


def vdif_utc(
    reference_epoch: int,
    seconds_from_epoch: int,
    frame_number: int,
    frames_per_second: int,
) -> Tuple[dt.datetime, bool]:
    """Convert VDIF time to UTC; fold 23:59:60 onto 23:59:59."""
    start = epoch_start(reference_epoch)
    elapsed_leaps = 0
    leap_second = False
    for effective in LEAP_EFFECTIVE_UTC:
        if effective <= start:
            continue
        leap_vdif_second = int((effective - start).total_seconds()) + elapsed_leaps
        if seconds_from_epoch < leap_vdif_second:
            break
        if seconds_from_epoch == leap_vdif_second:
            timestamp = effective - dt.timedelta(seconds=1)
            leap_second = True
            break
        elapsed_leaps += 1
    else:
        timestamp = start + dt.timedelta(
            seconds=seconds_from_epoch - elapsed_leaps
        )
    if not leap_second:
        timestamp = start + dt.timedelta(
            seconds=seconds_from_epoch - elapsed_leaps
        )
    timestamp += dt.timedelta(seconds=frame_number / frames_per_second)
    return timestamp, leap_second


def format_utc(header: VdifHeader, frames_per_second: int) -> str:
    timestamp, leap_second = vdif_utc(
        header.reference_epoch,
        header.seconds_from_epoch,
        header.frame_number,
        frames_per_second,
    )
    text = timestamp.isoformat(timespec="microseconds").replace("+00:00", "Z")
    return text + (" [folded leap second]" if leap_second else "")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Capture FPGA VDIF-over-UDP packets, validate header fields, and "
            "report loss, duplicates, or reordering."
        )
    )
    parser.add_argument("count", nargs="?", type=int, help="packets to receive")
    parser.add_argument("--bind-ip", default="10.17.16.11")
    parser.add_argument(
        "--port", dest="single_ports", action="append", type=int,
        help="UDP port; repeat to monitor multiple ports",
    )
    parser.add_argument(
        "--ports", dest="port_specs", action="append", default=[],
        help="comma-separated ports or inclusive ranges, e.g. 60000-60007",
    )
    parser.add_argument(
        "--stream", type=int, default=0,
        help="starting display label; increments across multiple ports",
    )
    parser.add_argument(
        "--packet-bytes", type=int, default=DEFAULT_PACKET_BYTES
    )
    parser.add_argument(
        "--receive-buffer-bytes", type=int, default=16 * 1024 * 1024,
        help="requested SO_RCVBUF for each port",
    )
    parser.add_argument(
        "--frames-per-second", type=int, default=DEFAULT_FRAMES_PER_SECOND
    )
    parser.add_argument("--timeout", type=float, default=None)
    parser.add_argument("--expect-version", type=int)
    parser.add_argument("--expect-edv", type=int)
    parser.add_argument("--expect-thread-id", type=int)
    parser.add_argument(
        "--print-every", type=int, default=0,
        help="also print every Nth packet (zero disables)",
    )
    parser.add_argument(
        "--self-test", action="store_true", help="test the parser without UDP"
    )
    return parser


def self_test() -> int:
    words = [0] * 8
    words[0] = 1234
    words[1] = (53 << 24) | 5678
    words[2] = (1 << 29) | (DEFAULT_PACKET_BYTES // 8)
    words[3] = 0x4142 | (17 << 16) | (7 << 26) | (1 << 31)
    words[4] = 1 << 24
    words[7] = 1
    header = parse_vdif_header(struct.pack("<8I", *words))
    assert header.seconds_from_epoch == 1234
    assert header.reference_epoch == 53
    assert header.frame_number == 5678
    assert header.frame_bytes == DEFAULT_PACKET_BYTES
    assert header.thread_id == 17
    assert header.bits_per_sample == 8
    assert header.complex_data
    assert header.edv == 1
    assert header.noise_source_on
    assert header.packet_id(DEFAULT_FRAMES_PER_SECOND) == (
        1234 * DEFAULT_FRAMES_PER_SECOND + 5678
    )
    timestamp, leap_second = vdif_utc(53, 0, 0, DEFAULT_FRAMES_PER_SECOND)
    assert timestamp == dt.datetime(2026, 7, 1, tzinfo=UTC)
    assert not leap_second
    assert resolve_ports([60002], ["60000-60001,60003"]) == [
        60002, 60000, 60001, 60003
    ]
    print("VDIF UDP checker self-test passed")
    return 0


def resolve_ports(
    single_ports: Optional[Sequence[int]], port_specs: Sequence[str]
) -> Sequence[int]:
    ports = list(single_ports or [])
    for specification in port_specs:
        for item in specification.split(","):
            item = item.strip()
            if not item:
                raise ValueError("empty item in --ports")
            if "-" in item:
                start_text, end_text = item.split("-", 1)
                start = int(start_text)
                end = int(end_text)
                if end < start:
                    raise ValueError(f"descending UDP port range: {item}")
                ports.extend(range(start, end + 1))
            else:
                ports.append(int(item))
    if not ports:
        ports.append(60002)
    unique_ports = []
    for port in ports:
        if not 0 <= port <= 65535:
            raise ValueError(f"port must be between 0 and 65535: {port}")
        if port not in unique_ports:
            unique_ports.append(port)
    return unique_ports


def check_expected(
    header: VdifHeader, args: argparse.Namespace
) -> Sequence[str]:
    problems = []
    if header.frame_number >= args.frames_per_second:
        problems.append(
            f"frame {header.frame_number} is outside 0.."
            f"{args.frames_per_second - 1}"
        )
    if header.frame_bytes != args.packet_bytes:
        problems.append(
            f"header frame length {header.frame_bytes} != UDP payload "
            f"length {args.packet_bytes}"
        )
    if args.expect_version is not None and header.version != args.expect_version:
        problems.append(
            f"VDIF version {header.version} != {args.expect_version}"
        )
    if args.expect_edv is not None and header.edv != args.expect_edv:
        problems.append(f"EDV {header.edv} != {args.expect_edv}")
    if (
        args.expect_thread_id is not None
        and header.thread_id != args.expect_thread_id
    ):
        problems.append(
            f"thread ID {header.thread_id} != {args.expect_thread_id}"
        )
    return problems


def capture_port(args: argparse.Namespace, port: int, stream: int) -> int:
    prefix = f"[{args.bind_ip}:{port}] "

    def report(message: str, error: bool = False) -> None:
        print(
            prefix + message,
            file=sys.stderr if error else sys.stdout,
            flush=True,
        )

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.receive_buffer_bytes)
    if args.timeout is not None:
        sock.settimeout(args.timeout)
    sock.bind((args.bind_ip, port))

    report(
        f"VDIF stream {stream}: listening on "
        f"frames/s={args.frames_per_second}, "
        f"expected bytes={args.packet_bytes}, "
        f"SO_RCVBUF={sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)}"
    )

    received = 0
    invalid = 0
    bad_length = 0
    header_errors = 0
    missing = 0
    duplicates = 0
    reordered = 0
    previous_packet_id: Optional[int] = None
    first_header: Optional[VdifHeader] = None
    last_header: Optional[VdifHeader] = None
    second_start_packet: Optional[int] = None
    second_start_value: Optional[int] = None

    try:
        while received < args.count:
            packet, address = sock.recvfrom(65535)
            received += 1
            if len(packet) != args.packet_bytes:
                bad_length += 1
                report(
                    f"ERROR packet {received}: received {len(packet)} bytes "
                    f"from {address}, expected {args.packet_bytes}", True
                )
            try:
                header = parse_vdif_header(packet)
            except ValueError as error:
                header_errors += 1
                report(f"ERROR packet {received}: {error}", True)
                continue

            if first_header is None:
                first_header = header
                report(
                    "first packet: "
                    f"UTC={format_utc(header, args.frames_per_second)}, "
                    f"epoch={header.reference_epoch}, "
                    f"second={header.seconds_from_epoch}, "
                    f"frame={header.frame_number}, version={header.version}, "
                    f"EDV={header.edv}, thread={header.thread_id}, "
                    f"bits={header.bits_per_sample}, "
                    f"complex={int(header.complex_data)}, "
                    f"noise={'ON' if header.noise_source_on else 'OFF'}"
                )

            problems = check_expected(header, args)
            if problems:
                header_errors += len(problems)
                for problem in problems:
                    report(f"ERROR packet {received}: {problem}", True)
            if header.invalid:
                invalid += 1

            packet_id = header.packet_id(args.frames_per_second)
            if previous_packet_id is not None:
                difference = packet_id - previous_packet_id
                if difference > 1:
                    missing += difference - 1
                    report(
                        f"LOSS before packet {received}: missing "
                        f"{difference - 1} frame(s); received "
                        f"second={header.seconds_from_epoch} "
                        f"frame={header.frame_number}", True
                    )
                elif difference == 0:
                    duplicates += 1
                    report(
                        f"DUPLICATE packet {received}: packet_id={packet_id}",
                        True,
                    )
                elif difference < 0:
                    reordered += 1
                    report(
                        f"REORDERED packet {received}: packet_id={packet_id}, "
                        f"previous={previous_packet_id}", True
                    )
            previous_packet_id = packet_id
            last_header = header

            if header.frame_number in (0, args.frames_per_second - 1):
                boundary = "start" if header.frame_number == 0 else "end"
                report(
                    f"second {boundary}: UTC="
                    f"{format_utc(header, args.frames_per_second)}, "
                    f"packet={received}, frame={header.frame_number}"
                )
                if header.frame_number == 0:
                    second_start_packet = received
                    second_start_value = header.seconds_from_epoch
                elif (
                    second_start_packet is not None
                    and second_start_value == header.seconds_from_epoch
                ):
                    report(
                        "complete-second capture count: "
                        f"{received - second_start_packet + 1} "
                        f"(expected {args.frames_per_second})"
                    )
                    second_start_packet = None
                    second_start_value = None
            elif args.print_every and received % args.print_every == 0:
                report(
                    f"packet={received}, second={header.seconds_from_epoch}, "
                    f"frame={header.frame_number}, packet_id={packet_id}"
                )
    except socket.timeout:
        report("ERROR receive timeout", True)
        header_errors += 1
    except KeyboardInterrupt:
        report("Interrupted by user", True)
    finally:
        sock.close()

    if first_header is not None and last_header is not None:
        report(
            "range: "
            f"{format_utc(first_header, args.frames_per_second)} -> "
            f"{format_utc(last_header, args.frames_per_second)}"
        )
    report(
        "summary: "
        f"received={received}, missing={missing}, duplicates={duplicates}, "
        f"reordered={reordered}, invalid={invalid}, bad_length={bad_length}, "
        f"header_errors={header_errors}"
    )
    return int(
        any((missing, duplicates, reordered, invalid, bad_length, header_errors))
    )


def run_capture(args: argparse.Namespace) -> int:
    if args.count is None or args.count <= 0:
        raise ValueError("count must be a positive integer")
    if args.frames_per_second <= 0:
        raise ValueError("frames-per-second must be positive")
    if args.packet_bytes < VDIF_HEADER_BYTES:
        raise ValueError("packet-bytes is smaller than the VDIF header")
    if args.receive_buffer_bytes <= 0:
        raise ValueError("receive-buffer-bytes must be positive")

    ports = resolve_ports(args.single_ports, args.port_specs)
    if len(ports) == 1:
        return capture_port(args, ports[0], args.stream)
    print(
        f"Monitoring {len(ports)} UDP ports concurrently on {args.bind_ip}: "
        + ", ".join(str(port) for port in ports),
        flush=True,
    )
    work = [
        (args, port, args.stream + index) for index, port in enumerate(ports)
    ]
    with ProcessPoolExecutor(max_workers=len(ports)) as executor:
        results = list(executor.map(capture_port_task, work))
    return int(any(results))


def capture_port_task(item: Tuple[argparse.Namespace, int, int]) -> int:
    args, port, stream = item
    return capture_port(args, port, stream)


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    try:
        return run_capture(args)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    sys.exit(main())
